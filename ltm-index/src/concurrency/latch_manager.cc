#include "concurrency/latch_manager.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/log.h"

namespace ltm {

// ===========================================================================
// M6 并发清单
//   [x] C0（已被 C1 取代；代码保留）：LatchManager::global() 的 shared_mutex 包住整次
//           索引操作。**C1 落地后索引层已不再取它** —— `C0LatchGuard` 现在只有
//           tests/concurrency/test_c0_latch.cc 在用（验证守卫本身的可重入语义）。
//           历史：原判据"8 读线程线性扩展 >= 4x"实测不可达（约 1.3-1.6x，
//           report/m6_1/c0_read_scaling.csv），判据因此改为**正确性**。
//   [x] C1：页级 latch coupling —— **已落地（2026-10-04）**
//           [x] 页闩表 + 真实加锁（64 路元锁分片 + 每页一把 shared_mutex）
//           [x] LockOrderChecker：每线程持锁栈，判据 = level 单调不增 + 不重复/不升级
//               （原"同层 pid 不降"已删 —— pid 是分配序不是树序，写路径的兄弟交互
//                本来就要同时持有同层的一对页，实测 183 条假阳性）
//           [x] 帧表边界（缺陷 #46）进 `BufferPool::Impl::table_mutex`（见 report/c1/）
//           [x] 页闩随 `PageGuard` 走 + 写路径 `thread_local g_write_path` 标记
//               + `BufferPool::write_mutex`（D4 单写者，先锁后看深度）
//           [x] 撤 C0：索引层 11 个入口不再取 C0；读者只取共享页闩
//           [ ] 读者侧 crabbing：读者仍"一次只持一个页闩"（DescendToLeaf 取子页前
//               先放父页）。试过，两个实测结论见 report/c1/c1_landed.txt：它既不是
//               当时 Segfault 的解，又会让每读者钉住帧数翻倍（8 读者 × 树高把 16 帧
//               池子钉满 → kOutOfSpace）。
//           验收：30 分钟压测无死锁无挂起；ASAN 干净 + LockOrderChecker 零逆序
//                 （MSVC 无 TSAN，不能用"TSAN 干净"当判据，见 docs/architecture.md §6）
//   [ ] C2（可选）：乐观读 —— 页头版本号 + 失败重试
//           验收：读吞吐相对 C1 提升 >= 30%
// ===========================================================================

// ===========================================================================
// C1 · 页闩表
// ===========================================================================
struct LatchManager::Impl {
  explicit Impl(bool check) : enable_order_check(check) {}
  bool enable_order_check = false;
  Stats stats;

  // 【分片分的是元锁，不是页锁】方案原文是"按页号分片减少冲突"。按页号去分
  // **页锁**只会制造假冲突——两个无关的页落进同一片，其中一个要写就把另一个的
  // 读也挡住了。真正需要分的是保护表本身的元锁：查找/建项时拿它，拿到那个页
  // 自己的锁立刻放掉，绝不持着元锁去阻塞页锁（那会把元锁变成全局串行点，
  // 等于把 C1 退回 C0）。
  //
  // 【为什么值是 unique_ptr 而不是 shared_mutex】unordered_map 的节点在 rehash
  // 时不搬（引用稳定），但这条依赖容器实现细节，而"某线程正阻塞在这把锁上、
  // 锁的地址却被搬走"是 UB。用指针把地址钉死，不留疑问。
  //
  // 【表项只增不删】项数上界是"本进程碰过的不同页数"，受页文件大小约束。空闲页
  // 只是回空闲链，页号不会消失，所以这不是无界增长。
  static constexpr uint32_t kMetaShards = 64;
  struct Shard {
    std::mutex meta;
    std::unordered_map<PageId, std::unique_ptr<std::shared_mutex>> pages;
  };
  std::unique_ptr<Shard[]> shards = std::make_unique<Shard[]>(kMetaShards);

  std::shared_mutex* FindOrCreate(PageId pid) {
    Shard& s = shards[static_cast<uint32_t>(pid) % kMetaShards];
    std::lock_guard<std::mutex> g(s.meta);
    std::unique_ptr<std::shared_mutex>& slot = s.pages[pid];
    if (!slot) {
      slot = std::make_unique<std::shared_mutex>();
    }
    return slot.get();
  }
};

// ---------------------------------------------------------------------------
// 本线程的持锁栈（**按管理器分开**）
//
// 与 C0 的 g_c0 同一个理由：闩是"本线程此刻在树上走到哪"的属性，记在守卫对象上
// 传不过嵌套调用，所以放 thread_local。
//
// 【为什么不是一个栈槽】最初写的是"一个线程同一时刻只碰一个 LatchManager，
// 换管理器就重置"。这是错的，而且是测试的日志抓出来的：同一线程上第二个管理器
// 一出现，第一个管理器的持锁栈就被整段抹掉，它已加的闩再也解不开 —— 日志里是
// 两条 "released on a thread that never acquired it"，锁永久泄漏。
//
// 现在按管理器指针分栈。同一个线程同时开两个池（测试与工具里很自然）互不影响。
// 表通常只有 1 项，线性查找。
//
// 【注意】这与 C0 的 g_c0 形状不同，且 C0 那边**有同一个隐患**（换池就重置计数）。
// 本文件不改 C0：C1 落地后 C0 会被撤掉，且当前代码结构下一次索引操作只碰一个池，
// 触发不了。记录在 docs/status.md 的 C1 一节，不假装它不存在。
// ---------------------------------------------------------------------------
namespace {

struct ThreadPageLatches {
  struct Entry {
    const LatchManager* mgr = nullptr;
    std::vector<HeldPage> held;  // 自根向下；level 单调不增是要维持的不变量
  };
  std::vector<Entry> per_mgr;
};

thread_local ThreadPageLatches g_page;

// 取本线程在这个管理器上的持锁栈；没有就建一个空的。
// 返回的指针只在本次调用内使用 —— 下面的用法都不跨第二次 push_back。
std::vector<HeldPage>* StackFor(const LatchManager* mgr) {
  for (auto& e : g_page.per_mgr) {
    if (e.mgr == mgr) {
      return &e.held;
    }
  }
  g_page.per_mgr.push_back(ThreadPageLatches::Entry{});
  g_page.per_mgr.back().mgr = mgr;
  return &g_page.per_mgr.back().held;
}

}  // namespace

PageLatch::PageLatch(LatchManager* owner, PageId pid, LatchMode mode, uint8_t level, bool owns)
    : owner_(owner), pid_(pid), mode_(mode), level_(level), owns_(owns) {}

PageLatch::~PageLatch() { Release(); }

PageLatch::PageLatch(PageLatch&& other) noexcept
    : owner_(other.owner_),
      pid_(other.pid_),
      mode_(other.mode_),
      level_(other.level_),
      owns_(other.owns_) {
  other.owner_ = nullptr;
  other.owns_ = false;
  other.pid_ = kInvalidPageId;
}

PageLatch& PageLatch::operator=(PageLatch&& other) noexcept {
  if (this != &other) {
    Release();
    owner_ = other.owner_;
    pid_ = other.pid_;
    mode_ = other.mode_;
    level_ = other.level_;
    owns_ = other.owns_;
    other.owner_ = nullptr;
    other.owns_ = false;
    other.pid_ = kInvalidPageId;
  }
  return *this;
}

void PageLatch::Release() {
  if (owner_ != nullptr) {
    owner_->ReleaseHeld(pid_, mode_, owns_);
  }
  owner_ = nullptr;
  owns_ = false;
  pid_ = kInvalidPageId;
}

bool LatchManager::DefaultAbortOnViolation() {
#ifdef NDEBUG
  // Release / RelWithDebInfo：只计数、只记日志，不把线上进程炸掉。
  // 判据本身（stats.order_violations）仍然有效，所以这不削弱可证伪性。
  return false;
#else
  return true;
#endif
}

LatchManager::LatchManager(bool enable_order_check)
    : impl_(new Impl(enable_order_check)),
      abort_on_violation_(enable_order_check && DefaultAbortOnViolation()) {
  LOG_INFO("LatchManager constructed (M6-2/C1: real per-page latches): order_check=",
           enable_order_check ? "on" : "off",
           " abort_on_violation=", abort_on_violation_ ? "on" : "off");
}

LatchManager::~LatchManager() = default;

// ---------------------------------------------------------------------------
// 顺序检查。
//
// 【必须在阻塞之前跑】若先 lock 再检查，一次真实逆序会先死锁住，检查器永远
// 拿不到机会说话——那就成了"只在没出事的时候报平安"的检查器。
// ---------------------------------------------------------------------------
void LatchManager::CheckOrder(PageId pid, LatchMode mode, uint8_t level) {
  if (!impl_->enable_order_check) {
    return;
  }
  const std::vector<HeldPage>& held = *StackFor(this);

  const char* what = nullptr;

  // (1) 重复获取 / 升级。三种组合里只有两种合法，见头文件 PageLatch::owns()。
  for (const HeldPage& h : held) {
    if (h.pid != pid) {
      continue;
    }
    if (h.mode == LatchMode::kExclusive || mode == LatchMode::kShared) {
      // 【合法复用 → 直接返回，不参与顺序判定】本线程已按**不弱于**本次请求的模式
      // 持有该页，这次 Acquire 根本不会去加锁（owns()==false）。
      //
      // 【为什么必须在这里 return，而不是继续往下走】否则"持着父页 + 两个子页，
      // 再去取同一个父页"会被下面第 (2) 条判成"向上加锁"，而它一次锁都没加。
      // 这个形状在写路径里是常态：`RebalanceOnce` 持着父页，`BorrowLoop` 里
      // `SyncSeparatorForChild(parent_pid, ...)` 又取一次父页。
      // 实测：soak_mixed 在 C1 下报了 183 条逆序，绝大多数是这个形状 —— 检查器的
      // 假阳性，不是实现的锁序问题。
      return;
    }
    what =
        "latch upgrade: this thread already holds this page SHARED and now wants it EXCLUSIVE "
        "(a read path started a write)";
    break;
  }

  // (2) level 单调不增：拿着低层页再去拿高层页就是"向上加锁"。
  // 判据用 level 而不是 pid，理由见头文件顶部（pid 是分配序，不是树序）。
  if (what == nullptr && !held.empty() && level > held.back().level) {
    what =
        "latch order inversion: acquiring a HIGHER tree level while a lower one is still held "
        "(locking upward, toward the root)";
  }

  // (3) 【已删除：同层 pid 不降】
  //
  // 原判据是"同时持有的同层页，pid 必须按获取顺序不减"。它在 C1 下**不成立**，
  // 而且会误伤合法实现：写路径的兄弟交互（分裂产生右半页、借键、合并）本来就要
  // **同时**持有同层的一对页，而它们的 pid 大小关系是任意的 —— 右半页常常从空闲链
  // 拿到更小的 pid。实测：soak_mixed 在小池子下报了 183 条这个形状的"违规"，
  // 而 tree.Validate() 全绿、数据完全正确。
  //
  // 真正防死锁的不是这条，而是这套设计的两个结构事实：
  //   ① 单写者（D4：写操作由 BufferPool::write_mutex 串行化），写者之间不可能成环；
  //   ② **读者一次只持一个页闩**（DescendToLeaf 取子页前先放父页；叶链遍历读完一页
  //      先放闩再取下一页；R 树的栈式遍历每轮只持一页）。读者持页时不再请求别的页，
  //      所以"读者持 A 等 B、写者持 B 等 A"这个环构造不出来。
  // 保留的 (1)(2) 仍然能抓住两类真 bug：升级（读路径里发起写）与向上加锁。

  if (what == nullptr) {
    return;
  }

  ++impl_->stats.order_violations;
  std::string chain;
  for (const HeldPage& h : held) {
    chain += "    held: pid=" + std::to_string(h.pid) +
             " level=" + std::to_string(static_cast<int>(h.level)) +
             " mode=" + (h.mode == LatchMode::kExclusive ? "X" : "S") + "\n";
  }
  LOG_ERROR(what, "\n", chain, "    requested: pid=", pid,
            " level=", static_cast<int>(level),
            " mode=", (mode == LatchMode::kExclusive ? "X" : "S"),
            "\n    held stack depth=", held.size());
  if (abort_on_violation_) {
    std::abort();
  }
}

StatusOr<PageLatch> LatchManager::Acquire(PageId pid, LatchMode mode, uint8_t level) {
  CheckOrder(pid, mode, level);  // 先判后锁：逆序不能死在死锁里，见上面

  std::vector<HeldPage>* held = StackFor(this);

  if (mode == LatchMode::kExclusive) {
    ++impl_->stats.exclusive_acquisitions;
  } else {
    ++impl_->stats.shared_acquisitions;
  }

  // 本线程已持有该页：复用，不再去锁。CheckOrder 已经把"升级"判成违规了，
  // 走到这里说明现有模式不弱于本次请求。复用不压栈——那一格归当初的守卫。
  for (const HeldPage& h : *held) {
    if (h.pid == pid) {
      ++impl_->stats.reused_acquisitions;
      return PageLatch(this, pid, h.mode, h.level, /*owns=*/false);
    }
  }

  std::shared_mutex* m = impl_->FindOrCreate(pid);
  if (mode == LatchMode::kExclusive) {
    if (!m->try_lock()) {
      ++impl_->stats.contentions;
      m->lock();
    }
  } else {
    if (!m->try_lock_shared()) {
      ++impl_->stats.contentions;
      m->lock_shared();
    }
  }
  held->push_back(HeldPage{pid, mode, level});
  return PageLatch(this, pid, mode, level, /*owns=*/true);
}

void LatchManager::ReleaseHeld(PageId pid, LatchMode mode, bool owns) {
  std::vector<HeldPage>* held = StackFor(this);
  if (!owns) {
    return;  // 复用来的守卫：栈里那一格归当初真正加锁的那个
  }
  // 【不卡栈顶】crabbing 的模式是"拿住子页、放掉父页"，父页天然不是栈顶。
  // 强制 LIFO 会把 C1 自己要用的动作判成违规，理由与取舍见头文件顶部第 4 条。
  // 这里只查"确实持有"：查不到就是重复释放、释放了没持有的页，或者守卫被拿到
  // 别的线程去解 —— 三种都是真 bug（页闩有线程亲和，持锁栈是 thread_local）。
  // 不静静地什么都不做：静默会让锁一直不放，泄漏成后面某次莫名其妙的挂起。
  auto it = held->end();
  for (auto i = held->begin(); i != held->end(); ++i) {
    if (i->pid == pid) {
      it = i;
      break;
    }
  }
  if (it == held->end()) {
    ++impl_->stats.order_violations;
    LOG_ERROR("PageLatch released but not held by this thread: pid=", pid,
              " is not in this thread's stack for this manager (", held->size(),
              " entries). Double release, releasing a page this thread never latched, or a latch "
              "handed to another thread. Not unlocking anything.");
    return;
  }
  std::shared_mutex* m = impl_->FindOrCreate(pid);
  if (mode == LatchMode::kExclusive) {
    m->unlock();
  } else {
    m->unlock_shared();
  }
  held->erase(it);
}

std::vector<HeldPage> LatchManager::HeldStack() const {
  for (const ThreadPageLatches::Entry& e : g_page.per_mgr) {
    if (e.mgr == this) {
      return e.held;
    }
  }
  return {};
}

const LatchManager::Stats& LatchManager::stats() const { return impl_->stats; }

// ===========================================================================
// M6-1 · C0 全局闩守卫
//
// 本线程的持闩状态放在 thread_local：闩是"本线程正在用哪个缓冲池"的属性，
// 记在守卫对象上无法跨嵌套调用传递（HybridIndex::Put → BPlusTree::Insert 是
// 两个不同的守卫对象）。
// ===========================================================================
namespace {

struct ThreadLatchState {
  const std::shared_mutex* latch = nullptr;
  int shared_depth = 0;
  int exclusive_depth = 0;
};

// 一个线程在同一时刻只可能持有一个缓冲池的 C0 闩（本项目的调用结构如此：
// 一次索引操作只碰一个池）。换池时当作重新开始，不做跨池的嵌套计数。
thread_local ThreadLatchState g_c0;

}  // namespace

C0LatchGuard::C0LatchGuard(std::shared_mutex* latch, LatchMode mode)
    : latch_(latch), mode_(mode) {
  if (latch_ == nullptr) {
    return;  // 未 Open：空操作
  }
  if (g_c0.latch != latch_) {
    // 本线程还没碰过这个池（或刚换了池）：重新开始计数
    g_c0 = ThreadLatchState{};
    g_c0.latch = latch_;
  }
  if (g_c0.exclusive_depth > 0) {
    // 独占下再要任意模式：已有独占，什么都不用做。
    // 注意这里加的是**独占**计数 —— 哪怕这次要的是共享（counted_ 必须跟着记独占，
    // 否则析构时去减共享计数就漏了锁）。
    ++g_c0.exclusive_depth;
    counted_ = Counted::kExclusive;
    return;
  }
  if (mode == LatchMode::kShared) {
    if (g_c0.shared_depth == 0) {
      latch_->lock_shared();
      owns_ = true;
    }
    ++g_c0.shared_depth;
    counted_ = Counted::kShared;
    return;
  }
  // 要独占。若本线程已持共享，这就是升级。
  if (g_c0.shared_depth > 0) {
    LOG_ERROR("C0 latch upgrade attempted: this thread already holds the shared latch and now "
              "wants it exclusive. A read path must never start a write (nested upgrade cannot be "
              "satisfied without deadlock). Refusing to grant the exclusive latch.");
    // DEBUG 下立刻炸掉：这是设计违规，不该被"锁没生效"静静地掩盖过去。
    assert(false && "C0 latch upgrade (shared -> exclusive) is a design violation");
    return;  // Release 下不满足请求：让"锁没起作用"暴露出来，而不是死锁
  }
  latch_->lock();
  owns_ = true;
  g_c0.exclusive_depth = 1;
  counted_ = Counted::kExclusive;
}

C0LatchGuard::~C0LatchGuard() { Release(); }

C0LatchGuard::C0LatchGuard(C0LatchGuard&& other) noexcept
    : latch_(other.latch_), mode_(other.mode_), counted_(other.counted_), owns_(other.owns_) {
  other.latch_ = nullptr;
  other.owns_ = false;
  other.counted_ = Counted::kNone;
}

C0LatchGuard& C0LatchGuard::operator=(C0LatchGuard&& other) noexcept {
  if (this != &other) {
    Release();
    latch_ = other.latch_;
    mode_ = other.mode_;
    counted_ = other.counted_;
    owns_ = other.owns_;
    other.latch_ = nullptr;
    other.owns_ = false;
    other.counted_ = Counted::kNone;
  }
  return *this;
}

void C0LatchGuard::Release() {
  if (latch_ == nullptr) {
    return;
  }
  if (g_c0.latch != latch_) {
    // 守卫比计数活得久（正常不该发生）：计数已丢，绝不猜着解锁。
    latch_ = nullptr;
    owns_ = false;
    counted_ = Counted::kNone;
    return;
  }
  // 严格按构造函数实际加过的那个计数去还（见头文件里 Counted 的说明）
  if (counted_ == Counted::kExclusive) {
    if (g_c0.exclusive_depth > 0) {
      --g_c0.exclusive_depth;
      if (g_c0.exclusive_depth == 0 && owns_) {
        latch_->unlock();
      }
    }
  } else if (counted_ == Counted::kShared) {
    if (g_c0.shared_depth > 0) {
      --g_c0.shared_depth;
      if (g_c0.shared_depth == 0 && owns_) {
        latch_->unlock_shared();
      }
    }
  }
  if (g_c0.shared_depth == 0 && g_c0.exclusive_depth == 0) {
    g_c0.latch = nullptr;
  }
  latch_ = nullptr;
  owns_ = false;
  counted_ = Counted::kNone;
}

}  // namespace ltm

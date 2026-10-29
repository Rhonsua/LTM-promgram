// tests/index/test_wal_barrier.cc
// M4-2 写屏障的**证伪性**测试（方案阶段 3 的 ① 与 ②）。
//
// 【为什么这个文件必须存在】一个从不被调用的屏障、一条永远为真的断言，与一个
// 正确实现看起来完全一样 —— 本项目的缺陷 #37 就是这么来的（ASAN 开关在 MSVC 下
// 静默失效，于是 M3 的一条门槛从未真正执行过，却被记成"通过"）。所以本文件不检查
// "跑起来没报错"，它检查**机制本身能不能响、以及在什么条件下必须不响**。
//
// 三条用例构成一个闭环：
//   A 构造一个"被改过、却没有任何 REDO 覆盖"的页 → 屏障必须**报错并毒化池**
//     （同时证明 deltas_outside_txn 这个泄漏计数真的会跳）
//   B 在**同样的条件下**但正确地走事务 → 屏障必须**放行**，且写前日志规则真的执行
//     （flushed_lsn 被推过页头的 page_lsn：这是"真的同步了"与"恰好没触发"的分界）
//   C 真实负载跑完 → 0 泄漏，且把每个活页从**磁盘**读回来，页头都必须带着 LSN
//     （独立于缓冲池自己的计数：它是从盘上的字节看出来的，不是读计数器读出来的）
//
// A 与 B 只差一件事：页被标脏时是否处在写事务内。B 通过而 A 报错，说明被检验的
// 既不是"永远放行"也不是"永远报错"。C 再用第二个信息源封住"计数被人为清零"。
//
// 【为什么不用 FaultInjector】`LTM_ENABLE_FAULT_INJECT` 在 build-vs 与 build-asan
// 里**都是 OFF**，用它的测试会静默通过 —— 那正是 #37 的形状。这里改用公开 API
// 就能构造出的真实违约路径（见用例 A 的注释），不需要任何测试专用开关。
//
// 断言宏说明：本项目的 harness 只有 EXPECT_TRUE/FALSE/EQ/NE/GE/LE 与
// ASSERT_TRUE/EQ/NOT_NULL，**没有 GT/LT，也不支持 << 追加消息**。所以"为什么这条
// 断言重要"写在注释里，需要额外诊断文字的地方用 if + LTM_FAIL_ 直接写。
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "index/bplus_tree.h"
#include "index/tree_ops.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/meta_page.h"
#include "storage/page.h"
#include "storage/wal.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {

// 一个最小的真实环境：真磁盘文件 + 真 WAL + 真缓冲池 + 一棵真的 B+ 树。
struct Env {
  std::string page_file;
  std::string wal_file;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<WalWriter> wal;
  std::unique_ptr<BufferPool> pool;
  BPlusPageOps ops{32};
  BPlusTree tree;

  Env(const std::string& tag, uint32_t frames, uint16_t capacity = 32)
      : page_file("log/_barrier_" + tag + ".data"), wal_file("log/_barrier_" + tag + ".wal"),
        ops(capacity) {
#if defined(_WIN32)
    std::system("if not exist log mkdir log");
#else
    std::system("mkdir -p log");
#endif
    std::remove(page_file.c_str());
    std::remove(wal_file.c_str());

    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    dopt.num_pages = 1 << 14;
    disk.reset(new DiskManager(dopt));
    WalOptions wopt;
    wal.reset(new WalWriter(wopt, disk.get()));
    BufferPoolOptions popt;
    popt.pool_frames = frames;
    // 严格模式是默认值；这里显式写出来，因为它正是被测对象：
    // 它是"只计数"与"当场中止"的分界，测试要钉住的是"中止"这一半。
    popt.strict_i6_barrier = true;
    pool.reset(new BufferPool(popt, disk.get(), wal.get(), nullptr, nullptr, nullptr));

    TreeBuildOptions topt;
    topt.order_or_capacity = capacity;
    topt.caller_tree_slot = 0;
    const Status s = tree.Open(pool.get(), &ops, topt, /*tree_id=*/1);
    if (!s.ok()) {
      std::printf("Env open failed: %s\n", s.ToString().c_str());
    }
  }

  ~Env() {
    pool.reset();
    disk.reset();
    std::remove(page_file.c_str());
    std::remove(wal_file.c_str());
  }

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
};

std::vector<char> Payload(uint64_t key, size_t len) {
  std::vector<char> s(len, 'x');
  for (size_t i = 0; i < len; ++i) {
    s[i] = static_cast<char>('a' + ((key + i) % 26));
  }
  return s;
}

IndexEntry MakeEntry(uint64_t key) {
  IndexEntry e;
  e.key = key;
  e.payload = Payload(key, 24);
  return e;
}

// 从**磁盘**上读出这棵树的根页号（页 0 的元数据槽位），不经过缓冲池的内存镜像。
// 用例 C 要的就是这种独立性：如果连"哪些页是活的"都从缓冲池的计数器里取，
// 那这条检查就和它要检验的东西共用同一个失效点了。
StatusOr<PageId> RootFromDisk(DiskManager* disk, uint32_t slot) {
  std::vector<char> buf(disk->page_size(), 0);
  LTM_RETURN_IF_ERROR(disk->ReadPage(kMetaPageId, buf.data()));
  const MetaPage* mp = MetaOf(buf.data() + kPageHeaderSize);
  if (mp->magic != kMetaPageMagic) {
    return Status::Error(StatusCode::kCorruption, "the metadata page on disk has no magic");
  }
  if (slot >= kMaxTreeSlots || mp->trees[slot].in_use == 0) {
    return NotFound("the tree slot on disk is empty");
  }
  return mp->trees[slot].root;
}

// 把树里所有活页的页号收集出来 —— 同样只认磁盘上的字节。
StatusOr<std::vector<PageId>> CollectLivePagesFromDisk(DiskManager* disk, TreePageOps* ops,
                                                       PageId root) {
  std::vector<PageId> out;
  if (root == kInvalidPageId) {
    return out;
  }
  std::vector<PageId> stack{root};
  std::unordered_set<PageId> seen;
  std::vector<char> buf(disk->page_size(), 0);
  while (!stack.empty()) {
    const PageId pid = stack.back();
    stack.pop_back();
    if (pid == kInvalidPageId || !seen.insert(pid).second) {
      continue;
    }
    LTM_RETURN_IF_ERROR(disk->ReadPage(pid, buf.data()));
    Page page(buf.data());
    out.push_back(pid);
    if (page.header().level > 0) {
      for (uint16_t i = 0; i < page.num_slots(); ++i) {
        // 空槽（墓碑）必须跳过：GetChild 对空槽返回 kInvalidPageId，把它入栈会
        // 触发 Fetch(kInvalidPageId) 的自报家门断言（缺陷 #28 曾把一次正常的
        // 销毁过程报成损坏）。这里同样不能踩。
        if (ops->IsTombstone(page.header(), page.payload(), i)) {
          continue;
        }
        const PageId c = ops->GetChild(page.header(), page.payload(), i);
        if (c != kInvalidPageId) {
          stack.push_back(c);
        }
      }
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// A. 违约必须被抓住
// ---------------------------------------------------------------------------
// 定理：屏障若不能被触发，就等于不存在。这条用例构造出 I6 的违约条件，并断言
// 机制**确实报错**。
//
// 【违约条件是怎么真实构造出来的，而不是靠开关伪造的】M4-2 的已知边界是
// "页分配/回收不进日志"（方案 D10），于是公开 API 里就真的存在一条违约路径：
//   pool->NewPage(...)  在**任何写事务之外**建一个页 —— 它被 Init 成 page_lsn = 0，
//                       没有 delta、没有 LSN 盖章，但它**是脏的**，迟早要落盘。
//   pool->FlushPage()   把这个页写到盘上。
// 此时 WAL 里已经有记录（前面插入过），而这个页没有任何 REDO 覆盖它 —— 不变量
// I6 被违反。屏障必须在这里响。这条路径不需要任何测试专用开关，它是真实存在的。
TEST(WalBarrier, A_DirtyPageWithNoRedoCoveringItIsRefusedAndPoisonsThePool) {
  Env env("violate", /*frames=*/64);

  // 先制造"WAL 里已经有记录"这个前提：没有它，last_lsn == kInvalidLsn，
  // 屏障会把它当成建库引导而放行（那是唯一正当的例外）。
  for (uint64_t k = 0; k < 200; ++k) {
    ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
  }
  ASSERT_TRUE(env.pool->last_lsn() > 0u);
  EXPECT_EQ(env.pool->stats().i6_violations, 0u);

  // 违约构造：事务外建页 + 事务外标脏。
  PageId orphan = kInvalidPageId;
  {
    auto g = env.pool->NewPage(PageType::kBPlusLeaf, /*tree_id=*/1, /*level=*/0, &orphan);
    ASSERT_TRUE(g.ok());
    g.value().mutable_payload()[0] = 'Z';  // 只有"脏"才需要落盘，才受 I6 约束
    g.value().Release();
  }
  ASSERT_TRUE(orphan != kInvalidPageId);
  // 只断言"非零"，不钉具体条数：NewPage 除了新页本身还会动元数据页（空闲链/
  // 分配游标），所以实测是 2 条而不是 1 条。要证明的是"这个泄漏计数真的会跳"，
  // 不是"泄漏恰好有几条" —— 钉死条数只会让测试在无关的实现变动下变红。
  std::printf("  pages dirtied outside a transaction: %llu\n",
              static_cast<unsigned long long>(env.pool->stats().deltas_outside_txn));
  EXPECT_TRUE(env.pool->stats().deltas_outside_txn > 0u);
  if (env.pool->stats().deltas_outside_txn == 0u) {
    LTM_FAIL_("a page dirtied outside any transaction must show up in the leak counter");
  }

  // 这一页要被写盘了 —— 而它的 page_lsn 是 kInvalidLsn。
  const Status flushed = env.pool->FlushPage(orphan);
  if (flushed.ok()) {
    LTM_FAIL_("the barrier let a page reach disk with no REDO covering it (I6 violated silently)");
  }
  EXPECT_FALSE(flushed.ok());
  // 不能写 EXPECT_EQ(flushed.code(), StatusCode::kCorruption)：StatusCode 是
  // enum class，没有 operator<<，harness 的 Show() 会编译失败（C2679）。
  EXPECT_TRUE(flushed.code() == StatusCode::kCorruption);
  if (flushed.code() != StatusCode::kCorruption) {
    LTM_FAIL_(std::string("the barrier refused the page, but for the wrong reason: ") +
              StatusCodeName(flushed.code()) + " / " + flushed.message());
  }
  EXPECT_EQ(env.pool->stats().i6_violations, 1u);

  // 【毒化的意义】断言报警之后如果还继续写盘，内存里已经改了一半的页会被析构
  // 时的 FlushAll 写下去 —— 那比不报警更坏。所以池必须拒绝后续写盘。
  if (env.pool->FlushPage(orphan).ok()) {
    LTM_FAIL_("a poisoned pool must refuse further page writes");
  }
  if (env.pool->FlushAll().ok()) {
    LTM_FAIL_("a poisoned pool must refuse FlushAll, or the destructor writes half-done pages");
  }
}

// ---------------------------------------------------------------------------
// B. 同样的条件、正确地走事务 → 必须放行，且写前日志规则真的执行
// ---------------------------------------------------------------------------
// 与 A 的差别只有一处：这一页是在**写事务内**建并改的。于是它拿到了 delta 与
// LSN 盖章，屏障必须放行。
//
// 这条用例同时钉住"同步"这件事本身：在 Commit 之前把它写盘，页头的 page_lsn
// 一定大于 wal->flushed_lsn（本事务还没提交，谁都没同步过它），于是 FlushFrame
// 必须真的去 Sync，并把 flushed_lsn 推过那个 LSN。
//
// 【为什么这条断言比"wal_sync_on_flush > 0"强】只检查计数非零，无法区分"真的
// 同步了"与"根本没走那条分支、计数被别处加了"。这里直接读盘上页头的 page_lsn，
// 再与 wal->flushed_lsn() 比较 —— 后者只有在真的 fsync 之后才会前进。
TEST(WalBarrier, B_TheSamePageInsideATransactionIsAllowedAndForcesAWriteAheadSync) {
  Env env("allow", /*frames=*/64);

  for (uint64_t k = 0; k < 200; ++k) {
    ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
  }

  const uint64_t syncs_before = env.pool->stats().wal_sync_on_flush;
  const uint64_t violations_before = env.pool->stats().i6_violations;
  const Lsn flushed_before = env.wal->flushed_lsn();

  PageId pid = kInvalidPageId;
  ASSERT_TRUE(env.pool->BeginWriteOp().ok());
  {
    auto g = env.pool->NewPage(PageType::kBPlusLeaf, /*tree_id=*/1, /*level=*/0, &pid);
    ASSERT_TRUE(g.ok());
    g.value().mutable_payload()[0] = 'Z';
    g.value().Release();  // Unpin(dirty) -> 采集 delta -> 盖 LSN
  }
  ASSERT_TRUE(env.pool->stats().wal_deltas > 0u);
  if (env.pool->stats().wal_deltas == 0u) {
    LTM_FAIL_("the delta collector never ran: the barrier would have no input at all");
  }
  EXPECT_EQ(env.pool->stats().lsn_stamps_missed, 0u);

  // 事务还没提交，就把这一页写盘 —— 写前日志规则必须在这里生效。
  const Status mid_txn_flush = env.pool->FlushPage(pid);
  if (!mid_txn_flush.ok()) {
    LTM_FAIL_("a correctly logged page must NOT be refused; if it is, the barrier is unusable");
  }
  EXPECT_TRUE(mid_txn_flush.ok());
  EXPECT_EQ(env.pool->stats().i6_violations, violations_before);
  if (env.pool->stats().wal_sync_on_flush <= syncs_before) {
    LTM_FAIL_("the write-ahead rule did not fire although a page was flushed mid-transaction");
  }

  // 真正的证据：盘上这一页的 LSN，必须已经被日志的 flushed_lsn 覆盖。
  std::vector<char> buf(env.disk->page_size(), 0);
  ASSERT_TRUE(env.disk->ReadPage(pid, buf.data()).ok());
  const Page on_disk(buf.data());
  if (on_disk.header().page_lsn == kInvalidLsn) {
    LTM_FAIL_("the page reached disk with no LSN in its header");
  }
  if (env.wal->flushed_lsn() < on_disk.header().page_lsn) {
    LTM_FAIL_("the page is on disk but its REDO is not durable: the write-ahead rule was skipped");
  }
  EXPECT_TRUE(env.wal->flushed_lsn() > flushed_before);

  ASSERT_TRUE(env.pool->CommitWriteOp().ok());
}

// ---------------------------------------------------------------------------
// C. 真实负载下的完整性：0 泄漏 + 每个活页在盘上都带 LSN
// ---------------------------------------------------------------------------
// 【这条用例就是方案里的"delta 完整性测试"】它的判据不经过缓冲池自己的计数器，
// 而是把每个活页从**磁盘文件**读回来检查页头。因此它有两个独立的失败通道：
//   1) 任何一个采集点被拆掉（比如 Insert 忘了包事务）→ 页不会拿到 LSN 盖章 →
//      淘汰时被屏障当场中止（用例 A 已证明这个中止是真的）→ 负载直接失败；
//   2) 万一某条路径绕过了屏障 → 下面的逐页检查在盘上发现没盖章的活页 → 变红。
//
// 池子故意开得很小（16 帧 < 树的页数），逼出"事务中途淘汰脏页"这条最难的分支：
// 它既是写前日志规则唯一会被用到的场合，也是 delta 采集最容易漏的地方。
TEST(WalBarrier, C_ARealWorkloadLeavesEveryLivePageOnDiskCarryingAnLsn) {
  Env env("workload", /*frames=*/16);

  const uint64_t kInserts = 800;
  uint64_t erased = 0;
  for (uint64_t k = 0; k < kInserts; ++k) {
    ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    if (k % 3 == 0 && k > 50) {
      ASSERT_TRUE(env.tree.Erase(MakeEntry(k - 50)).ok());
      ++erased;
    }
  }
  ASSERT_TRUE(erased > 0u);

  const BufferPool::PoolStats& ps = env.pool->stats();
  EXPECT_TRUE(ps.wal_deltas > 0u);
  if (ps.deltas_missing_before != 0u) {
    LTM_FAIL_("a page was modified inside a transaction without ever having been pinned in it");
  }
  if (ps.deltas_outside_txn != 0u) {
    LTM_FAIL_("a page was dirtied outside every transaction: a collection point is missing");
  }
  if (ps.lsn_stamps_missed != 0u) {
    LTM_FAIL_("an LSN was produced but no frame was there to take it");
  }
  EXPECT_EQ(ps.lsn_stamps, ps.wal_deltas);
  EXPECT_EQ(ps.i6_unstamped_writes, 0u);
  EXPECT_EQ(ps.i6_violations, 0u);
  // 【这里为什么不要求 wal_sync_on_flush > 0】它只有在"页在**本事务**内被 Unpin
  // 之后、Commit 之前被淘汰"时才会触发；而 Commit 每次都无条件 Sync，所以跨过
  // Commit 边界的页一定有 page_lsn <= flushed_lsn。要让它触发，池子必须小到
  // **单次操作**（根+内部+叶+元数据页，约 4~5 帧）都放不下 —— 那已经小到
  // PrepareEviction 可能找不到可淘汰帧了。用一个依赖淘汰时序的断言去证明"写前
  // 规则在路径上"既脆弱又不如用例 B：B 是**故意构造**出这个条件并直接验证
  // flushed_lsn 被推过页头的 page_lsn。真实负载里的触发次数由验收程序给出
  // （B+ 3 次 / R 280 次，见 report/m4_2/phase2_wal_wiring.txt）。
  std::printf("  write-ahead syncs during this workload: %llu (0 is acceptable here; see the comment)\n",
              static_cast<unsigned long long>(ps.wal_sync_on_flush));

  // 全部落盘，然后只认磁盘上的字节。
  ASSERT_TRUE(env.pool->FlushAll().ok());
  auto root = RootFromDisk(env.disk.get(), /*slot=*/0);
  ASSERT_TRUE(root.ok());
  auto live = CollectLivePagesFromDisk(env.disk.get(), &env.ops, root.value());
  ASSERT_TRUE(live.ok());
  if (live.value().size() <= 3u) {
    LTM_FAIL_("the tree is too small for this test to mean anything");
  }

  std::vector<char> buf(env.disk->page_size(), 0);
  uint64_t unstamped = 0;
  for (PageId pid : live.value()) {
    ASSERT_TRUE(env.disk->ReadPage(pid, buf.data()).ok());
    const Page page(buf.data());
    if (page.header().page_lsn == kInvalidLsn) {
      ++unstamped;
      std::printf("  live page %u sits on disk with page_lsn == kInvalidLsn (no REDO covers it)\n",
                  static_cast<unsigned>(pid));
    }
  }
  EXPECT_EQ(unstamped, 0u);
  if (unstamped != 0u) {
    LTM_FAIL_("some live page reached disk without any REDO record covering it (I6 violated)");
  }

  // 数据本身也还得是对的 —— 否则"每个页都盖了章"可以靠"其实什么都没写"来满足。
  ASSERT_TRUE(env.tree.Validate().ok());
  EXPECT_EQ(env.tree.stats().num_entries, kInserts - erased);
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

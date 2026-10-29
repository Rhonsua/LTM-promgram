// tests/concurrency/test_page_latch.cc
// M6-2 · C1 页闩与 LockOrderChecker 的验收测试。
//
// ---------------------------------------------------------------------------
// 这个文件存在的原因
// ---------------------------------------------------------------------------
// C1 落地之前，LatchManager::Acquire 返回的是一个**不持锁**的守卫（骨架），
// CheckOrder 是个空函数。也就是说"页闩"这个名字在代码里成立了很久，实际
// 一个锁都没加过。这类东西最容易变成假绿：调用方按有锁去写，测试按有锁去断言，
// 而底下什么都没发生。
//
// 所以本文件的判据全部指向**能不能变红**：
//   * 互斥：独占期内别的线程必须进不来 —— 骨架实现下这条必然失败
//   * 顺序：真的逆序必须被计数并打出持锁链 —— 空实现下计数恒为 0
//   * 对照：正确的下钻（含 crabbing）必须一次都不报
//   * 反向对照：共享闩必须**真的允许**并发持有 —— 防"全都串行化"式的假通过
//   * 争用计数：同一页高并发必须真的记到争用，不同页必须为 0 —— 防计数器是摆设
//
// 每个用例自带 LatchManager：持锁栈是 thread_local，换管理器的指针会重置它，
// 用例之间互不污染。少数故意把栈留在脏状态的用例改用堆上泄漏的管理器（见
// CrossThreadReleaseIsCaught），保证"新管理器的地址"与脏栈记的地址永不相等。
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/latch_manager.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {

// 把独占/共享窗口拉长到调度器看得见的尺度。太小了（比如空循环）测试会退化成
// "运气好就过"，而这类测试一旦假绿，比没有还糟。
void HoldABit() { std::this_thread::sleep_for(std::chrono::microseconds(30)); }

uint64_t Violations(const LatchManager& m) {
  return m.stats().order_violations.load(std::memory_order_relaxed);
}

}  // namespace

// ---------------------------------------------------------------------------
// 用例 1：页闩真的互斥吗
//
// 两个原子量互相作证：
//   * 读者持共享闩时，writer_active 必须是 0
//   * 写者持独占闩时，readers 必须是 0
// 骨架实现（不持锁）下这两个断言必然被踩到，所以这条判据是可证伪的。
// ---------------------------------------------------------------------------
TEST(PageLatch, ExclusiveReallyExcludesShared) {
  LatchManager mgr(true);
  constexpr PageId kPage = 7;
  constexpr uint8_t kLevel = 2;
  constexpr int kWriterRounds = 120;
  constexpr int kReaderThreads = 4;
  constexpr int kReaderRounds = 150;

  std::atomic<int> readers{0};
  std::atomic<int> writer_active{0};
  std::atomic<int> violations{0};
  std::atomic<int> peak_readers{0};
  std::atomic<bool> go{false};

  std::thread writer([&] {
    while (!go.load()) {
    }
    for (int i = 0; i < kWriterRounds; ++i) {
      auto l = mgr.Acquire(kPage, LatchMode::kExclusive, kLevel);
      if (readers.load() != 0) {
        ++violations;  // 独占期内还有读者 —— 闩没起作用
      }
      writer_active.store(1);
      HoldABit();
      writer_active.store(0);
    }
  });

  std::vector<std::thread> readers_v;
  for (int t = 0; t < kReaderThreads; ++t) {
    readers_v.emplace_back([&] {
      while (!go.load()) {
      }
      for (int i = 0; i < kReaderRounds; ++i) {
        auto l = mgr.Acquire(kPage, LatchMode::kShared, kLevel);
        if (writer_active.load() != 0) {
          ++violations;  // 共享闩拿到了，但写者还在独占期内
        }
        const int now = readers.fetch_add(1) + 1;
        int seen = peak_readers.load();
        while (now > seen && !peak_readers.compare_exchange_weak(seen, now)) {
        }
        HoldABit();
        readers.fetch_sub(1);
      }
    });
  }

  go.store(true);
  writer.join();
  for (auto& r : readers_v) {
    r.join();
  }

  EXPECT_EQ(violations.load(), 0);
  // 反向对照：如果实现把所有请求都当独占处理，violations 会是 0，但这里会塌到 1。
  EXPECT_GE(peak_readers.load(), 2);

  const auto& s = mgr.stats();
  std::printf("  [info] exclusive/shared interleave: %d writer rounds, %d reader threads x %d; "
              "violations=%d peak_concurrent_readers=%d contentions=%llu\n",
              kWriterRounds, kReaderThreads, kReaderRounds, violations.load(), peak_readers.load(),
              static_cast<unsigned long long>(s.contentions.load()));
}

// ---------------------------------------------------------------------------
// 用例 2：顺序检查器抓得住"向上加锁"（level 逆序）
// ---------------------------------------------------------------------------
TEST(PageLatch, LockOrderCheckerDetectsInversion) {
  LatchManager mgr(true);
  mgr.set_abort_on_violation(false);  // 要观测，不想被 abort 打断
  EXPECT_FALSE(mgr.abort_on_violation());
  EXPECT_EQ(Violations(mgr), 0u);

  // 正常下钻：根(level 3) → 叶(level 1)
  auto root = mgr.Acquire(100, LatchMode::kShared, 3);
  ASSERT_TRUE(root.ok());
  auto leaf = mgr.Acquire(200, LatchMode::kShared, 1);
  ASSERT_TRUE(leaf.ok());
  EXPECT_EQ(Violations(mgr), 0u);

  // 逆序：手里还攥着 level 1，去要 level 2 —— 这是"从叶向根向上加锁"
  auto upward = mgr.Acquire(300, LatchMode::kShared, 2);
  ASSERT_TRUE(upward.ok());
  EXPECT_EQ(Violations(mgr), 1u);
  std::printf("  [info] level inversion (held L1, asked L2) -> order_violations=%llu\n",
              static_cast<unsigned long long>(Violations(mgr)));

  const std::vector<HeldPage> stack = mgr.HeldStack();
  EXPECT_EQ(stack.size(), static_cast<size_t>(3));
}

// ---------------------------------------------------------------------------
// 用例 3：【原"抓兄弟右向左"已删除】同层 pid 序在 C1 下不成立，不能当判据
//
// 原判据是"同时持有的同层页，pid 必须按获取顺序不减"。C1 落地时实测它**误伤
// 合法实现**：写路径的兄弟交互（分裂产生右半页、借键、合并）本来就要同时持有
// 同层的一对页，而右半页常常从空闲链拿到更小的 pid。soak_mixed 在小池子下报了
// 183 条这个形状的"违规"，而 tree.Validate() 全绿、数据完全正确。
//
// 所以这一条被删掉了（理由与替代保证见 latch_manager.h 顶部）。本用例反过来钉住
// 删除后的行为：右向左**不再报**；而真正会成环的"向上加锁"仍由用例 2 抓。
// 防死锁靠的是另外两个结构事实：单写者（D4）+ 读者一次只持一个页闩。
// ---------------------------------------------------------------------------
TEST(PageLatch, SiblingPidOrderIsNotAViolation) {
  LatchManager mgr(true);
  mgr.set_abort_on_violation(false);
  EXPECT_EQ(Violations(mgr), 0u);

  // 先大 pid 再小 pid（同层）：C1 下这是**合法**的（右半页可能 pid 更小）
  auto right = mgr.Acquire(900, LatchMode::kExclusive, 1);
  auto left = mgr.Acquire(500, LatchMode::kExclusive, 1);
  ASSERT_TRUE(right.ok());
  ASSERT_TRUE(left.ok());
  EXPECT_EQ(Violations(mgr), 0u);  // 不再报 —— 这是有意的偏离，不是回归

  // 对照：向上加锁**仍然**报（用例 2 已单独覆盖，这里再钉一次，防止删除时误伤）
  LatchManager up(true);
  up.set_abort_on_violation(false);
  auto leaf = up.Acquire(10, LatchMode::kShared, 0);
  auto root = up.Acquire(20, LatchMode::kShared, 2);  // 0 -> 2 是向上加锁
  ASSERT_TRUE(leaf.ok());
  ASSERT_TRUE(root.ok());
  EXPECT_EQ(Violations(up), 1u);
  std::printf("  [info] sibling right-to-left=%llu (removed rule), upward=%llu (still caught)\n",
              static_cast<unsigned long long>(Violations(mgr)),
              static_cast<unsigned long long>(Violations(up)));
}

// ---------------------------------------------------------------------------
// 用例 4：正确的下钻一次都不该报 —— 含 crabbing 的"放非栈顶父页"
//
// 这条是负向对照。没有它，用例 2/3 会被一个"见谁都报"的实现蒙过去。
//
// 同时它把方案 §6 那句"释放顺序必须与获取顺序相反"的取舍钉在这里：
// crabbing 就是先拿子页再放父页，放的时候父页**不是栈顶**。若真按"反序"卡死，
// 下面这一段会直接违规 —— 而它正是 C1 自己要用的模式。
// ---------------------------------------------------------------------------
TEST(PageLatch, CorrectDescentIsSilentIncludingCrabbing) {
  LatchManager mgr(true);
  mgr.set_abort_on_violation(false);

  // (a) 最朴素的下钻：一次只持一个，释放顺序其实与获取顺序**相同**
  {
    auto root = mgr.Acquire(10, LatchMode::kShared, 3);
    ASSERT_TRUE(root.ok());
    root.value().Release();
    auto mid = mgr.Acquire(40, LatchMode::kShared, 2);
    ASSERT_TRUE(mid.ok());
    mid.value().Release();
    auto leaf = mgr.Acquire(70, LatchMode::kShared, 0);
    ASSERT_TRUE(leaf.ok());
    leaf.value().Release();
    EXPECT_EQ(mgr.HeldStack().size(), static_cast<size_t>(0));
  }
  EXPECT_EQ(Violations(mgr), 0u);

  // (b) crabbing：父页在被持着子页的时候放掉 —— 非栈顶释放
  {
    auto parent = mgr.Acquire(11, LatchMode::kExclusive, 3);
    ASSERT_TRUE(parent.ok());
    auto child = mgr.Acquire(80, LatchMode::kExclusive, 0);
    ASSERT_TRUE(child.ok());
    EXPECT_EQ(mgr.HeldStack().size(), static_cast<size_t>(2));
    parent.value().Release();  // 栈里是 [3,0]，放掉 3 这一格
    EXPECT_EQ(mgr.HeldStack().size(), static_cast<size_t>(1));
    EXPECT_EQ(Violations(mgr), 0u);
  }
  EXPECT_EQ(Violations(mgr), 0u);

  // (c) 同层左→右：兄弟交互的合法形态
  {
    auto l = mgr.Acquire(300, LatchMode::kShared, 1);
    ASSERT_TRUE(l.ok());
    auto r = mgr.Acquire(310, LatchMode::kShared, 1);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(Violations(mgr), 0u);
  }
  EXPECT_EQ(Violations(mgr), 0u);
  std::printf("  [info] correct descent (plain + crabbing + left-to-right siblings): "
              "order_violations=%llu\n",
              static_cast<unsigned long long>(Violations(mgr)));
}

// ---------------------------------------------------------------------------
// 用例 5：本线程已持有的页再来一次 —— 复用而不是自死锁
//
// shared_mutex 不可重入。树的调用是嵌套的（HybridIndex::Put → BPlusTree::Insert
// 两处都要叶闩），若第二次真的去 lock，表现是**测试挂住**，最容易被误当成"跑得慢"。
// ---------------------------------------------------------------------------
TEST(PageLatch, ReusedAcquisitionDoesNotRelatch) {
  LatchManager mgr(true);
  mgr.set_abort_on_violation(false);
  const uint64_t reused_before = mgr.stats().reused_acquisitions.load();

  auto outer = mgr.Acquire(55, LatchMode::kShared, 1);
  ASSERT_TRUE(outer.ok());
  EXPECT_TRUE(outer.value().owns());

  auto inner = mgr.Acquire(55, LatchMode::kShared, 1);  // 同一页、同样共享
  ASSERT_TRUE(inner.ok());
  EXPECT_FALSE(inner.value().owns());  // 复用：没有真的加锁
  EXPECT_EQ(Violations(mgr), 0u);
  EXPECT_EQ(mgr.HeldStack().size(), static_cast<size_t>(1));  // 只压了一格

  EXPECT_EQ(mgr.stats().reused_acquisitions.load(), reused_before + 1);
  inner.value().Release();
  EXPECT_EQ(mgr.HeldStack().size(), static_cast<size_t>(1));  // 复用者不弹栈
  outer.value().Release();
  EXPECT_EQ(mgr.HeldStack().size(), static_cast<size_t>(0));

  // 已持共享再要独占 = 升级 = 设计违规（读路径内部发起了写）
  auto shared_only = mgr.Acquire(56, LatchMode::kShared, 1);
  ASSERT_TRUE(shared_only.ok());
  auto upgrade = mgr.Acquire(56, LatchMode::kExclusive, 1);
  EXPECT_EQ(Violations(mgr), 1u);
  std::printf("  [info] reuse counted, upgrade rejected: order_violations=%llu reused=%llu\n",
              static_cast<unsigned long long>(Violations(mgr)),
              static_cast<unsigned long long>(mgr.stats().reused_acquisitions.load()));
}

// ---------------------------------------------------------------------------
// 用例 6：跨线程释放
//
// 持锁栈是 thread_local，所以页闩有线程亲和：谁加的谁解。拿到别的线程去解，
// 那边根本没这条记录 —— 必须报出来，而不是静静地什么都不做（静默会让锁泄漏到
// 后面某个用例里，变成一次莫名其妙的挂起）。
//
// 本用例故意把管理器泄漏在堆上：脏栈里记着这个地址，只要它一直被占着，后面
// 用例新建的管理器地址就绝不会撞上它，thread_local 必然重置 —— 用例之间不串味。
// ---------------------------------------------------------------------------
TEST(PageLatch, CrossThreadReleaseIsCaught) {
  LatchManager* mgr = new LatchManager(true);  // 故意泄漏，见文件头与上面注释
  mgr->set_abort_on_violation(false);

  auto* holder = new StatusOr<PageLatch>(mgr->Acquire(31, LatchMode::kExclusive, 1));
  ASSERT_TRUE(holder->ok());
  EXPECT_TRUE(holder->value().owns());
  const uint64_t before = Violations(*mgr);

  std::thread other([&] { holder->value().Release(); });
  other.join();

  EXPECT_EQ(Violations(*mgr), before + 1);
  // 本线程的持锁栈没被动过：那一格还在（说明没有跨线程解开锁）
  EXPECT_EQ(mgr->HeldStack().size(), static_cast<size_t>(1));
  std::printf("  [info] cross-thread release caught: order_violations=%llu, lock still held=%s\n",
              static_cast<unsigned long long>(Violations(*mgr)),
              mgr->HeldStack().size() == 1 ? "yes" : "no");
  // holder / mgr 不删：本线程的持锁栈指着这个地址，留着才确定
}

// ---------------------------------------------------------------------------
// 用例 7：争用计数是真的
//
// 同一页被多线程猛敲，contentions 必须真的涨；各敲各的页，必须是 0。
// 计数器如果是摆设，这条会失败 —— 这是"数字只能来自程序输出"那条铁律的自检。
// ---------------------------------------------------------------------------
TEST(PageLatch, ContentionCounterIsReal) {
  constexpr int kThreads = 8;
  constexpr int kRounds = 2000;

  auto hammer = [&](bool same_page) -> uint64_t {
    LatchManager mgr(true);
    const uint64_t before = mgr.stats().contentions.load();
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
      ts.emplace_back([&mgr, &go, t, same_page] {
        while (!go.load()) {
        }
        const PageId pid = same_page ? 777 : static_cast<PageId>(1000 + t);
        for (int i = 0; i < kRounds; ++i) {
          auto l = mgr.Acquire(pid, LatchMode::kExclusive, 1);
        }
      });
    }
    go.store(true);
    for (auto& th : ts) {
      th.join();
    }
    return mgr.stats().contentions.load() - before;
  };

  const uint64_t shared_page = hammer(true);
  const uint64_t private_pages = hammer(false);
  EXPECT_TRUE(shared_page > 0);
  EXPECT_EQ(private_pages, 0u);
  std::printf("  [info] contentions: same page=%llu, per-thread pages=%llu\n",
              static_cast<unsigned long long>(shared_page),
              static_cast<unsigned long long>(private_pages));
}

// ---------------------------------------------------------------------------
// 用例 8：默认值
//
// abort_on_violation 必须跟 enable_order_check 走：关掉检查就不该配一个会炸的
// 开关。同时确认 DEBUG 默认值这条函数在两种构建下都自洽。
// ---------------------------------------------------------------------------
TEST(PageLatch, AbortDefaultFollowsOrderCheck) {
  LatchManager off(false);
  EXPECT_FALSE(off.abort_on_violation());
  LatchManager on(true);
  EXPECT_EQ(on.abort_on_violation(), LatchManager::DefaultAbortOnViolation());
  // 注意：字面量必须是纯 ASCII（控制台是 GBK），中文只出现在注释与 docs 里
  std::printf("  [info] DefaultAbortOnViolation()=%s (expected false in RelWithDebInfo)\n",
              LatchManager::DefaultAbortOnViolation() ? "true" : "false");
}

int main(int argc, char** argv) {
  // Run in a child process: use the build's default, never force abort on.
  if (argc == 2 && std::string(argv[1]) == "--probe-order-abort") {
#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    LatchManager mgr(true);
    if (!mgr.abort_on_violation()) {
      std::fprintf(stderr, "ORDER_ABORT_PROBE: requires Debug default\n");
      return 2;
    }
    auto leaf = mgr.Acquire(10, LatchMode::kShared, 0);
    if (!leaf.ok()) return 4;
    std::fprintf(stderr, "ORDER_ABORT_PROBE: held=10/L0 requested=20/L2\n");
    std::fflush(stderr);
    auto root = mgr.Acquire(20, LatchMode::kShared, 2);
    LTM_UNUSED(root);
    std::fprintf(stderr, "ORDER_ABORT_PROBE: unexpectedly survived\n");
    return 5;
  }
  return ltm::test::RunAll(argc, argv);
}

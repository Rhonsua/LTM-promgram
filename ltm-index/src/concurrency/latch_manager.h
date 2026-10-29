// src/concurrency/latch_manager.h
// 并发模型（方案 §4.10）分三阶段推进；本文件提供统一的锁入口，
// 使 C0 → C1 → C2 的切换只改这一个文件，不动算子层。
//
//   C0（必达）全局 latch：读共享 / 写独占 —— 多读单写
//   C1（应有）页级 latch coupling：树内自根向下，恒定向下的加锁序
//   C2（可选）乐观读：版本号 + 重试
//
// 硬约束：
//   * 加锁顺序恒定：根 → 叶；兄弟页交互恒定先左后右（禁止向上加锁）
//   * DEBUG 构建下启用 LockOrderChecker：发现逆序立即 abort 并打印持锁链
//
// ---------------------------------------------------------------------------
// C1 · LockOrderChecker 实际检查的两件事（以及它检查不到什么）
//
// 【判据用 level，不用 pid 正序】方案 §6 的原话是"发现 pid 逆序即 abort"。
// pid 是**分配顺序**，不是树序：一次分裂里新分配的右兄弟会从空闲链上拿到
// 更小的 pid，于是"正确的先左后右"在 pid 上是递减的。拿 pid 当序，会把正确
// 的实现判成违规，而真正的逆序（拿着叶再去拿根）反而可能看起来递增。
// 所以权威判据是 level；pid 序在 C1 里被彻底删掉了（理由见下面"删掉的第 4 条"）：
//
//   1. level 单调不增 —— 本线程持锁栈自根向下，level 必须一路不增。
//      拿着 level=L 的页再去拿 level>L 的页，就是"向上加锁"，直接违规。
//      这条覆盖了方案要的"禁止向上加锁"。
//   2. 不重复获取 / 不升级 —— 同一页不得二次加锁；已持共享再要独占是升级，
//      同 C0 的判断（读路径内部发起了写）。都不可能在正确实现里出现。
//      **合法复用（已持不弱于本次请求的模式）直接返回，不参与下面的顺序判定** ——
//      否则"持着父页 + 两个子页、再去取同一个父页"会被判成向上加锁，而它一次锁
//      都没加。这个形状在写路径里是常态（RebalanceOnce 持父页、BorrowLoop 里又
//      取一次父页），实测报了 183 条假阳性。
//   3. 释放必须命中本线程当前持有的某一格。**允许非栈顶**，理由见下。
//
// 【C1 删掉的第 4 条：同层 pid 不降】原判据是"同时持有的同层页，pid 必须按获取
// 顺序不减"，用来近似"兄弟交互恒先左后右"。它在 C1 下**不成立**：写路径的兄弟
// 交互（分裂产生右半页、借键、合并）本来就要**同时**持有同层的一对页，而它们的
// pid 大小关系是任意的 —— 右半页常常从空闲链拿到更小的 pid。pid 是分配序，不是
// 树序。实测：soak_mixed 在小池子下报 183 条这个形状的"违规"，而 tree.Validate()
// 全绿、数据完全正确。
//
// 本实现依赖下面的结构约束；跨线程是否形成锁环仍须独立检测和压力验证：
//   ① 单写者（D4：写操作由 BufferPool::write_mutex 串行化），写者之间不可能成环；
//   ② 读者下钻自根向下耦合，拿到子页闩后释放父页闩；叶链遍历先放当前页再取下一页，
//      R 树的栈式遍历每轮只持一页。读者不带着同层页闩等待另一个同层页。
// 检查器因此弱于"任意逆序都能抓"，但它抓住的是这套设计里真正会成环的两类形状。
// 这条偏离写在 docs/status.md 的 C1 一节，不假装是完备检查。
//
// 【它检查不到什么】跨线程的锁序环。本检查器是**每线程**的持锁栈，两个线程
// 各自合法、合起来成环它看不见。Windows MSVC 无 TSAN；2026-10-07 已接入 Linux
// Clang TSAN 验收，运行库对照、检测结果与范围见 report/c1/tsan/README.md。
// 锁序检查、ASAN 和压力不能替代 TSAN，有限检测也不能证明任意调度均安全。
//
// 【为什么不强制"释放顺序 = 获取顺序的反序"】方案 §6 写的是"下钻时先拿子页
// 再放父页；释放顺序必须与获取顺序相反"。这两句不能同时成立，而且第二句恰好
// 禁止了 latch crabbing 本身：
//
//     拿父 P(level 3) → 拿子 C(level 0) → 放父 P → 继续用 C
//
// 放 P 的时候 C 在栈顶，P 不是栈顶。若按"必须反序"卡死，这个动作直接违规——
// 而它正是 C1 要靠的模式。反过来，最朴素的"锁-读-放-再锁下一层"（一次只持一个）
// 也是合法的，它的释放顺序是**递增**的。两种都对，所以释放侧不存在一条统一的
// 顺序约束。
//
// 真正的危险是死锁，而死锁只由**获取**顺序决定，所以严格约束全部压在 acquire 侧
// （第 1、2 条）。释放侧只保留"确实持有"这一条——它能抓住重复释放和释放
// 没持有的页这两种真 bug，又不会误伤合法模式。
// 这条偏离写在 docs/status.md 的 C1 一节与 report/c1/README.md，不是悄悄改的。
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "util/status.h"
#include "util/types.h"

namespace ltm {

class LatchManager;

// 取闩模式。
//
// 【kNone 的用途】缓冲池自己的内部取页（页 0、空闲链页、新页）**不**走页闩：
//   * 页 0 的 level 是 1（见 LoadMeta），而它会在写路径持着叶闩（level 0）时被取用，
//     取闩就成了"向上加锁"，必然报逆序；而页 0 的并发访问由写串行化 + 内存镜像保护。
//   * 空闲链页与新页不在任何树的键空间里，读者到不了，不需要闩。
// 索引层的取页一律用 kShared / kExclusive。
enum class LatchMode : uint8_t { kShared = 0, kExclusive = 1, kNone = 2 };

// 本线程持锁栈上的一格。level 是页在树里的层级（叶 0，根最大），
// 由调用方从 PageHeader::level 取——闩管理器自己不读页。
struct HeldPage {
  PageId pid = kInvalidPageId;
  LatchMode mode = LatchMode::kShared;
  uint8_t level = 0;
};

class PageLatch {
 public:
  // 【默认构造 = 空守卫（None）】必须公开：PageGuard 把它作为成员，而 PageGuard
  // 默认构造是很多容器/StatusOr 的前提（StatusOr<PageGuard> 会默认构造一个 T）。
  // 空守卫不解锁、不计数，与 PageLatch::None() 完全等价。
  PageLatch() = default;
  PageLatch(PageLatch&& other) noexcept;
  PageLatch& operator=(PageLatch&& other) noexcept;
  PageLatch(const PageLatch&) = delete;
  PageLatch& operator=(const PageLatch&) = delete;
  ~PageLatch();

  PageId pid() const { return pid_; }
  LatchMode mode() const { return mode_; }
  uint8_t level() const { return level_; }

  // 本守卫是否真的去锁了底层页闩。
  //
  // 【false 不等于"没加锁就往下走"】两处来源，语义完全不同：
  //   1) 本线程已按**不弱于**本次请求的模式持有该页 —— 复用，安全；
  //      同 C0LatchGuard 的计数做法，理由也一样（树的调用是嵌套的：
  //      HybridIndex::Put → BPlusTree::Insert 两处都要叶闩）。
  //   2) PageLatch::None() —— 调用方尚未 Open，或检出违规后降级返回。
  // 第 2 种在 DEBUG 下必然已经 abort 过，Release 下由 stats.order_violations
  // 计数暴露，不会静静过去。
  bool owns() const { return owns_; }

  // 空守卫：不解锁、不计数
  static PageLatch None() { return PageLatch(); }

  void Release();

 private:
  friend class LatchManager;
  PageLatch(LatchManager* owner, PageId pid, LatchMode mode, uint8_t level, bool owns);

  LatchManager* owner_ = nullptr;
  PageId pid_ = kInvalidPageId;
  LatchMode mode_ = LatchMode::kShared;
  uint8_t level_ = 0;
  bool owns_ = false;
};

// ---------------------------------------------------------------------------
// M6-1 · C0 全局闩的 RAII 守卫　**【C1 落地后索引层已不再使用，仅测试在用】**
//
// 索引层现在用的是**页闩**（`FetchPage(pid, mode)` 把闩挂在 `PageGuard` 上）+ 写串行化
// `BufferPool::write_mutex`。C0LatchGuard 保留下来只因为
// tests/concurrency/test_c0_latch.cc 还在用它验证"守卫可重入"这套语义本身。
//
// 【为什么需要它，而不直接用 shared_lock】索引层的调用是**嵌套**的：
// HybridIndex::Put 会调 BPlusTree::Insert 与 RTree::Insert，两个入口各自要取闩。
// shared_mutex 不可重入，两次加锁就是自死锁。守卫因此在本线程内做计数：
//   * 本线程已按 kShared 持闩，再要 kShared  → 不再加锁（owns() == false），安全
//   * 本线程已按 kExclusive 持闩，再要任意   → 不再加锁，安全
//   * 本线程已按 kShared 持闩，却要 kExclusive → **升级**，这是设计违规
//     （读路径内部发起了写），无法安全满足。DEBUG 下直接 abort 并打印；
//     Release 下不满足请求，把违规暴露成"锁没起作用"而不是死锁。
class C0LatchGuard {
 public:
  // latch 允许为 nullptr（调用方尚未 Open）——此时守卫退化为空操作。
  C0LatchGuard(std::shared_mutex* latch, LatchMode mode);
  ~C0LatchGuard();
  C0LatchGuard(C0LatchGuard&& other) noexcept;
  C0LatchGuard& operator=(C0LatchGuard&& other) noexcept;
  C0LatchGuard(const C0LatchGuard&) = delete;
  C0LatchGuard& operator=(const C0LatchGuard&) = delete;

  // 本守卫是否真的持有了底层锁（嵌套时为 false）
  bool owns() const { return owns_; }

 private:
  void Release();

  // 【记的是"我加进了哪个计数"，不是"我要的是哪个模式"】两者在嵌套时会分叉：
  // 已持独占时再要共享，构造函数加的是独占计数，析构若按"我要的是共享"去还，
  // 就会去减一个没加过的计数，独占计数永远减不到 0 → 锁泄漏。
  // 这条不是推演出来的，是 test_c0_latch.cc 的 LatchGuardIsReentrant 抓出来的。
  enum class Counted : uint8_t { kNone = 0, kShared = 1, kExclusive = 2 };

  std::shared_mutex* latch_ = nullptr;
  LatchMode mode_ = LatchMode::kShared;
  Counted counted_ = Counted::kNone;
  bool owns_ = false;
};

class LatchManager {
 public:
  explicit LatchManager(bool enable_order_check);
  ~LatchManager();
  LatchManager(const LatchManager&) = delete;
  LatchManager& operator=(const LatchManager&) = delete;

  // level：该页在树里的层级（叶 0，根最大），调用方从 PageHeader::level 取。
  // **没有默认值**，逼调用方想清楚自己在树的哪一层 —— 顺序检查全靠它。
  StatusOr<PageLatch> Acquire(PageId pid, LatchMode mode, uint8_t level);

  // 顺序检查：本线程持锁栈 + 本次请求。检出问题按下面的策略处理。
  // 单独暴露是为了让"下钻前先验证"和"真的去锁"分开——检查必须发生在阻塞之前。
  void CheckOrder(PageId pid, LatchMode mode, uint8_t level);

  struct Stats {
    // C1：多个线程同时走这条路径，计数必须是原子的（同 M6-1 对 PoolStats 的处理）
    std::atomic<uint64_t> shared_acquisitions{0};
    std::atomic<uint64_t> exclusive_acquisitions{0};
    // 真的在页闩上等过的次数（try_lock 失败后才阻塞），不是"想过要等"
    std::atomic<uint64_t> contentions{0};
    // 检出逆序/重复获取/升级的次数。**这是 LockOrderChecker 的判据本身**，
    // 不是日志装饰：DEBUG 下它同时会 abort，Release 下它是唯一的信号。
    std::atomic<uint64_t> order_violations{0};
    // 本线程已持有时复用（未真正加锁）的次数——正常现象，见 PageLatch::owns()
    std::atomic<uint64_t> reused_acquisitions{0};
  };
  const Stats& stats() const;

  // 检出违规时是否立即 abort。默认：DEBUG 构建下 true，Release 下 false
  // （Release 里 abort 会把线上进程变成一个随时会炸的雷，只用计数暴露）。
  // 测试要观测"确实被检出"就得先关掉它——所以它必须可关，否则判据不可证伪。
  void set_abort_on_violation(bool on) { abort_on_violation_ = on; }
  bool abort_on_violation() const { return abort_on_violation_; }
  // 编译期判定的 DEBUG 默认值，测试用它确认"DEBUG 下确实是开的"
  static bool DefaultAbortOnViolation();

  // 本线程当前持锁栈（自根向下）的快照；诊断与测试用
  std::vector<HeldPage> HeldStack() const;

  // 全局 latch（C0 模式）：读写互斥，用于 M6-1 的基线实现
  std::shared_mutex& global() { return global_; }

 private:
  friend class PageLatch;
  void ReleaseHeld(PageId pid, LatchMode mode, bool owns);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::shared_mutex global_;
  bool abort_on_violation_ = false;
};

}  // namespace ltm

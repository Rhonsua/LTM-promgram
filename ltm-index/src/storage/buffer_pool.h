// src/storage/buffer_pool.h
// L1 缓存层：页框管理 / 钉住 / 淘汰 / PageGuard RAII（方案 §4.3）。
//
// 硬约束：
//   * 唯一合法的页访问入口是 FetchPage()，返回 PageGuard。
//   * 禁止提供返回裸页指针的接口，从编译期杜绝裸指针逃逸（方案 D2）。
//   * 钉住页永不驱逐；脏页驱逐前必须先落 WAL 再写盘（不变量 I6）。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "concurrency/latch_manager.h"
#include "storage/disk_manager.h"
#include "storage/eviction_policy.h"
#include "storage/meta_page.h"
#include "storage/page.h"
#include "storage/wal.h"
#include "util/config.h"
#include "util/status.h"
#include "util/types.h"

namespace ltm {

class RecoveryManager;
// 热度表与驱逐策略都只通过存储层抽象使用（见 storage/eviction_policy.h）
class BufferPool;

// RAII 守卫：析构自动 unpin；标记为写者则置脏。
// 禁止把 PageGuard 或页数据指针保存到任何长生命周期结构中。
//
// 【C1：守卫同时持有页闩】取页时可以带一个页闩（kShared/kExclusive），闩随守卫
// 析构释放。释放顺序由 `Release()` 决定：**先放闩、再 unpin** —— 放闩之后没人
// 会改这页的内容，unpin 只是归还帧。反过来（先 unpin 再放闩）会让帧在闩还持有时
// 被别人换走，`page()` 指向的内存就不再是这一页了。
class PageGuard {
 public:
  PageGuard() = default;
  PageGuard(BufferPool* pool, uint32_t frame_index, PageId pid);
  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;
  ~PageGuard();

  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;

  bool valid() const { return pool_ != nullptr; }
  PageId pid() const { return pid_; }

  // C1：把页闩交给守卫（取页时由缓冲池调用）。守卫析构即释放。
  void attach_latch(PageLatch latch) { latch_ = std::move(latch); }
  bool holds_latch() const { return latch_.owns(); }
  LatchMode latch_mode() const { return latch_.mode(); }

  // 绑定到本帧数据的 Page 视图（Page 是廉价对象：内部只有一个指针）。
  //
  // 【返回 const Page 是刻意的，D9】Page 只是一个指针包装，所以 `Page p =
  // guard.page(); p.mutable_header()` 照样写得动帧内存 —— 类型系统必须把这条路
  // 堵死。返回 const 之后，`guard.page().mutable_*()` 直接编译不过，于是想拿到
  // **可写**的 Page 就只剩 `mutable_page()` 一条路，而它一定会 MarkDirty()。
  // 页算子里那些收 `Page*` 的挂钩（SplitInto / BorrowFrom）同理：唯一的合法来源
  // 是 `&guard.mutable_page()`，标脏不可能被忘掉。
  //
  // 注意 `Page p = guard.page();`（拷贝一份再改）仍然能编译，因为拷贝出来的是
  // 非 const 对象。这是本约束的边界，不是漏洞：真正要防的是"直接对守卫的返回值
  // 写"，而那是页式实现里最常见的写法。
  const Page page() const;
  const PageHeader& header() const { return page().header(); }
  const char* payload() const { return page().payload(); }

  // 写访问：会把本守卫标记为脏页（忘记标脏是页式实现最常见的 bug）
  PageHeader* mutable_header();
  char* mutable_payload();
  // 可直接传给需要 Page* 的页内算子（如 TreePageOps::SplitInto），同时标脏
  Page mutable_page() {
    MarkDirty();
    return page();
  }

  void MarkDirty();
  bool dirty() const { return dirty_; }

  // 释放守卫（等价于作用域结束）
  void Release();

  // 【转让 pin】放弃这个守卫但**不 Unpin** —— 这一格 pin 由调用方接管。
  // 只给 BufferPool::NewPage 的"复用空闲页"用：它先把空闲页 Fetch 进来（拿到 1 个
  // pin），再把这一格 pin 直接变成新页的 pin。若走 Release，pin 会先掉到 0，
  // 在"掉到 0"与"重新置 1"之间这个帧就可能被淘汰、再被交给别的页（C1 下读者
  // 并发跑着，这个窗口是真实的）。闩也一并放弃（空闲页本来就不带闩）。
  void Detach();

  // 【取走页闩】把闩从守卫里移出，守卫此后只负责 unpin。
  //
  // 用途只有一个：**读者侧 crabbing**。方案 §4.6 要求"拿到子页闩之后再放父页闩"，
  // 而朴素的"父+子两帧一起钉"会让每读者同时占两帧 —— 8 读者 × 树高就把 16 帧的
  // 池子钉满（`kOutOfSpace`，实测）。这里把"闩"和"pin"解耦：
  //   * **闩是页号级的、与帧是否驻留无关** —— 放掉 pin 让帧可被复用（帧预算仍 1），
  //     而闩继续挡住写者改这一页（父页一改，读者的下钻路由就可能失效）。
  //   * 代价：持闩期间这一页的帧可能被淘汰；但内容没被改过（闩在），
  //     写回/重读拿到的都是同一份，安全。
  // 调用方接管闩的释放（`PageLatch::Release()`）。
  PageLatch take_latch() { return std::move(latch_); }

 private:
  BufferPool* pool_ = nullptr;
  uint32_t frame_ = 0;
  PageId pid_ = kInvalidPageId;
  bool dirty_ = false;
  // C1 页闩（默认 None = 不持闩）。Release() 先放它、再 unpin。
  PageLatch latch_;
};

class BufferPool {
 public:
  struct PoolStats {
    // M6-1 · C0：命中路径在**共享**闩下并发执行，这两个计数是同一条路径上的共享
    // 可变状态，必须是原子的。其余字段只在写操作（独占闩）或未命中路径上被改 ——
    // 未命中路径现在由 Impl::table_mutex 的独占段串行化（缺陷 #46），保持普通整数。
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    uint64_t evictions = 0;
    uint64_t dirty_evictions = 0;
    uint64_t prefetches = 0;
    uint64_t prefetch_hits = 0;
    uint64_t wal_replays = 0;
    uint64_t pages_created = 0;
    uint64_t pages_allocated = 0;  // 从空闲链取出
    uint64_t pages_freed = 0;      // 归还空闲链

    // ---- M4-2 写屏障的可见性计数（缺一个都会让"假绿"变得可能）----
    uint64_t wal_deltas = 0;            // 采集并追加进 WAL 的页 delta 数
    uint64_t deltas_skipped_identical = 0;  // before == after，跳过（D3）
    uint64_t deltas_missing_before = 0;     // 事务内改过但没被钉住过 -> 采集漏洞
    uint64_t deltas_outside_txn = 0;        // 事务之外标脏的页（如 Open 建根页）
    uint64_t lsn_stamps = 0;                // 成功写进页头的 page_lsn 次数
    uint64_t lsn_stamps_missed = 0;         // 要写 LSN 时该帧已不在池中
    // 写盘前先按写前日志规则补一次 fsync 的次数。**非零是正常的**：
    // 事务中途淘汰脏页就必然发生。它是性能指标（多出来的 fsync），不是正确性指标。
    uint64_t wal_sync_on_flush = 0;
    // 脏页带着 page_lsn == kInvalidLsn 落盘的次数 —— 这才是真正的 I6 缺口：
    // 页被写过，却没有任何 REDO 覆盖它。Sync 修不好这件事。
    uint64_t i6_unstamped_writes = 0;
    uint64_t i6_violations = 0;             // 上面那条在 strict 模式下触发并中止的次数

    // 日志体积的**实测**口径（方案"性能"一节把"只记变化区间"列为第一优先，
    // 但那个 20–50x 是估的，不是量的）。这两个计数给出真实比值：
    // delta_bytes_full 是按整页记的总字节，delta_bytes_changed 是各 delta 中
    // 真正变化的字节区间之和。比值就是"区间记录"能省下的倍数。
    uint64_t delta_bytes_full = 0;
    uint64_t delta_bytes_changed = 0;

    // ---- M6 · 写事务 Abort 的运行期回滚（C1 的前置条件）----
    // Abort 不只是写一条 kAbort：它必须把内存状态退回事务开始态，否则日志说
    // "这个事务没了"、内存里却留着它的记录。三个计数分开是为了让"回滚确实发生了
    // 并且覆盖到每一类状态"可见 —— 一个总计数会让"只回滚了页帧、忘了分配器"
    // 这种半吊子修复看起来一样绿。
    uint64_t abort_pages_rolled_back = 0;   // 在池中、被就地还原的页
    uint64_t abort_pages_reloaded = 0;      // 事务中被换出、Abort 时重新读回来还原的页
    uint64_t abort_pages_discarded = 0;     // 事务里新建、Abort 时被收回（丢脏位）的页
    uint64_t abort_alloc_restores = 0;      // 分配器游标/空闲链被还原的次数
    uint64_t abort_participant_undos = 0;   // 索引层注册的派生状态回滚被执行了几次

    PoolStats() = default;
    // atomic 成员删掉了隐式拷贝构造/赋值，但 `IndexStats::pool = bp->stats()` 要按
    // 值拷一份快照（stats() 只给引用，快照是调用方的事），所以显式补回来。
    // 取 relaxed 就够：这里要的是"某个瞬间的计数"，不是用它排序任何别的内存访问。
    PoolStats(const PoolStats& o) { *this = o; }
    PoolStats& operator=(const PoolStats& o) {
      hits.store(o.hits.load(std::memory_order_relaxed), std::memory_order_relaxed);
      misses.store(o.misses.load(std::memory_order_relaxed), std::memory_order_relaxed);
      evictions = o.evictions;
      dirty_evictions = o.dirty_evictions;
      prefetches = o.prefetches;
      prefetch_hits = o.prefetch_hits;
      wal_replays = o.wal_replays;
      pages_created = o.pages_created;
      pages_allocated = o.pages_allocated;
      pages_freed = o.pages_freed;
      wal_deltas = o.wal_deltas;
      deltas_skipped_identical = o.deltas_skipped_identical;
      deltas_missing_before = o.deltas_missing_before;
      deltas_outside_txn = o.deltas_outside_txn;
      lsn_stamps = o.lsn_stamps;
      lsn_stamps_missed = o.lsn_stamps_missed;
      wal_sync_on_flush = o.wal_sync_on_flush;
      i6_unstamped_writes = o.i6_unstamped_writes;
      i6_violations = o.i6_violations;
      delta_bytes_full = o.delta_bytes_full;
      delta_bytes_changed = o.delta_bytes_changed;
      abort_pages_rolled_back = o.abort_pages_rolled_back;
      abort_pages_reloaded = o.abort_pages_reloaded;
      abort_pages_discarded = o.abort_pages_discarded;
      abort_alloc_restores = o.abort_alloc_restores;
      abort_participant_undos = o.abort_participant_undos;
      return *this;
    }
  };

  // 注意：heat 与 policy 由调用方持有生命周期（通常放在 HybridIndex 里），
  //       BufferPool 只借用指针。二者允许为 nullptr —— 此时退化为纯 CLOCK 扫描。
  BufferPool(const BufferPoolOptions& options, DiskManager* disk, WalWriter* wal,
             RecoveryManager* recovery, HeatSource* heat = nullptr,
             const EvictionPolicy* policy = nullptr);
  ~BufferPool();
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // ---- 页访问 ----
  // 缺页 → 读盘 → 校验 → 钉住并返回守卫。
  //
  // 【C1 的 mode 参数】索引层取树页时传 kShared（读）/ kExclusive（写），返回的
  // 守卫会带上这把页闩；缓冲池自己的内部取页（页 0、空闲链、新页）用默认 kNone。
  // 见 latch_manager.h 的 LatchMode 说明。
  StatusOr<PageGuard> FetchPage(PageId pid, LatchMode mode = LatchMode::kNone);
  // 取一个空闲页：优先复用空闲页链，否则向文件申请新页。返回的页已 Init 并置脏。
  // 新页尚未发布（父页还没指向它），读者到不了，所以不带页闩。
  StatusOr<PageGuard> NewPage(PageType type, TreeId tree_id, uint8_t level, PageId* out_pid);
  // 把页归还空闲链（脏页会先刷盘）。页号在归还后不可再被引用（不变量 I7）。
  Status FreePage(PageId pid);

  // 仅供 BufferPool 自身在引导阶段（初始化元数据页）使用：
  // 跳过校验和检查，因为此时页在磁盘上还是一块全零。
  StatusOr<PageGuard> FetchUnchecked(PageId pid, LatchMode mode = LatchMode::kNone);

  Status Unpin(PageId pid, bool dirty);
  Status FlushPage(PageId pid);
  Status FlushAll();

  const PoolStats& stats() const;
  // Snapshot pool and optional I/O counters under write_mutex -> table_mutex.
  // Raw stats()/disk() references still require a quiescent caller.
  PoolStats SnapshotStats(DiskManager::IoStats* io = nullptr) const;
  double hit_rate() const;

  // ---- M6-1 · C0 全局闩（多读单写）----
  //
  // 【它为什么住在缓冲池，而不是某棵树里】闩保护的是"一次索引操作期间，池里的
  // 帧不会被别人换掉"。帧归缓冲池所有，只有池能给出这个保证。索引层在公开入口
  // 处加锁：读操作（Search/RangeScan/ScanAll）取共享，写操作（Insert/Erase/
  // Checkpoint）取独占。
  //
  // 【C0 已由 C1 取代，2026-10-04】索引层**不再取**这把闩：写串行化归 `write_mutex()`
  // （D4 单写者），写与读的互斥下移到**页闩**（`FetchPage` 的 mode 参数把闩挂在
  // `PageGuard` 上）。这里保留 `c0_latch()` 只是因为它仍在 `C0LatchGuard` 与
  // `test_c0_latch` 的语义里；生产路径上没有调用者。
  // 未命中/淘汰路径的帧表安全由 `Impl::table_mutex` 负责（缺陷 #46）。
  std::shared_mutex& c0_latch() { return c0_latch_; }
  uint32_t pool_frames() const;
  uint32_t resident_pages() const;
  uint32_t dirty_pages() const;

  // ---- C1 · 页闩表 ----
  //
  // 【为什么闩表归缓冲池】页闩保护的是"这一页的字节正被谁读/写"，而页属于池。
  // 索引层持着父页闩下钻、在子页闩拿到之后再放父页闩（latch coupling），
  // 全部通过这里取闩。锁序（全局唯一一条）：**页闩自根向下**；
  // 页闩 → frame_table（缓冲池内部的锁）→ 该页的 load-lock。
  //
  // 【它和 C0 闩的关系】C0 的 `c0_latch()` 是"整次操作"的粗粒度闩，C1 要用页闩
  // 取代它；过渡期两者并存（页闩在 C0 之下，冗余但无害），撤 C0 的最后一步才去掉。
  LatchManager& latches() { return *latch_mgr_; }
  const LatchManager& latches() const { return *latch_mgr_; }
  // 未 Open 的场景不会发生（闩表在构造时就建好），这里只给诊断用
  LatchManager* latches_or_null() { return latch_mgr_.get(); }
  const LatchManager::Stats& latch_stats() const { return latch_mgr_->stats(); }

  // ---- C1 · 写串行化（D4：写路径永远单写者）----
  //
  // 【它取代了 C0 闩的哪一半】C0 的 shared_mutex 干两件事：① 写与写互斥
  // ② 写与读互斥。C1 把 ② 下移到**页闩**（写者持被改页的独占闩，读者持读页的
  // 共享闩），只把 ① 留在这里。于是"写串行化"不再顺带把全部读者也挡住。
  //
  // 由 `BeginWriteOp()` 在 depth 0 处获取、`Commit/AbortWriteOp` 在 depth 0 处释放，
  // 所以任何走 `ScopedWriteOp` 的写操作（含 HybridIndex 的双树 Put）自动被串行化，
  // 不需要每个入口各写一遍。可重入（recursive_mutex），嵌套写只加深计数。
  std::recursive_mutex& write_mutex() { return write_mutex_; }

  // 热冷 / 迁移（借用，可能为 nullptr）
  HeatSource* heat_source() { return heat_; }
  const EvictionPolicy* eviction_policy() const { return policy_; }

  // 元数据页（页 0）
  // 页 0 的权威布局见 storage/meta_page.h。上层（各棵树）**不要**自己
  // reinterpret 页 0 —— 那会与缓冲池的物理页管理（空闲链、分配游标）互相覆盖
  // （BPlusTree 曾是这么做的，导致建库时读到全零页并当成损坏）。
  // FetchMetaPage() 仅供只读检查；改元数据请用下面两个方法。
  StatusOr<PageGuard> FetchMetaPage() { return FetchPage(kMetaPageId); }
  // 把元数据页从磁盘加载进内存镜像（幂等）。**上层在直接读 meta() 之前必须先调用它**：
  // 内存镜像是懒加载的，未加载时是一份全零镜像，而"槽位 in_use == 0"的语义是
  // "这棵树还不存在" —— 于是重开数据库时会看不见已有的树、又新建一棵空树，
  // 直接把原数据的根页/空闲链覆盖掉（BPlusTree 重开读到 0 条就是这个原因）。
  Status EnsureMetaLoaded();
  // 元数据页的内存镜像（需要先 EnsureMetaLoaded；读它不产生 I/O）
  const MetaPage& meta() const { return *meta_copy_; }
  // 写入树槽位 / 物理页状态，并立刻把元数据页落盘（返回时磁盘上已是新状态）
  Status WriteTreeSlot(uint32_t slot, const TreeMetaSlot& value);
  Status WriteBufferMeta();
  // false 表示数据文件是本次新建的（页 0 之前全是零）
  bool recovered_from_disk() const { return recovered_from_disk_; }

  // WAL 转发：索引层不得直接持有 WalWriter（分层约束），需要写日志时经此转发。
  // 记录编码 / 顺序追加 / Commit 前的 fsync 屏障见 M4-1（storage/wal.cc）。
  StatusOr<TxnId> BeginOp();
  // 返回该页 REDO 记录的 LSN（无日志模式下返回 kInvalidLsn）
  StatusOr<Lsn> AppendPageDelta(TxnId txn, const PageImageDelta& delta);
  Status Commit(TxnId txn);
  Status Abort(TxnId txn);
  Lsn flushed_lsn() const;
  Lsn last_lsn() const;

  // ---- 写事务（M4-2）----
  //
  // 【为什么事务住在这里，而不是某棵树里】M5 的 HybridIndex::Put 要**两棵树一个事务**。
  // 事务若归 IndexTree::Impl，双树 Put 就会裂成两个事务 —— 正是本项要防的"提交一半"，
  // 只是升了一层。BufferPool 拥有全部帧，是唯一能看见"哪些页将被写盘"的对象。
  //
  // 可重入：嵌套调用只加深计数，只有最外层 Commit 才真正提交（RTree::Insert 内部的
  // Erase+Insert、以及将来 HybridIndex 内部对两棵树的调用都会嵌套）。
  Status BeginWriteOp();
  Status CommitWriteOp();
  Status AbortWriteOp();
  bool write_op_active() const;

  // ---- 写事务的"参与者回滚"协议（M6 · Abort 的运行期回滚）----
  //
  // 【为什么页的 before-image 不够】缓冲池能回滚的只有**它自己拥有的**状态：
  // 帧、分配器游标、页 0 的镜像。但索引层还各自持有一份**派生状态** ——
  // IndexTree::Impl 的 root / leftmost_leaf / height / num_entries。它们不在任何
  // 页里（页 0 的树槽位只在 Open/Close/Checkpoint 时同步，不是每次插入都写），
  // 所以页回滚够不着它们；不回滚的后果是 Validate 直接报
  // "total entries in pages 400 != metadata count 600"。
  //
  // 于是索引层在**加入事务时**（而不是提交时）把自己的事务开始态注册进来。
  // 注册必须发生在它改动任何东西之前，那样捕获到的就是"事务开始态"。
  //
  // 同一个事务里重复注册是允许的（一棵树在一次 Upsert 里会进出多次），
  // 去重由注册方自己用 write_op_generation() 做 —— 缓冲池不去猜谁注册过。
  uint64_t write_op_generation() const;
  void RegisterWriteUndo(std::function<void()> fn);

  // 让调用方（检查点/时钟线程）能把 flush 挂进淘汰路径
  bool ShouldFlushDirty() const;

 private:
  friend class PageGuard;
  StatusOr<PageGuard> FetchChecked(PageId pid, bool validate_checksum,
                                   LatchMode mode = LatchMode::kNone);
  char* FrameData(uint32_t frame_index);
  const char* FrameData(uint32_t frame_index) const;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<MetaPage> meta_copy_;
  // C1 · 页闩表。声明在 impl_ 之后 = 先于 impl_ 析构，保证没有线程还能拿着闩
  // 去访问已经释放的帧表（与 c0_latch_ 同一个理由）。
  std::unique_ptr<LatchManager> latch_mgr_;
  // C1 · 写串行化（D4）。由 BeginWriteOp/CommitWriteOp 在 depth 0 处获取/释放。
  mutable std::recursive_mutex write_mutex_;
  // M6-1 · C0 全局闩。最后一个成员 = 最后被析构：保证它在 impl_ 之后销毁，
  // 免得析构期间还有线程持有闩去访问已经释放的帧表。
  std::shared_mutex c0_latch_;
  bool recovered_from_disk_ = false;
  HeatSource* heat_ = nullptr;
  const EvictionPolicy* policy_ = nullptr;
};

}  // namespace ltm

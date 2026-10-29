// src/index/hybrid_index.h
// 混合索引门面（方案 §4.9）：对外统一一维/多维/空间/混合三类查询。
//
// 与中期实现的差异（重点）：
//   1. 不再手工维护「键 → 空间条目」映射。B+ 树叶条目的 value 直接存 R 树叶页号，
//      删除时由 B+ 树定位结果直接驱动 R 树删除 —— 从根上消除双树不一致（不变量 I8）。
//   2. put/erase 生成一个 WriteBatch，由 WalWriter 以**同一个 txn_id 一次提交**，
//      崩溃恢复要么全生效要么全回滚；禁止两条独立事务分别写两棵树。
//
// 【M5 实现时必须知道的两件事】（实测结论，详见 docs/status.md 的 M5 一节）
//   * 「B+ 叶条目里存 R 树叶页号」只是**提示**，不是事实：R 树的借键与合并会把记录
//     搬到兄弟页、并释放原页。所以取用时必须先校验、失败则退化为叶链全扫
//     （`RTree::Locate`），把提示当权威会读到别的树的页。提示的有效率有计数器，
//     是 M6 基准的一个真实指标。
//   * 两棵树的元数据槽位必须不同（`HybridOptions::bplus_slot` / `rtree_slot`），
//     否则后打开的那棵树会把前者的根页号/高度覆盖掉。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "index/index_entry.h"
#include "index/index_registry.h"
#include "index/tree_ops.h"
#include "migration/hot_cold_tracker.h"
#include "migration/migration_policy.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/recovery.h"
#include "storage/wal.h"
#include "util/config.h"

namespace ltm {

struct HybridOptions {
  Config config;
  TreeBuildOptions bplus;
  TreeBuildOptions rtree;
  TreeId bplus_tree_id = 1;
  TreeId rtree_tree_id = 2;
  // 两棵树在元数据页里各占一个槽位。默认 0/1 —— **必须不同**。
  uint32_t bplus_slot = 0;
  uint32_t rtree_slot = 1;
  bool create_if_missing = true;
};

struct IndexStats {
  TreeStats bplus;
  TreeStats rtree;
  BufferPool::PoolStats pool;
  double pool_hit_rate = 0.0;
  DiskManager::IoStats io;
  uint64_t queries = 0;
  uint64_t writes = 0;
  // M5：双树定位提示的有效率（hint 命中 / 失效 / 退化为叶链全扫）
  uint64_t locate_hint_hits = 0;
  uint64_t locate_hint_misses = 0;
  uint64_t locate_chain_scans = 0;
  // M5：I8 交叉校验过的记录对数（Validate() 填）
  uint64_t i8_pairs_checked = 0;
  HotColdTracker::Stats heat;
};

class HybridIndex {
 public:
  HybridIndex();
  ~HybridIndex();
  HybridIndex(const HybridIndex&) = delete;
  HybridIndex& operator=(const HybridIndex&) = delete;

  // 打开/新建：内部持有 DiskManager / RecoveryManager（它持有 WalWriter）/ BufferPool
  //
  // 【顺序是契约】DiskManager → RecoveryManager → BufferPool → Recover → 各棵树 Open。
  // 恢复必须在任何索引访问之前完成，否则读到的是恢复前的页。
  Status Open(const HybridOptions& options);
  Status Close();       // flush_all + 元数据落盘 + 置 clean_shutdown
  bool is_open() const;

  // ---- 写：一次逻辑事务，双树原子提交（不变量 I8）----
  Status Put(const IndexEntry& entry);
  Status Erase(KeyId key);

  // ---- 读 ----
  StatusOr<IndexEntry> Get(KeyId key);
  // 注意：本类的成员函数名 Query 会**遮蔽**同名类型 ltm::Query。
  // 在类作用域内写 const Query& 时，编译器按"成员函数名"解析参数类型，
  // 报出误导性的 "缺少类型说明符 - 假定为 int"（MSVC C4430/C2143）。
  // 因此本头文件内一律写全限定名 ltm::Query。同类陷阱：任何与类型同名的成员函数。
  Status Query(const ltm::Query& query, const std::function<bool(const IndexEntry&)>& emit);
  Status Scan(const ltm::Query& query, const std::function<bool(const IndexEntry&)>& emit);
  Status Knn(const ltm::Query& query, uint32_t k, std::vector<IndexEntry>* out);

  // ---- 运维 ----
  Status Checkpoint();
  Status DecayHeat();        // synchronized explicit maintenance; no background thread
  Status Validate();          // 全量不变量自检 + I8 双树交叉校验（不写文件）
  IndexStats stats() const;

  // Public operations coordinate whole dual-tree reads/writes. Get/range/spatial
  // and KNN reads can overlap; diagnostic counters publish a brief snapshot.
  // Query callbacks may reenter read APIs after each page latch is released.
  // Mutation/Close/Checkpoint/Validate from a read callback returns kInvalidArgument.
  // Open/Close are coordinated; object destruction requires all callers to stop.
  // Raw pool/disk access bypasses the API guard: external synchronization required.
  BufferPool* pool() { return pool_.get(); }
  DiskManager* disk() { return disk_.get(); }

 private:
  // 一维探针（B+ 树只接受 dim == 0 的记录）
  static IndexEntry OneDim(KeyId key);
  // 从 B+ 读一条记录（载荷 + R 树叶页号提示）。用单点区间扫描实现，
  // 复用扫描与解码路径；Search 和扫描结果现在均可持有自有载荷。
  Status ReadBPlusKey(KeyId key, IndexEntry* out, bool* present);
  // 把 R 树的查询结果物化成 IndexEntry；回调载荷可能由页批次或 KNN 缓冲持有。
  // 在 emit 回调内拷贝，不依赖页守卫存活，也不保留临时缓冲指针。
  static IndexEntry Materialize(const RecordRef& ref);

  struct Impl;
  std::unique_ptr<Impl> impl_;

  std::unique_ptr<DiskManager> disk_;
  // 【M4-3 起 WalWriter 归 RecoveryManager 所有】恢复期写的 CLR 与恢复之后的正常写
  // 必须落在同一条 LSN 序列上，所以只能有一个持有者；BufferPool 与检查点都借用它。
  std::unique_ptr<RecoveryManager> recovery_;
  // 热度表与驱逐策略由本层持有，再借指针给 BufferPool（依赖倒置，见
  // storage/eviction_policy.h）。必须声明在 pool_ 之前，保证先构造后析构。
  std::unique_ptr<HotColdTracker> hot_cold_;
  std::unique_ptr<MigrationPolicy> migration_policy_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<CheckpointManager> checkpoint_;
  std::unique_ptr<TreeOps> bplus_;
  std::unique_ptr<TreeOps> rtree_;
};

}  // namespace ltm

// src/index/rtree.h
// 页式 R 树（方案 §4.8）。
//
// 与中期实现的差异：
//   1. 维度处理改为「模板 Scalar/MAX_DIM + 运行期 dim 校验」：页布局不依赖维度，
//      但热路径用栈上定长数组，消除 new/delete。
//   2. 分裂策略可切换：even（中期做法，仅作对照）/ linear / quadratic（默认）。
//   3. KNN 用 best-first 优先队列 + 剪枝，并报告剪枝率。
//   4. 每个 MBR 合并/面积增量都前置 validate_mbr()，异常路径必须有明确错误码。
#pragma once

#include "index/tree_ops.h"

namespace ltm {

enum class RSplitPolicy : uint8_t { kEven = 0, kLinear = 1, kQuadratic = 2 };

// 每页最大条目数的默认值与下限。上限由运行时按维度算出的字节容量决定
// （见 rtree.cc 的 ByteCapacityLimit），因为 dim 是运行期才知道的。
constexpr uint16_t kDefaultRTreeCapacity = 64;
constexpr uint16_t kRTreeMinCapacity = 4;

RSplitPolicy ParseSplitPolicy(const std::string& name);
const char* SplitPolicyName(RSplitPolicy p);

// 页内 MBR 条目
#pragma pack(push, 1)
struct RTreeEntry {
  PageId child;         // 叶页时为 kInvalidPageId（载荷直接跟在后面）
  uint16_t payload_len; // 叶条目：随后的载荷长度
  uint16_t dim;         // 冗余维度，用于加载时校验
  // double lo[dim]; double hi[dim]; char payload[payload_len];
};
#pragma pack(pop)

class RTreePageOps final : public TreePageOps {
 public:
  // capacity 是**每页最大条目数**（不是阶数）；实际生效值会按页容量与维度夹紧，
  // 见 rtree.cc 的 ByteCapacityLimit()。默认 64 与 TreeBuildOptions::order_or_capacity 一致。
  explicit RTreePageOps(uint16_t dim, RSplitPolicy policy, uint16_t capacity = 64);

  const char* name() const override { return "rtree-page"; }

  // ---- 树身份 ----
  MetaTreeKind MetaKind() const override { return MetaTreeKind::kRTree; }
  PageType PageTypeForLevel(uint8_t level) const override {
    return level == 0 ? PageType::kRTreeLeaf : PageType::kRTreeInternal;
  }

  // ---- 页元数据 ----
  bool IsLeaf(const PageHeader& header) const override { return header.level == 0; }
  uint8_t Level(const PageHeader& header) const override { return header.level; }
  uint16_t Capacity() const override { return capacity_; }
  ElemKey FirstKey(const PageHeader& header, const char* payload) const override;
  // 多维树没有"最左子页哨兵"：0 号槽是真条目，所有槽一视同仁。
  // 这一条同时决定了两件事：分隔条目刷新从 0 号槽开始；Validate 不豁免重复引用。
  bool HasLeftmostSentinel() const override { return false; }
  // 插入会让路径上每个子页的 MBR 并集变大 → 返回路径必须**逐层**刷新父页分隔条目。
  bool SeparatorMayChangeOnInsert() const override { return true; }
  // 叶内条目按分裂结果排列，与路由标签无关 → 不做二分、不断言叶内键序。
  bool KeysAreOrdered() const override { return false; }

  // ---- 条目访问 ----
  // LowerBound 对多维树**不参与路由**（见 tree_ops.h 的 P1-4）：
  // 叶内无序，二分没有意义；保留它只为满足契约，路由一律走 ChooseChildSlot。
  SlotId LowerBound(const PageHeader& header, const char* payload, ElemKey key) const override;
  ElemKey GetKey(const PageHeader& header, const char* payload, SlotId slot) const override;
  PageId GetChild(const PageHeader& header, const char* payload, SlotId slot) const override;

  // ---- 页生命周期 / 修改 ----
  Status InitPage(PageHeader* header, char* payload, bool is_leaf) override;
  Status InsertEntry(PageHeader* header, char* payload, SlotId at, ElemKey key, PageId child,
                     const std::vector<char>& value) override;
  Status EraseSlot(PageHeader* header, char* payload, SlotId slot) override;

  bool IsOverflow(const PageHeader& header, const char* payload) const override;
  bool IsUnderflow(const PageHeader& header, const char* payload) const override;
  // R 树的下溢阈值是容量的 40%（方案 §4.8），低于 B+ 的 50%
  uint16_t UnderflowThreshold() const override;

  // ---- 分裂 / 借键 / 合并 ----
  Status SplitInto(PageHeader* header, char* payload, Page* new_page) override;
  bool BorrowFrom(PageHeader* header, char* payload, Page* donor, BorrowDirection dir,
                  const TreeEntry& parent_sep, TreeEntry* moved_out) override;
  Status MergeFrom(PageHeader* header, char* payload, const Page& donor,
                   ElemKey separator_for_internal) override;

  // ---- 自检 ----
  Status ValidatePage(const PageHeader& header, const char* payload) const override;
  Status CheckParentKey(const PageHeader& parent, const char* parent_payload, SlotId i,
                        const Page& child) const override;
  void AccumulateOverlap(const PageHeader& header, const char* payload, double* sum,
                         uint64_t* count) const override;

  // ---- v3 挂钩：逻辑记录 <-> 页内条目 ----
  Status EncodeEntry(const IndexEntry& entry, TreeEntry* out) const override;
  StatusOr<RecordRef> DecodeLeafEntry(const PageHeader& header, const char* payload,
                                      SlotId slot) const override;
  SlotId FindEntry(const PageHeader& header, const char* payload,
                   const IndexEntry& probe) const override;
  StatusOr<LeafInsertResult> InsertLeafEntry(PageHeader* header, char* payload,
                                             const TreeEntry& entry) override;
  SlotId ChooseChildSlot(const PageHeader& header, const char* payload, const IndexEntry& target,
                         SlotChoice purpose) const override;
  StatusOr<std::vector<SlotId>> CandidateChildSlots(const PageHeader& header, const char* payload,
                                                    const IndexEntry& target,
                                                    SlotChoice purpose) const override;
  TreeEntry BuildSeparator(const PageHeader& child_header,
                           const char* child_payload) const override;
  Status WriteSeparator(PageHeader* header, char* payload, SlotId slot,
                        const TreeEntry& sep) override;
  bool SeparatorEquals(const PageHeader& header, const char* payload, SlotId slot,
                       const TreeEntry& sep) const override;
  uint32_t EntryBytes(const TreeEntry& entry) const override;
  uint32_t EntryByteBudget() const override;
  // 「空间先满」协议的分半判据：与 ChooseChildSlot(kInsert) 同构（最小面积增量
  // → 面积 → 条目更少）。用中性默认会把分裂退化成随机划分，空间聚集性荡然无存。
  PageId ChooseHalf(const TreeEntry& entry, PageId left, const PageHeader& left_header,
                    const char* left_payload, PageId right, const PageHeader& right_header,
                    const char* right_payload) const override;

  // ---- R 树自有的读取入口（供 RTree 的查询半边使用）----
  // 页内第 slot 条目的 MBR（叶页与内部页都有）
  StatusOr<Mbr> EntryMbr(const PageHeader& header, const char* payload, SlotId slot) const;

  uint16_t dim() const { return dim_; }
  RSplitPolicy split_policy() const { return policy_; }

 private:
  uint16_t dim_;
  RSplitPolicy policy_;
  uint16_t capacity_ = 0;
};

class RTree final : public TreeOps {
 public:
  RTree();
  ~RTree() override;

  const char* name() const override { return "rtree"; }
  Status Open(BufferPool* pool, TreePageOps* page_ops, const TreeBuildOptions& options,
              TreeId tree_id) override;
  Status Insert(const IndexEntry& entry) override;
  Status Erase(const IndexEntry& entry) override;
  StatusOr<RecordRef> Search(const IndexEntry& probe) override;
  Status RangeScan(const Query& query, const std::function<bool(const RecordRef&)>& emit) override;
  Status SpatialQuery(const Query& query,
                      const std::function<bool(const RecordRef&)>& emit) override;
  // Exact top-k by ascending (distance, key), including ties at the cutoff.
  // k == 0 retains the existing one-neighbor behavior.
  Status Knn(const Query& query,
             const std::function<bool(const RecordRef&, double)>& emit) override;
  Status ScanAll(const std::function<bool(const RecordRef&)>& emit) override;
  Status Validate() override;
  TreeStats stats() const override;
  Status Destroy() override;

  // ---------------------------------------------------------------------------
  // 混合索引（方案 §4.9）需要的两个额外入口
  //
  // 【为什么 R 树必须提供"按键定位"而不是复用 Search】R 树的插入去向由 MBR 决定，
  // 记录的逻辑键（KeyId）只是随条目一起存的载荷，**不是路由键**（路由键是 MBR
  // 的 Z 序编码）。于是"这条键在不在树里"无法用 MBR 相交来剪枝 —— 拿不到 MBR
  // 就没法走 Search。唯一能做的定位是叶链全扫，所以本类直接把它做成一个入口，
  // 并把"提示页"作为可选的加速手段。
  // ---------------------------------------------------------------------------

  // 插入并报告这条记录最终落在哪个叶页（混合索引把它写进 B+ 叶条目）。
  StatusOr<PageId> InsertReturningLeaf(const IndexEntry& entry);

  // 按键定位一条记录，返回**物化**的结果（key / MBR / payload 都已拷出页外）。
  //
  // 【hint 只是提示】R 树的分裂、借键、合并都会让记录换页：借键会把一条记录搬到
  // 兄弟页，合并会把整页记录搬到兄弟页并**释放原页**（原页随后可能被分配给别的树）。
  // 所以 hint 指定的页必须先校验（是 R 树叶页、属于本树、页里确实有这条键），
  // 校验不过就退化为叶链全扫。把提示当权威会读到别的树的页或已释放的页 ——
  // 那是静默损坏，不是性能问题。
  StatusOr<IndexEntry> Locate(KeyId key, PageId hint = kInvalidPageId);

  // 定位的命中统计（给基准与结题报告用：hint 的有效率本身就是 M6 的一个指标）
  struct LocateStats {
    uint64_t hint_hits = 0;    // hint 直接命中
    uint64_t hint_misses = 0;  // 给了 hint 但失效（换页 / 页被释放 / 键不在该页）
    uint64_t chain_scans = 0;  // 退化为叶链全扫的次数
  };
  const LocateStats& locate_stats() const;
  void ResetLocateStats();

  // M3-4 验收：KNN 与暴力扫描结果必须等价（容差 1e-9）
  Status KnnBruteForce(const Query& query,
                       const std::function<bool(const RecordRef&, double)>& emit) const;

  // 剪枝率统计：访问页数 / 暴力访问页数
  struct KnnProbeStats {
    uint64_t pages_visited = 0;
    uint64_t entries_scanned = 0;
    uint64_t brute_pages = 0;
  };
  // A synchronized value snapshot, never an alias to mutable counters. Fields
  // describe the last completed best-first call; brute_pages is overwritten by
  // the last completed brute-force call and reset by a completed best-first call.
  // Concurrent calls cannot use this diagnostic snapshot as per-query telemetry.
  KnnProbeStats knn_stats() const;

 private:
  // 插入的实现（upsert + 单事务）。out_leaf 非空时写出记录最终落在哪个叶页。
  Status InsertInternal(const IndexEntry& entry, PageId* out_leaf);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm

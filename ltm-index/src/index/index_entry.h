// src/index/index_entry.h
// 统一的逻辑记录表示与查询描述（方案 §4.9）。
//
// 关键设计（取代中期报告的 HybridIndex 手工维护键→空间条目映射）：
//   B+ 树的叶条目 value 直接存放 R 树的叶页号，删除时由 B+ 树定位结果直接驱动
//   R 树删除，不再维护独立的映射结构 —— 从根上消除"双树不一致"风险。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "util/status.h"
#include "util/types.h"

namespace ltm {

// 多维向量（避免为每个条目单独分配：小维度用 inline 缓冲）
class Point {
 public:
  Point() = default;
  explicit Point(uint16_t dim) : coords_(dim, 0.0) {}

  uint16_t dim() const { return static_cast<uint16_t>(coords_.size()); }
  double operator[](uint16_t d) const { return coords_[d]; }
  double& operator[](uint16_t d) { return coords_[d]; }
  void resize(uint16_t d) { coords_.resize(d); }
  const std::vector<double>& coords() const { return coords_; }

  Status Validate() const;

 private:
  std::vector<double> coords_;
};

// 最小边界矩形
struct Mbr {
  Point lo;
  Point hi;

  static Mbr Invalid(uint16_t dim);
  bool valid() const;
  Status Validate() const;   // lo[d] <= hi[d] 且维度一致
  // 各维长度之积（2 维即矩形面积）。非法/退化 MBR 一律返回 0，
  // 而不是 inf 或 NaN —— 否则选叶策略的"最小面积增量"比较会全部失效。
  double Area() const;
  double Margin() const;     // 各维长度之和（用于平局判定）
  double DistanceTo(const Point& p) const;  // 点到 MBR 的最小距离（KNN 剪枝用）
  bool Intersects(const Mbr& other) const;
  bool Contains(const Mbr& other) const;
  bool ContainsPoint(const Point& p) const;
  void ExpandToInclude(const Mbr& other);
  void ExpandToInclude(const Point& p);
  // 面积增量；非法/空 MBR 返回 0（调用方应先 Validate）
  double EnlargementToInclude(const Mbr& other) const;
};

// 系统对外的一条逻辑记录
struct IndexEntry {
  KeyId key = 0;                 // 一维键（同时作为逻辑主键）
  Mbr mbr;                       // 多维范围；dim==0 表示纯一维记录
  std::vector<char> payload;     // 记录载荷（页内按紧凑布局序列化）

  // 混合索引的定位提示（方案 §4.9）：R 树里承载本记录的叶页号。
  // B+ 树的页算子把它写进自己的叶条目（`BPlusLeafEntry::rtree_leaf_page`），
  // 于是"按主键定位 R 树里的同一条记录"不必再全树搜索。
  //
  // 【它只是提示，不是事实】R 树的分裂/借键/合并都会让记录换页（借键与合并甚至
  // 会把记录搬到兄弟页、并释放原页），所以取用时**必须**先校验：目标页确实是
  // R 树叶页、且页里确实有这条键。校验失败就退化为叶链全扫（`RTree::Locate`），
  // 不能把提示当成权威 —— 那会读到别的树的页，或者读到一个已释放的页。
  PageId rtree_leaf = kInvalidPageId;

  uint16_t dim() const { return mbr.lo.dim(); }
  Status Validate() const;
};

// 查询结果引用：要么直接带回载荷，要么只带定位信息（大记录场景）
struct RecordRef {
  KeyId key = 0;
  Mbr mbr;
  const char* payload = nullptr;
  uint32_t payload_len = 0;
  PageId rtree_leaf = kInvalidPageId;  // B+ 树叶条目中保存的 R 树叶页号

  // 【为什么需要一块自有缓冲 —— 这是被实测逼出来的，不是设计洁癖】
  //
  // P1 以前扫描回调在页守卫存活期间执行；现在范围/空间扫描先复制一页命中
  // 记录、释放页闩，再调用 emit。KNN 回调仍由查询结果缓冲持有载荷。
  //
  // 但 `Search` 是把结果**返回给调用方**的：返回时内部守卫已经析构（契约见
  // tree_ops.h），payload 若仍指向页帧就是 use-after-free —— 别的线程一次
  // 缺页/淘汰就能把那块帧交给另一个页。tools/c0_boundary_probe 在小池子下
  // 实测到过这种"读回别人的载荷"（wrong_value）。原契约写的是"调用方需自行
  // 拷贝"，可调用方**拿到指针时守卫已经放掉了**，根本没有安全的拷贝窗口。
  //
  // Search 和范围/空间扫描把载荷拷进这里，payload 改指向它；复制 RecordRef
  // 可延长这块载荷的寿命。KNN 回调此成员可能为空，需在回调内物化结果。
  std::shared_ptr<const std::string> owned_payload;

  // 这条记录是不是自带载荷（payload 指向 owned_payload 而非页帧）
  bool owns_payload() const { return owned_payload != nullptr; }
};

// 把一条"payload 指向页内"的 RecordRef 变成**自持载荷**的 RecordRef：
// 把字节拷进 owned_payload，payload 改指向它。给 Search 这类"结果要活过守卫"
// 的接口及释放页闩后才执行回调的页批次使用。
RecordRef WithOwnedPayload(RecordRef ref);

enum class QueryKind : uint8_t {
  kPoint = 0,         // 一维精确
  kRange1D = 1,       // 一维区间
  kRangeSpatial = 2,  // 多维范围（矩形包含）
  kIntersect = 3,     // 空间相交
  kKnn = 4,           // 最近邻
};

struct Query {
  QueryKind kind = QueryKind::kPoint;
  KeyId key_lo = 0;
  KeyId key_hi = 0;
  Mbr mbr;             // 空间查询范围
  Point probe;         // KNN 查询点
  uint32_t k = 10;     // KNN 的 k
  TreeId tree = 0;     // 目标子树（混合索引内）
};

// 载荷序列化：页内紧凑布局，往返无损（属性测试覆盖）
class PayloadCodec {
 public:
  // 布局：dim(u16) | key(u64) | lo[dim](f64) | hi[dim](f64) | len(u32) | bytes
  static std::vector<char> Encode(const IndexEntry& entry);
  static Status Decode(const char* data, uint32_t len, IndexEntry* out);
  static uint32_t EncodedSize(const IndexEntry& entry);
};

}  // namespace ltm

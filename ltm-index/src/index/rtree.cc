#include "index/rtree.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "concurrency/latch_manager.h"
#include "index/page_utils.h"
#include "util/log.h"

namespace ltm {

using pageutil::RecomputeFreeSpaceOn;

// ===========================================================================
// 实现总览（M3）
//
// 跨页算法（下钻 / 分裂传播 / 借键合并 / 下溢循环 / 根升降 / 自检骨架）全部
// 由 src/index/tree_ops.cc 的 IndexTree 承担，本文件只提供**页内**语义：
// 布局、选叶、分裂、借键、合并、自检与编码。
//
// 页布局（严格按 rtree.h 已声明的 RTreeEntry，头 8 字节之后是"条目体"）：
//
//   叶条目 = RTreeEntry{ child=kInvalidPageId, payload_len, dim }
//            + lo[dim] | hi[dim] | key:u64 | payload[payload_len]
//   内部条目 = RTreeEntry{ child=子页号, payload_len=0, dim }
//            + lo[dim] | hi[dim]
//
// 设计要点（逐条都有代价，写在它能被推翻的地方）：
//
// (1) **MBR 只存一份**。TreeEntry.value 就是"条目体"，key/child 放在 RTreeEntry
//     头里，不套 PayloadCodec —— 后者是自描述布局，会把 dim/lo/hi 重复存第二遍
//     （dim=2 时每条白费 26 字节），而这里布局由 dim_ 完全确定。
//
// (2) **路由标签是派生值，不落盘**。GetKey 从页内 lo/hi 现算 Z-order 码。
//     存一份派生值只会带来"改了 MBR 忘了改 Z 码"的不一致（I9 的同类问题）。
//
// (3) **叶内条目无序**（KeysAreOrdered()==false）：分裂按面积分组，不保序。
//     因此 LowerBound 不参与路由（见 tree_ops.h 的 P1-4），全部叶层定位都走
//     FindEntry 钩子。这是 R 树与 B+ 树在"叶页内部"这一层的根本不兼容。
//
// (4) **EntryByteBudget 让变长条目退化成定长算术**。预算 = kPagePayloadSize /
//     capacity_，于是"页在达到 capacity_ 条之前先被字节撑满"不可能发生，
//     IsOverflow（槽数口径）与分裂/下溢/合并的算术与 B+ 树完全一致。
//     capacity_ 在构造函数里已被 ByteCapacityLimit(dim) 夹住，保证预算 ≥ 最小条目开销。
//
// (5) 所有对 double 的读写走 std::memcpy：条目区从页尾向下生长，偏移不一定
//     8 字节对齐（叶条目含变长载荷），直接解引用 double* 是未定义行为。
// ===========================================================================

namespace {

constexpr size_t kRHeaderBytes = sizeof(RTreeEntry);  // 8

// 【尺寸口径，全文件只此一处定义】
//   RHalfBytes(dim) —— MBR 的**一半**（lo 段或 hi 段）在页内占的字节
//   RBodyBytes(dim) —— 完整 MBR（lo[dim] 紧跟 hi[dim]）占的字节
// 条目体（TreeEntry.value）就是"头之后的全部字节"：
//   内部条目体 = lo | hi                       （RBodyBytes）
//   叶条目体   = lo | hi | key | payload        （RBodyBytes + 8 + |payload|）
// 键与载荷的偏移都按 RBodyBytes 算。早期版本把 RBodyBytes 当成"一半"用，
// 于是 hi 与 key 都被从错位的地方读出来 —— 症状是 FindEntry 永远找不到
// 刚插进去的条目、父页 MBR 读出天文数字。
inline size_t RHalfBytes(uint16_t dim) { return static_cast<size_t>(dim) * sizeof(double); }

inline size_t RBodyBytes(uint16_t dim) { return static_cast<size_t>(2u) * RHalfBytes(dim); }

inline size_t RLeafEntrySize(uint16_t dim, uint16_t payload_len) {
  return kRHeaderBytes + RBodyBytes(dim) + sizeof(KeyId) + payload_len;
}

// 一条页内条目的解析视图。只取指针与少量标量，不做任何拷贝，
// 也不假设对齐（读 lo/hi 一律经 RLoAt/RHiAt 的 memcpy）。
struct REntryView {
  const char* base = nullptr;
  uint16_t dim = 0;
  uint16_t payload_len = 0;
  uint32_t payload_off = 0;
  KeyId key = 0;  // 仅叶条目有意义
  PageId child = kInvalidPageId;
  bool is_leaf = false;
};

inline double RLoAt(const REntryView& v, uint16_t d) {
  double x = 0.0;
  std::memcpy(&x, v.base + kRHeaderBytes + static_cast<size_t>(d) * sizeof(double), sizeof(double));
  return x;
}

inline double RHiAt(const REntryView& v, uint16_t d) {
  double x = 0.0;
  std::memcpy(&x,
              v.base + kRHeaderBytes + RHalfBytes(v.dim) +
                  static_cast<size_t>(d) * sizeof(double),
              sizeof(double));
  return x;
}

// 解析（不校验 MBR 合法性，那由 ValidatePage 负责）。
// len 必须与 is_leaf 决定的定长前缀相容，且条目内记录的 dim 必须与树一致 ——
// 这是"R 树维度校验与异常路径"的落点：维度不符时条目一律视为不可解析。
bool ParseREntry(const char* base, uint16_t len, uint16_t dim, bool is_leaf, REntryView* out) {
  const size_t need = is_leaf ? (kRHeaderBytes + RBodyBytes(dim) + sizeof(KeyId))
                              : (kRHeaderBytes + RBodyBytes(dim));
  if (base == nullptr || len < need) {
    return false;
  }
  RTreeEntry head{};
  std::memcpy(&head, base, sizeof(head));
  if (head.dim != dim) {
    return false;
  }
  if (is_leaf) {
    if (head.child != kInvalidPageId) {
      return false;
    }
    if (static_cast<size_t>(head.payload_len) != len - need) {
      return false;
    }
    KeyId k = 0;
    std::memcpy(&k, base + kRHeaderBytes + RBodyBytes(dim), sizeof(KeyId));
    out->key = k;
    out->payload_off = static_cast<uint32_t>(need);
  } else {
    if (head.child == kInvalidPageId || head.payload_len != 0) {
      return false;
    }
  }
  out->base = base;
  out->dim = dim;
  out->payload_len = head.payload_len;
  out->child = head.child;
  out->is_leaf = is_leaf;
  return true;
}

inline bool RParseSlot(const PageHeader& header, const char* payload, SlotId slot,
                       uint16_t dim, REntryView* out) {
  if (slot >= header.num_slots) {
    return false;
  }
  const Slot s = pageutil::SlotsOf(payload)[slot];
  if (pageutil::SlotIsEmpty(s)) {
    return false;
  }
  return ParseREntry(payload + s.offset, s.len, dim, header.level == 0, out);
}

// ---- 几何运算：全部在裸 double 数组上做，避免为每次比较构造 Mbr（含 vector 分配）----

inline double RArea(const double* lo, const double* hi, uint16_t dim) {
  double a = 1.0;
  for (uint16_t d = 0; d < dim; ++d) {
    const double w = hi[d] - lo[d];
    if (!(w > 0.0)) {
      return 0.0;  // 退化（点/线）MBR 面积记 0，与 Mbr::Area 口径一致
    }
    a *= w;
  }
  return a;
}

inline double RUnionArea(const double* alo, const double* ahi, const double* blo, const double* bhi,
                         uint16_t dim) {
  double a = 1.0;
  for (uint16_t d = 0; d < dim; ++d) {
    const double l = alo[d] < blo[d] ? alo[d] : blo[d];
    const double h = ahi[d] > bhi[d] ? ahi[d] : bhi[d];
    const double w = h - l;
    if (!(w > 0.0)) {
      return 0.0;
    }
    a *= w;
  }
  return a;
}

inline bool RIntersects(const double* alo, const double* ahi, const double* blo, const double* bhi,
                        uint16_t dim) {
  for (uint16_t d = 0; d < dim; ++d) {
    if (ahi[d] < blo[d] || bhi[d] < alo[d]) {
      return false;
    }
  }
  return true;
}

inline bool RContains(const double* alo, const double* ahi, const double* blo, const double* bhi,
                      uint16_t dim) {
  for (uint16_t d = 0; d < dim; ++d) {
    if (alo[d] > blo[d] || ahi[d] < bhi[d]) {
      return false;
    }
  }
  return true;
}

inline double RIntersectionArea(const double* alo, const double* ahi, const double* blo,
                                const double* bhi, uint16_t dim) {
  double a = 1.0;
  for (uint16_t d = 0; d < dim; ++d) {
    const double l = alo[d] > blo[d] ? alo[d] : blo[d];
    const double h = ahi[d] < bhi[d] ? ahi[d] : bhi[d];
    if (h < l) {
      return 0.0;
    }
    const double w = h - l;
    if (!(w > 0.0)) {
      return 0.0;
    }
    a *= w;
  }
  return a;
}

// 点到 MBR 的最小距离（KNN 剪枝的下界）。与 Mbr::DistanceTo 同口径。
inline double RPointDistance(const double* lo, const double* hi, const double* p, uint16_t dim) {
  double sum = 0.0;
  for (uint16_t d = 0; d < dim; ++d) {
    double delta = 0.0;
    if (p[d] < lo[d]) {
      delta = lo[d] - p[d];
    } else if (p[d] > hi[d]) {
      delta = p[d] - hi[d];
    }
    sum += delta * delta;
  }
  return std::sqrt(sum);
}

// ---- 并集累加器（MBR 并集是 I9 的唯一真值来源）----
struct RMbrAcc {
  std::vector<double> lo;
  std::vector<double> hi;
  bool empty = true;

  void Reset(uint16_t dim) {
    lo.assign(dim, 0.0);
    hi.assign(dim, 0.0);
    empty = true;
  }
  void Add(const double* l, const double* h, uint16_t dim) {
    if (empty) {
      lo.assign(l, l + dim);
      hi.assign(h, h + dim);
      empty = false;
      return;
    }
    for (uint16_t d = 0; d < dim; ++d) {
      if (l[d] < lo[d]) {
        lo[d] = l[d];
      }
      if (h[d] > hi[d]) {
        hi[d] = h[d];
      }
    }
  }
};

// ---- 路由标签：MBR 中心点的 Z-order（Morton）编码 ----
//
// 只用于复用跨页算法的路由比较，**不落盘**（见文件头 (2)）。
// double → 16bit 量化走 IEEE-754 的"可排序整数"变换：负数整体取反、正数置符号位，
// 于是整数的自然序与 double 的序一致（含 ±inf）。NaN 映射到确定值，
// 不做特殊处理 —— 它只是路由标签，不参与任何不变量。
inline uint64_t RSortableBits(double x) {
  uint64_t u = 0;
  std::memcpy(&u, &x, sizeof(u));
  if (u & 0x8000000000000000ull) {
    u = ~u;
  } else {
    u |= 0x8000000000000000ull;
  }
  return u;
}

inline uint16_t RQuantize(double x) {
  return static_cast<uint16_t>(RSortableBits(x) >> 48);
}

inline uint64_t RMorton4(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
  uint64_t r = 0;
  for (int i = 15; i >= 0; --i) {
    const uint64_t shift = static_cast<uint64_t>(15 - i) * 4u;
    r |= static_cast<uint64_t>((a >> i) & 1u) << (shift + 0u);
    r |= static_cast<uint64_t>((b >> i) & 1u) << (shift + 1u);
    r |= static_cast<uint64_t>((c >> i) & 1u) << (shift + 2u);
    r |= static_cast<uint64_t>((d >> i) & 1u) << (shift + 3u);
  }
  return r;
}

constexpr uint16_t kZOrderDims = 4;

inline ElemKey RZOrderOf(const std::vector<double>& lo, const std::vector<double>& hi,
                         uint16_t dim) {
  uint16_t q[kZOrderDims] = {0, 0, 0, 0};
  const uint16_t n = dim < kZOrderDims ? dim : kZOrderDims;
  for (uint16_t d = 0; d < n; ++d) {
    q[d] = RQuantize(0.5 * (lo[d] + hi[d]));
  }
  uint64_t code = RMorton4(q[0], q[1], q[2], q[3]);
  if (dim > kZOrderDims) {
    // 4 维以外的坐标折叠进低 16 位，保证高维条目的路由标签仍然可分辨
    uint64_t fold = 0;
    for (uint16_t d = kZOrderDims; d < dim; ++d) {
      fold = fold * 1099511628211ull ^ RQuantize(0.5 * (lo[d] + hi[d]));
    }
    code ^= (fold & 0xFFFFull);
  }
  return code;
}

ElemKey RZOrderOfView(const REntryView& v) {
  std::vector<double> lo(v.dim), hi(v.dim);
  for (uint16_t d = 0; d < v.dim; ++d) {
    lo[d] = RLoAt(v, d);
    hi[d] = RHiAt(v, d);
  }
  return RZOrderOf(lo, hi, v.dim);
}

// 把一条条目的 MBR 读进调用方给的缓冲区（避免每次比较都建 vector）
bool RReadMbr(const REntryView& v, double* lo, double* hi) {
  for (uint16_t d = 0; d < v.dim; ++d) {
    lo[d] = RLoAt(v, d);
    hi[d] = RHiAt(v, d);
  }
  return true;
}

// ---- 分裂分组 ----
//
// 三种策略共用同一个"分配"骨架，差异只在**种子选取**与**下一条的选取**：
//   kEven     ：前半 / 后半（中期做法，仅作对照实验）
//   kLinear   ：线性种子 + 顺序分配（Guttman 线性代价）
//   kQuadratic：二次种子 + 最大偏好差分配（默认，目标是降低重叠面积）
//
// min_group 是下溢阈值：任何一半的条目数都不得低于它，否则分裂会制造一个
// 立即下溢的页，Validate 的 I3 当场报错。调用方保证 n >= 2*min_group。
Status ChoosePartition(const std::vector<std::vector<char>>& entries, uint16_t dim, bool is_leaf,
                       RSplitPolicy policy, uint16_t min_group, std::vector<bool>* to_new) {
  const size_t n = entries.size();
  to_new->assign(n, false);
  if (policy == RSplitPolicy::kEven) {
    for (size_t i = n / 2; i < n; ++i) {
      (*to_new)[i] = true;
    }
    return Status::Ok();
  }

  std::vector<std::vector<double>> lo(n), hi(n);
  for (size_t i = 0; i < n; ++i) {
    REntryView v;
    if (!ParseREntry(entries[i].data(), static_cast<uint16_t>(entries[i].size()), dim, is_leaf,
                     &v)) {
      return Status::Error(StatusCode::kCorruption, "unparsable entry during the split");
    }
    lo[i].resize(dim);
    hi[i].resize(dim);
    RReadMbr(v, lo[i].data(), hi[i].data());
  }

  // ---- 种子选取 ----
  size_t s1 = 0;
  size_t s2 = 1;
  if (policy == RSplitPolicy::kQuadratic) {
    double best = -1.0;
    for (size_t i = 0; i < n; ++i) {
      for (size_t j = i + 1; j < n; ++j) {
        const double d = RUnionArea(lo[i].data(), hi[i].data(), lo[j].data(), hi[j].data(), dim) -
                         RArea(lo[i].data(), hi[i].data(), dim) - RArea(lo[j].data(), hi[j].data(), dim);
        if (d > best) {
          best = d;
          s1 = i;
          s2 = j;
        }
      }
    }
  } else {
    // 线性种子：每维找"下界最大的那条"与"上界最小的那条"，用该维跨度归一化后
    // 取间距最大的一维的那两条。O(dim·n)，不出现 n² 的两两比较。
    double best = -1.0;
    for (uint16_t d = 0; d < dim; ++d) {
      size_t max_lo_i = 0;
      size_t min_hi_i = 0;
      double span_lo = lo[0][d];
      double span_hi = hi[0][d];
      for (size_t i = 1; i < n; ++i) {
        if (lo[i][d] > lo[max_lo_i][d]) {
          max_lo_i = i;
        }
        if (hi[i][d] < hi[min_hi_i][d]) {
          min_hi_i = i;
        }
        if (lo[i][d] < span_lo) {
          span_lo = lo[i][d];
        }
        if (hi[i][d] > span_hi) {
          span_hi = hi[i][d];
        }
      }
      if (max_lo_i == min_hi_i) {
        continue;
      }
      const double span = span_hi - span_lo;
      const double sep =
          (lo[max_lo_i][d] - hi[min_hi_i][d]) / (span > 0.0 ? span : 1.0);
      if (sep > best) {
        best = sep;
        s1 = max_lo_i;
        s2 = min_hi_i;
      }
    }
  }
  if (s1 == s2) {
    s1 = 0;
    s2 = 1;
  }

  std::vector<bool> assigned(n, false);
  assigned[s1] = true;
  assigned[s2] = true;
  (*to_new)[s2] = true;

  std::vector<double> glo = lo[s1];
  std::vector<double> ghi = hi[s1];
  std::vector<double> hlo = lo[s2];
  std::vector<double> hhi = hi[s2];
  size_t cnt0 = 1;
  size_t cnt1 = 1;
  size_t remaining = n - 2;

  auto expand = [dim](std::vector<double>* tlo, std::vector<double>* thi,
                      const std::vector<double>& elo, const std::vector<double>& ehi) {
    for (uint16_t d = 0; d < dim; ++d) {
      if (elo[d] < (*tlo)[d]) {
        (*tlo)[d] = elo[d];
      }
      if (ehi[d] > (*thi)[d]) {
        (*thi)[d] = ehi[d];
      }
    }
  };

  while (remaining > 0) {
    // 某一组即便拿走全部剩余条目也补不足 min_group → 把剩下的全给它。
    // 这一步保证分裂不会制造下溢页（否则 Validate 的 I3 立刻报错）。
    if (cnt0 + remaining <= min_group) {
      for (size_t i = 0; i < n; ++i) {
        if (!assigned[i]) {
          (*to_new)[i] = false;
          expand(&glo, &ghi, lo[i], hi[i]);
        }
      }
      break;
    }
    if (cnt1 + remaining <= min_group) {
      for (size_t i = 0; i < n; ++i) {
        if (!assigned[i]) {
          (*to_new)[i] = true;
          expand(&hlo, &hhi, lo[i], hi[i]);
        }
      }
      break;
    }

    size_t pick = n;
    if (policy == RSplitPolicy::kQuadratic) {
      double best_diff = -1.0;
      for (size_t i = 0; i < n; ++i) {
        if (assigned[i]) {
          continue;
        }
        const double e0 = RUnionArea(glo.data(), ghi.data(), lo[i].data(), hi[i].data(), dim) -
                          RArea(glo.data(), ghi.data(), dim);
        const double e1 = RUnionArea(hlo.data(), hhi.data(), lo[i].data(), hi[i].data(), dim) -
                          RArea(hlo.data(), hhi.data(), dim);
        const double diff = (e0 > e1) ? (e0 - e1) : (e1 - e0);
        if (diff > best_diff) {
          best_diff = diff;
          pick = i;
        }
      }
    } else {
      for (size_t i = 0; i < n; ++i) {
        if (!assigned[i]) {
          pick = i;
          break;
        }
      }
    }
    if (pick >= n) {
      break;
    }

    const double e0 = RUnionArea(glo.data(), ghi.data(), lo[pick].data(), hi[pick].data(), dim) -
                      RArea(glo.data(), ghi.data(), dim);
    const double e1 = RUnionArea(hlo.data(), hhi.data(), lo[pick].data(), hi[pick].data(), dim) -
                      RArea(hlo.data(), hhi.data(), dim);
    // 三级 tie-break：面积增量 → 组面积 → 条目更少的一组（与选叶判据同构）
    bool go_new;
    if (e1 != e0) {
      go_new = (e1 < e0);
    } else {
      const double a0 = RArea(glo.data(), ghi.data(), dim);
      const double a1 = RArea(hlo.data(), hhi.data(), dim);
      if (a1 != a0) {
        go_new = (a1 < a0);
      } else {
        go_new = (cnt1 <= cnt0);
      }
    }
    assigned[pick] = true;
    --remaining;
    if (go_new) {
      (*to_new)[pick] = true;
      expand(&hlo, &hhi, lo[pick], hi[pick]);
      ++cnt1;
    } else {
      expand(&glo, &ghi, lo[pick], hi[pick]);
      ++cnt0;
    }
  }
  return Status::Ok();
}

// 维度 d 下一条**空载荷叶条目**的页内开销：条目头 + lo/hi + key + 槽目录项。
// 用它给出容量的字节上限，保证"每页至少放得下 capacity_ 条空载荷条目" ——
// 这是树能持续推进（分裂后一定放得下）的下限。变长载荷超出时由
// EntryByteBudget 在插入前拒掉（见文件头 (4)）。
size_t MinLeafEntryBytes(uint16_t dim) {
  return RLeafEntrySize(dim, 0) + sizeof(Slot);
}

uint16_t ByteCapacityLimit(uint16_t dim) {
  if (dim == 0) return 0;
  const size_t per = MinLeafEntryBytes(dim);
  const size_t cap = kPagePayloadSize / per;
  return static_cast<uint16_t>(cap > 0xFFFF ? 0xFFFF : cap);
}

}  // namespace

RSplitPolicy ParseSplitPolicy(const std::string& name) {
  if (name == "even") return RSplitPolicy::kEven;
  if (name == "linear") return RSplitPolicy::kLinear;
  return RSplitPolicy::kQuadratic;  // 默认
}

const char* SplitPolicyName(RSplitPolicy p) {
  switch (p) {
    case RSplitPolicy::kEven: return "even";
    case RSplitPolicy::kLinear: return "linear";
    case RSplitPolicy::kQuadratic: return "quadratic";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// 构造与容量
// ---------------------------------------------------------------------------
RTreePageOps::RTreePageOps(uint16_t dim, RSplitPolicy policy, uint16_t capacity)
    : dim_(dim), policy_(policy) {
  const uint16_t limit = ByteCapacityLimit(dim);
  const uint16_t wanted = capacity == 0 ? kDefaultRTreeCapacity : capacity;
  capacity_ = wanted < limit ? wanted : limit;
  if (capacity_ < kRTreeMinCapacity) {
    capacity_ = kRTreeMinCapacity;
  }
}

// ---------------------------------------------------------------------------
// 页元数据与条目访问
// ---------------------------------------------------------------------------
ElemKey RTreePageOps::FirstKey(const PageHeader& header, const char* payload) const {
  // 多维树没有"最左子页哨兵"：0 号槽是真条目。空页给 kMinElemKey 占位
  // （BuildSeparator 对空页另有一套约定，见那里的注释）。
  REntryView v;
  if (!RParseSlot(header, payload, 0, dim_, &v)) {
    return kMinElemKey;
  }
  return RZOrderOfView(v);
}

SlotId RTreePageOps::LowerBound(const PageHeader& header, const char* payload, ElemKey key) const {
  // 叶内条目无序（KeysAreOrdered()==false），二分没有意义。
  // 保留本方法只为满足契约；路由一律走 ChooseChildSlot / CandidateChildSlots。
  // 这里退化为"第一个键 >= key 的槽"的线性扫描，语义与 B+ 一致但代价 O(n)。
  const uint16_t n = header.num_slots;
  for (uint16_t i = 0; i < n; ++i) {
    REntryView v;
    if (!RParseSlot(header, payload, i, dim_, &v)) {
      continue;
    }
    if (RZOrderOfView(v) >= key) {
      return i;
    }
  }
  return n;
}

ElemKey RTreePageOps::GetKey(const PageHeader& header, const char* payload, SlotId slot) const {
  REntryView v;
  if (!RParseSlot(header, payload, slot, dim_, &v)) {
    return kMinElemKey;
  }
  return RZOrderOfView(v);
}

PageId RTreePageOps::GetChild(const PageHeader& header, const char* payload, SlotId slot) const {
  if (header.level == 0) {
    return kInvalidPageId;
  }
  REntryView v;
  if (!RParseSlot(header, payload, slot, dim_, &v)) {
    return kInvalidPageId;
  }
  return v.child;
}

Status RTreePageOps::InitPage(PageHeader* header, char* payload, bool is_leaf) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("RTreePageOps::InitPage got a null pointer");
  }
  if (dim_ == 0) {
    return InvalidArgument("RTreePageOps dimension is 0");
  }
  (void)payload;
  // 与 B+ 一致：不写 level，只校验调用方传参自洽（页类型由 NewPage 设定）
  if (is_leaf && header->level != 0) {
    return Status::Error(StatusCode::kCorruption,
                         "InitPage(is_leaf=true) but the page level != 0: the caller passed inconsistent arguments");
  }
  if (!is_leaf && header->level == 0) {
    return Status::Error(StatusCode::kCorruption,
                         "InitPage(is_leaf=false) but the page level == 0: the caller passed inconsistent arguments");
  }
  header->num_slots = 0;
  header->free_space = static_cast<uint16_t>(kPagePayloadSize);
  header->frag_bytes = 0;
  header->next_page = kInvalidPageId;
  header->prev_page = kInvalidPageId;
  header->flags = 0;
  return Status::Ok();
}

Status RTreePageOps::InsertEntry(PageHeader* header, char* payload, SlotId at, ElemKey key,
                                 PageId child, const std::vector<char>& value) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("RTreePageOps::InsertEntry got a null pointer");
  }
  const bool is_leaf = (header->level == 0);
  const size_t need = is_leaf ? (RBodyBytes(dim_) + sizeof(KeyId)) : RBodyBytes(dim_);
  if (value.size() < need) {
    return InvalidArgument("the entry body is only " + std::to_string(value.size()) + " bytes, at least " +
                           std::to_string(need) + "(dim=" + std::to_string(dim_) + ")");
  }
  if (at > header->num_slots) {
    at = header->num_slots;
  }
  // key 是派生出来的路由标签（Z 码），不落盘：这里只断言调用方给的值与
  // 条目体里的 MBR 自洽，防止"上游算了一套、页内存了另一套"的双真相。
  ElemKey derived = kMinElemKey;
  {
    std::vector<double> lo(dim_), hi(dim_);
    for (uint16_t d = 0; d < dim_; ++d) {
      std::memcpy(&lo[d], value.data() + static_cast<size_t>(d) * sizeof(double), sizeof(double));
      std::memcpy(&hi[d], value.data() + RHalfBytes(dim_) + static_cast<size_t>(d) * sizeof(double),
                  sizeof(double));
    }
    derived = RZOrderOf(lo, hi, dim_);
  }
  if (key != derived) {
    return Status::Error(StatusCode::kCorruption,
                         "InsertEntry's routing label disagrees with the entry body MBR (two sources of truth)");
  }

  const size_t total = kRHeaderBytes + value.size();
  if (total > 0xFFF0u) {
    return Status::Error(StatusCode::kPageFull, "entry too large");
  }
  std::vector<char> buf(total);
  RTreeEntry head{};
  head.child = is_leaf ? kInvalidPageId : child;
  head.dim = dim_;
  head.payload_len =
      is_leaf ? static_cast<uint16_t>(value.size() - RBodyBytes(dim_) - sizeof(KeyId)) : 0;
  std::memcpy(buf.data(), &head, sizeof(head));
  std::memcpy(buf.data() + kRHeaderBytes, value.data(), value.size());

  Page self(payload - kPageHeaderSize);
  const Status st = self.InsertEntryAt(at, buf.data(), static_cast<uint16_t>(buf.size()));
  if (!st.ok()) {
    return st;
  }
  return Status::Ok();
}

Status RTreePageOps::EraseSlot(PageHeader* header, char* payload, SlotId slot) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("RTreePageOps::EraseSlot got a null pointer");
  }
  Page self(payload - kPageHeaderSize);
  return self.EraseEntry(slot);
}

bool RTreePageOps::IsOverflow(const PageHeader& header, const char* payload) const {
  (void)payload;
  return header.num_slots >= capacity_;
}

bool RTreePageOps::IsUnderflow(const PageHeader& header, const char* payload) const {
  // 按存活槽计数：本树不保留墓碑，但用 LiveSlots 与 Validate 的填充率口径严格一致
  // （两处各算一次就会出现"通用层认为没下溢、页算子认为下溢"）。
  return pageutil::LiveSlots(header, payload) < UnderflowThreshold();
}

uint16_t RTreePageOps::UnderflowThreshold() const {
  // 方案 §4.8：R 树的下溢阈值是容量的 40%（低于 B+ 的 50%），下限 2
  const uint16_t t = static_cast<uint16_t>(static_cast<uint32_t>(capacity_) * 40u / 100u);
  return t < 2 ? 2 : t;
}

// ---------------------------------------------------------------------------
// 分裂
// ---------------------------------------------------------------------------
Status RTreePageOps::SplitInto(PageHeader* header, char* payload, Page* new_page) {
  if (header == nullptr || payload == nullptr || new_page == nullptr) {
    return InvalidArgument("RTreePageOps::SplitInto got a null pointer");
  }
  const bool is_leaf = (header->level == 0);

  // 先把存活条目整体拷进临时缓冲：源页与目标页的条目区在**同一帧的两端**
  // （或两帧），逐条直写会踩掉尚未读取的源条目。分组完成后统一重灌。
  std::vector<std::vector<char>> entries;
  for (uint16_t i = 0; i < header->num_slots; ++i) {
    const Slot s = pageutil::SlotsOf(payload)[i];
    if (pageutil::SlotIsEmpty(s)) {
      continue;
    }
    entries.emplace_back(payload + s.offset, payload + s.offset + s.len);
  }
  const size_t m = entries.size();
  if (m < 3) {
    return Status::Error(StatusCode::kCorruption,
                         "split of a page with fewer than 3 live entries (actual " + std::to_string(m) + ")");
  }
  const uint16_t min_group = UnderflowThreshold();
  if (m < static_cast<size_t>(min_group) * 2u) {
    // 页太稀疏，怎么分都会制造一个下溢页。声明了 EntryByteBudget 的树走不到这里
    // （预算保证 capacity_ 条一定放得下，于是分裂点必然是 num_slots == capacity_），
    // 走到这里说明该树的容量/预算配置自相矛盾 —— 明确报错，绝不制造下溢页。
    return Status::Error(StatusCode::kPageFull,
                         "the page holds only " + std::to_string(m) + " entries: cannot be split in two without producing an underfull page"
                         " (threshold " + std::to_string(min_group) +
                             "): entry too large, overflow-page support required (not implemented)");
  }

  std::vector<bool> to_new;
  LTM_RETURN_IF_ERROR(
      ChoosePartition(entries, dim_, is_leaf, policy_, min_group, &to_new));

  // 清空两页后重灌。new_page 由 NewPage + InitPage 建出，槽数已为 0；
  // 这里仍显式复位，保证"重灌"这个动作不依赖调用方的前置状态。
  header->num_slots = 0;
  header->free_space = static_cast<uint16_t>(kPagePayloadSize);
  header->frag_bytes = 0;
  new_page->mutable_header()->num_slots = 0;
  new_page->mutable_header()->free_space = static_cast<uint16_t>(kPagePayloadSize);
  new_page->mutable_header()->frag_bytes = 0;
  new_page->mutable_header()->level = header->level;
  new_page->mutable_header()->type = header->type;
  new_page->mutable_header()->tree_id = header->tree_id;

  Page self(payload - kPageHeaderSize);
  for (size_t i = 0; i < m; ++i) {
    Page* dst = to_new[i] ? new_page : &self;
    SlotId out = kInvalidSlot;
    const Status st =
        dst->AppendEntry(entries[i].data(), static_cast<uint16_t>(entries[i].size()), &out);
    if (!st.ok()) {
      return Status::Error(st.code(), std::string("R tree split failed to move entries: ") + st.message());
    }
  }

  if (is_leaf) {
    // 叶链：old -> new -> old_next（old_next 的 prev 由通用层修正）
    const PageId old_next = header->next_page;
    new_page->mutable_header()->next_page = old_next;
    new_page->mutable_header()->prev_page = header->page_id;
    header->next_page = new_page->header().page_id;
  }

  RecomputeFreeSpaceOn(header, payload);
  RecomputeFreeSpaceOn(new_page->mutable_header(), new_page->payload());

  if (header->num_slots == 0 || new_page->header().num_slots == 0) {
    return Status::Error(StatusCode::kCorruption, "the R tree split produced an empty page");
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 借键与合并
//
// 多维树与一维树在这里有两处结构性差异（tree_ops.h 的 v3 修订记录已固化）：
//   * 叶条目的**槽序不承载语义**（无序），所以两个方向都可以追加；
//     不存在 B+ 那种"借左兄弟必须插到开头"的布局手术。
//   * 内部页是"N 条目对 N 子页"、没有哨兵，父页的分隔条目在合并时**直接删除**
//     而不是下压进子页（所以 separator_for_internal 被忽略）。
// ---------------------------------------------------------------------------
bool RTreePageOps::BorrowFrom(PageHeader* header, char* payload, Page* donor, BorrowDirection dir,
                              const TreeEntry& parent_sep, TreeEntry* moved_out) {
  LTM_UNUSED(parent_sep);  // 多维树内部页借键不需要转移键：MBR 由调用方重新同步
  if (header == nullptr || payload == nullptr || donor == nullptr) {
    return false;
  }
  const PageHeader& dh = donor->header();
  if (dh.level != header->level) {
    return false;
  }
  const uint16_t dn = dh.num_slots;
  if (dn == 0) {
    return false;
  }
  // 方向决定搬哪一条：借左兄弟搬它的**末**条目，借右兄弟搬它的**首**条目。
  // 槽序对多维树无语义，所以两个方向都只需追加到本页末尾。
  const SlotId donor_slot =
      (dir == BorrowDirection::kFromLeft) ? static_cast<SlotId>(dn - 1) : static_cast<SlotId>(0);
  REntryView v;
  if (!RParseSlot(dh, donor->payload(), donor_slot, dim_, &v)) {
    return false;
  }
  const uint16_t len = pageutil::SlotsOf(donor->payload())[donor_slot].len;

  Page self(payload - kPageHeaderSize);
  const Status st = self.AppendEntry(v.base, len, nullptr);
  if (!st.ok()) {
    return false;  // 本页放不下：交给合并路径
  }
  if (moved_out != nullptr) {
    moved_out->key = v.is_leaf ? v.key : RZOrderOfView(v);
    moved_out->child = v.child;
    moved_out->value.assign(v.base + kRHeaderBytes, v.base + len);
  }
  if (!donor->EraseEntry(donor_slot).ok()) {
    return false;
  }
  donor->Compact();  // 不留墓碑：IsUnderflow / num_slots 的口径才与存活条目数一致
  RecomputeFreeSpaceOn(header, payload);
  RecomputeFreeSpaceOn(donor->mutable_header(), donor->payload());
  return true;
}

Status RTreePageOps::MergeFrom(PageHeader* header, char* payload, const Page& donor,
                               ElemKey separator_for_internal) {
  // 多维树内部页没有哨兵：那条分隔条目随父页槽一起消失，不下压。
  LTM_UNUSED(separator_for_internal);
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("RTreePageOps::MergeFrom got a null pointer");
  }
  const PageHeader& dh = donor.header();
  if (dh.level != header->level) {
    return Status::Error(StatusCode::kCorruption, "the two pages being merged are not on the same level");
  }
  std::vector<std::vector<char>> moved;
  moved.reserve(dh.num_slots);
  for (uint16_t i = 0; i < dh.num_slots; ++i) {
    const Slot s = pageutil::SlotsOf(donor.payload())[i];
    if (pageutil::SlotIsEmpty(s)) {
      continue;
    }
    moved.emplace_back(donor.payload() + s.offset, donor.payload() + s.offset + s.len);
  }
  Page self(payload - kPageHeaderSize);
  for (const std::vector<char>& buf : moved) {
    const Status st = self.AppendEntry(buf.data(), static_cast<uint16_t>(buf.size()), nullptr);
    if (!st.ok()) {
      return Status::Error(st.code(),
                           std::string("R tree merge failed to move entries (a smaller capacity may be needed): ") + st.message());
    }
  }
  RecomputeFreeSpaceOn(header, payload);
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------
Status RTreePageOps::ValidatePage(const PageHeader& header, const char* payload) const {
  const uint16_t n = header.num_slots;
  if (static_cast<uint32_t>(n) * sizeof(Slot) + header.free_space > kPagePayloadSize) {
    return Status::Error(StatusCode::kCorruption, "slot directory plus free space exceeds the page capacity");
  }
  if (n > capacity_) {
    return Status::Error(StatusCode::kCorruption,
                         "entries in page " + std::to_string(n) + " exceeds capacity " +
                             std::to_string(capacity_) + " (should have been split already)");
  }
  const bool is_leaf = (header.level == 0);
  for (uint16_t i = 0; i < n; ++i) {
    const Slot s = pageutil::SlotsOf(payload)[i];
    if (pageutil::SlotIsEmpty(s)) {
      continue;
    }
    // 先做边界检查再看内容：s.offset/s.len 是磁盘上的字节，损坏的页会让它们
    // 指向条目区之外，此时任何解析都是越界读。
    if (static_cast<uint32_t>(s.offset) + s.len > kPagePayloadSize) {
      return Status::Error(StatusCode::kCorruption,
                           "slot " + std::to_string(i) + " offset out of range");
    }
    // 不变量 I5：条目必须能自描述。维度不符、长度不符都在这里被拦下 ——
    // 这是"R 树维度校验与异常路径"要求的那道门。
    REntryView v;
    if (!ParseREntry(payload + s.offset, s.len, dim_, is_leaf, &v)) {
      return Status::Error(StatusCode::kCorruption,
                           "slot " + std::to_string(i) + " entry is unparsable (len " +
                               std::to_string(s.len) + ", dim=" + std::to_string(dim_) + ")");
    }
    for (uint16_t d = 0; d < dim_; ++d) {
      const double lo = RLoAt(v, d);
      const double hi = RHiAt(v, d);
      // 用 !(lo <= hi) 而不是 (lo > hi)：NaN 也要被拒绝
      if (!(lo <= hi)) {
        return Status::Error(StatusCode::kCorruption,
                             "slot " + std::to_string(i) + " MBR in dim " + std::to_string(d) +
                                 " is degenerate (lo=" + std::to_string(lo) + " hi=" +
                                 std::to_string(hi) + ")");
      }
    }
  }
  // 【刻意不检查键序】多维树的叶内条目按分裂结果排列，与路由标签无关，
  // "键升序"在这里不是不变量（KeysAreOrdered()==false）。
  return Status::Ok();
}

// I9：父页该槽存的 MBR 必须**恰好等于**子页全部条目 MBR 的并集。
// 是"精确相等"而不是"包含"：只增不减的父 MBR 会让查询剪枝出错
// （父 MBR 大于真实并集 → 多访问一些页，不致命；但删除后不收缩会让
//  Validate 无法发现"该收缩没收缩"的结构漂移，I9 就形同虚设）。
Status RTreePageOps::CheckParentKey(const PageHeader& parent, const char* parent_payload, SlotId i,
                                    const Page& child) const {
  if (i >= parent.num_slots) {
    return InvalidArgument("CheckParentKey slot out of range");
  }
  REntryView pv;
  if (!RParseSlot(parent, parent_payload, i, dim_, &pv)) {
    return Status::Error(StatusCode::kCorruption,
                         "parent slot " + std::to_string(i) + " entry is unparsable");
  }
  RMbrAcc acc;
  acc.Reset(dim_);
  const PageHeader& ch = child.header();
  for (uint16_t s = 0; s < ch.num_slots; ++s) {
    REntryView cv;
    if (!RParseSlot(ch, child.payload(), s, dim_, &cv)) {
      continue;
    }
    std::vector<double> lo(dim_), hi(dim_);
    RReadMbr(cv, lo.data(), hi.data());
    acc.Add(lo.data(), hi.data(), dim_);
  }
  if (acc.empty) {
    // 空子页：BuildSeparator 对空页给出"零 MBR"，两边约定一致才算通过。
    // （正常结构里不会出现空子页：非根页的填充率下限是 40%，且合并会回收空页。）
    return Status::Ok();
  }
  for (uint16_t d = 0; d < dim_; ++d) {
    if (RLoAt(pv, d) != acc.lo[d] || RHiAt(pv, d) != acc.hi[d]) {
      return Status::Error(
          StatusCode::kCorruption,
          "invariant I9 violated: parent slot " + std::to_string(i) + " dim " + std::to_string(d) +
              " MBR [" + std::to_string(RLoAt(pv, d)) + ", " + std::to_string(RHiAt(pv, d)) +
              "] != child page " + std::to_string(ch.page_id) + " entry union [" +
              std::to_string(acc.lo[d]) + ", " + std::to_string(acc.hi[d]) + "]");
    }
  }
  return Status::Ok();
}

void RTreePageOps::AccumulateOverlap(const PageHeader& header, const char* payload, double* sum,
                                     uint64_t* count) const {
  if (sum == nullptr || count == nullptr || header.level == 0) {
    return;  // 只统计内部页的 MBR 重叠
  }
  const uint16_t n = header.num_slots;
  if (n < 2) {
    return;
  }
  std::vector<std::vector<double>> lo(n), hi(n);
  std::vector<bool> live(n, false);
  for (uint16_t i = 0; i < n; ++i) {
    REntryView v;
    if (!RParseSlot(header, payload, i, dim_, &v)) {
      continue;
    }
    lo[i].resize(dim_);
    hi[i].resize(dim_);
    RReadMbr(v, lo[i].data(), hi[i].data());
    live[i] = true;
  }
  for (uint16_t i = 0; i < n; ++i) {
    if (!live[i]) {
      continue;
    }
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < n; ++j) {
      if (!live[j]) {
        continue;
      }
      *sum += RIntersectionArea(lo[i].data(), hi[i].data(), lo[j].data(), hi[j].data(), dim_);
    }
  }
  ++(*count);  // 参与统计的内部页数（平均重叠面积 = sum / count）
}

// ---------------------------------------------------------------------------
// v3 挂钩
// ---------------------------------------------------------------------------
Status RTreePageOps::EncodeEntry(const IndexEntry& entry, TreeEntry* out) const {
  if (out == nullptr) {
    return InvalidArgument("RTreePageOps::EncodeEntry got a null pointer");
  }
  if (entry.dim() != dim_) {
    return Status::Error(StatusCode::kDimensionMismatch,
                         "record dim " + std::to_string(entry.dim()) + " != index dim " +
                             std::to_string(dim_));
  }
  LTM_RETURN_IF_ERROR(entry.mbr.Validate());
  if (entry.payload.size() > 0xFFFFu) {
    return InvalidArgument("payload length " + std::to_string(entry.payload.size()) + " exceeds 65535");
  }
  const size_t mbr = RBodyBytes(dim_);
  const size_t half = RHalfBytes(dim_);
  const std::vector<double>& lo = entry.mbr.lo.coords();
  const std::vector<double>& hi = entry.mbr.hi.coords();
  out->value.assign(mbr + sizeof(KeyId) + entry.payload.size(), 0);
  for (uint16_t d = 0; d < dim_; ++d) {
    std::memcpy(out->value.data() + static_cast<size_t>(d) * sizeof(double), &lo[d],
                sizeof(double));
    std::memcpy(out->value.data() + half + static_cast<size_t>(d) * sizeof(double), &hi[d],
                sizeof(double));
  }
  std::memcpy(out->value.data() + mbr, &entry.key, sizeof(KeyId));
  if (!entry.payload.empty()) {
    std::memcpy(out->value.data() + mbr + sizeof(KeyId), entry.payload.data(),
                entry.payload.size());
  }
  out->key = RZOrderOf(lo, hi, dim_);
  out->child = kInvalidPageId;  // 叶条目不指向子页
  return Status::Ok();
}

StatusOr<RecordRef> RTreePageOps::DecodeLeafEntry(const PageHeader& header, const char* payload,
                                                  SlotId slot) const {
  if (header.level != 0) {
    return Status::Error(StatusCode::kInvalidArgument, "DecodeLeafEntry needs a leaf page");
  }
  REntryView v;
  if (!RParseSlot(header, payload, slot, dim_, &v)) {
    return Status::Error(StatusCode::kCorruption,
                         "slot " + std::to_string(slot) + " leaf entry is unparsable");
  }
  RecordRef ref;
  ref.key = v.key;
  ref.mbr = Mbr::Invalid(dim_);
  for (uint16_t d = 0; d < dim_; ++d) {
    ref.mbr.lo[d] = RLoAt(v, d);
    ref.mbr.hi[d] = RHiAt(v, d);
  }
  ref.payload = v.base + v.payload_off;
  ref.payload_len = v.payload_len;
  ref.rtree_leaf = header.page_id;
  return ref;
}

SlotId RTreePageOps::FindEntry(const PageHeader& header, const char* payload,
                               const IndexEntry& probe) const {
  if (header.level != 0) {
    return kInvalidSlot;
  }
  if (probe.dim() != dim_) {
    return kInvalidSlot;
  }
  for (uint16_t i = 0; i < header.num_slots; ++i) {
    REntryView v;
    if (!RParseSlot(header, payload, i, dim_, &v)) {
      continue;
    }
    if (v.key != probe.key) {
      continue;
    }
    // (键, MBR) 双匹配：键是逻辑主键，MBR 是空间身份，两者都对上才是同一条记录
    bool same = true;
    for (uint16_t d = 0; d < dim_ && same; ++d) {
      if (RLoAt(v, d) != probe.mbr.lo[d] || RHiAt(v, d) != probe.mbr.hi[d]) {
        same = false;
      }
    }
    if (same) {
      return i;
    }
  }
  return kInvalidSlot;
}

StatusOr<LeafInsertResult> RTreePageOps::InsertLeafEntry(PageHeader* header, char* payload,
                                                         const TreeEntry& entry) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("RTreePageOps::InsertLeafEntry got a null pointer");
  }
  if (header->level != 0) {
    return Status::Error(StatusCode::kCorruption, "InsertLeafEntry got a non-leaf page");
  }
  if (entry.value.size() < RBodyBytes(dim_) + sizeof(KeyId)) {
    return InvalidArgument("leaf entry body is too short");
  }
  KeyId key = 0;
  std::memcpy(&key, entry.value.data() + RBodyBytes(dim_), sizeof(KeyId));

  Page self(payload - kPageHeaderSize);
  SlotId idx = kInvalidSlot;
  for (uint16_t i = 0; i < header->num_slots; ++i) {
    REntryView v;
    if (!RParseSlot(*header, payload, i, dim_, &v)) {
      continue;
    }
    if (v.key == key) {
      idx = i;
      break;
    }
  }
  const bool overwrote = (idx != kInvalidSlot);
  if (overwrote) {
    // 重复主键：覆盖（条目总数不变）。必须压实，否则墓碑会让 num_slots 与
    // 存活条目数分叉，下溢判定随之失真。
    LTM_RETURN_IF_ERROR(self.EraseEntry(idx));
    self.Compact();
  }
  // 叶内无序：一律追加到末尾（不维护任何序，这正是 KeysAreOrdered()==false 的含义）
  Status st = InsertEntry(header, payload, header->num_slots, entry.key, kInvalidPageId,
                          entry.value);
  if (st.code() == StatusCode::kPageFull) {
    self.Compact();
    st = InsertEntry(header, payload, header->num_slots, entry.key, kInvalidPageId, entry.value);
  }
  LTM_RETURN_IF_ERROR(st);
  return overwrote ? LeafInsertResult::kOverwrote : LeafInsertResult::kInserted;
}

SlotId RTreePageOps::ChooseChildSlot(const PageHeader& header, const char* payload,
                                     const IndexEntry& target, SlotChoice purpose) const {
  const uint16_t n = header.num_slots;
  if (n == 0) {
    return 0;
  }
  if (target.dim() != dim_) {
    return kInvalidSlot;
  }
  std::vector<double> tlo(dim_), thi(dim_);
  for (uint16_t d = 0; d < dim_; ++d) {
    tlo[d] = target.mbr.lo[d];
    thi[d] = target.mbr.hi[d];
  }
  std::vector<double> lo(dim_), hi(dim_);

  SlotId best = kInvalidSlot;
  double best_key = 0.0;
  double best_area = 0.0;
  for (uint16_t i = 0; i < n; ++i) {
    REntryView v;
    if (!RParseSlot(header, payload, i, dim_, &v)) {
      continue;
    }
    RReadMbr(v, lo.data(), hi.data());
    double metric = 0.0;
    double area = 0.0;
    if (purpose == SlotChoice::kInsert) {
      // 选叶：最小面积增量。退化 MBR（点记录）面积为 0，所有增量也都是 0 ——
      // 那时下面两级的 tie-break 才是真正的判据，不能省。
      area = RArea(lo.data(), hi.data(), dim_);
      metric = RUnionArea(lo.data(), hi.data(), tlo.data(), thi.data(), dim_) - area;
    } else {
      // 查找：优先"包含"目标的孩子，其次按相交面积从大到小
      if (RContains(lo.data(), hi.data(), tlo.data(), thi.data(), dim_)) {
        return i;
      }
      area = RIntersectionArea(lo.data(), hi.data(), tlo.data(), thi.data(), dim_);
      metric = -area;
    }
    if (best == kInvalidSlot || metric < best_key ||
        (metric == best_key && area < best_area)) {
      best = i;
      best_key = metric;
      best_area = area;
    }
  }
  return best == kInvalidSlot ? 0 : best;
}

StatusOr<std::vector<SlotId>> RTreePageOps::CandidateChildSlots(const PageHeader& header,
                                                                const char* payload,
                                                                const IndexEntry& target,
                                                                SlotChoice purpose) const {
  LTM_UNUSED(purpose);  // 相交候选与用途无关：插入只挑一个，查找/删除要试全部
  std::vector<SlotId> out;
  if (header.level == 0 || header.num_slots == 0) {
    return out;
  }
  if (target.dim() != dim_) {
    return Status::Error(StatusCode::kDimensionMismatch,
                         "probe dim " + std::to_string(target.dim()) + " != index dim " +
                             std::to_string(dim_));
  }
  std::vector<double> tlo(dim_), thi(dim_), lo(dim_), hi(dim_);
  for (uint16_t d = 0; d < dim_; ++d) {
    tlo[d] = target.mbr.lo[d];
    thi[d] = target.mbr.hi[d];
  }
  for (uint16_t i = 0; i < header.num_slots; ++i) {
    REntryView v;
    if (!RParseSlot(header, payload, i, dim_, &v)) {
      continue;
    }
    RReadMbr(v, lo.data(), hi.data());
    // 完整性依据（不能改成"只取包含的"）：父页 MBR 恰好是子页条目 MBR 的并集
    // （I9），所以目标若真在某棵子树里，它与那棵子树的 MBR 必然相交。
    // 用相交而不是包含，是因为目标 MBR 可能只是子页里某条记录的 MBR 的
    // 超集或子集 —— 例如删一条记录时 target.mbr 就是那条记录的 MBR。
    if (RIntersects(lo.data(), hi.data(), tlo.data(), thi.data(), dim_)) {
      out.push_back(i);
    }
  }
  return out;
}

TreeEntry RTreePageOps::BuildSeparator(const PageHeader& child_header,
                                       const char* child_payload) const {
  TreeEntry e;
  e.child = child_header.page_id;
  e.value.resize(RBodyBytes(dim_), 0);
  RMbrAcc acc;
  acc.Reset(dim_);
  std::vector<double> lo(dim_), hi(dim_);
  for (uint16_t i = 0; i < child_header.num_slots; ++i) {
    REntryView v;
    if (!RParseSlot(child_header, child_payload, i, dim_, &v)) {
      continue;
    }
    RReadMbr(v, lo.data(), hi.data());
    acc.Add(lo.data(), hi.data(), dim_);
  }
  if (acc.empty) {
    // 空子页：给一个退化的全零 MBR。CheckParentKey 对空子页直接放行，
    // 两边约定一致 —— 正常结构里不会出现空子页（非根页填充率下限 40%）。
    acc.lo.assign(dim_, 0.0);
    acc.hi.assign(dim_, 0.0);
  }
  for (uint16_t d = 0; d < dim_; ++d) {
    std::memcpy(e.value.data() + static_cast<size_t>(d) * sizeof(double), &acc.lo[d],
                sizeof(double));
    std::memcpy(e.value.data() + RHalfBytes(dim_) + static_cast<size_t>(d) * sizeof(double),
                &acc.hi[d], sizeof(double));
  }
  e.key = RZOrderOf(acc.lo, acc.hi, dim_);
  return e;
}

Status RTreePageOps::WriteSeparator(PageHeader* header, char* payload, SlotId slot,
                                    const TreeEntry& sep) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("RTreePageOps::WriteSeparator got a null pointer");
  }
  if (slot >= header->num_slots) {
    return InvalidArgument("WriteSeparator slot out of range");
  }
  if (sep.value.size() != RBodyBytes(dim_)) {
    return InvalidArgument("the separator entry body must be exactly 2*dim*8 bytes long");
  }
  const Slot s = pageutil::SlotsOf(payload)[slot];
  if (pageutil::SlotIsEmpty(s)) {
    return Status::Error(StatusCode::kCorruption, "WriteSeparator points at an empty slot");
  }
  REntryView v;
  if (!RParseSlot(*header, payload, slot, dim_, &v)) {
    return Status::Error(StatusCode::kCorruption, "WriteSeparator points at an unparsable entry");
  }
  // 内部条目是定长的（头 + 2*dim*8），可以原地改写 lo/hi，不搬动条目区。
  // 键（Z 码）是派生值，不落盘，因此不需要改写。
  std::memcpy(payload + s.offset + kRHeaderBytes, sep.value.data(), sep.value.size());
  return Status::Ok();
}

bool RTreePageOps::SeparatorEquals(const PageHeader& header, const char* payload, SlotId slot,
                                   const TreeEntry& sep) const {
  if (slot >= header.num_slots || sep.value.size() != RBodyBytes(dim_)) {
    return false;
  }
  REntryView v;
  if (!RParseSlot(header, payload, slot, dim_, &v)) {
    return false;
  }
  // 逐维按数值比较（不是 memcmp）：+0.0 与 -0.0 位型不同但数值相等，
  // 判等为真可以避免一次无意义的改写 —— 而"无意义改写会把本来干净的父页标脏，
  // 从而改变缓冲池的驱逐与写盘次数"，M2 的性能基线就复现不出来了。
  for (uint16_t d = 0; d < dim_; ++d) {
    double lo = 0.0;
    double hi = 0.0;
    std::memcpy(&lo, sep.value.data() + static_cast<size_t>(d) * sizeof(double), sizeof(double));
    std::memcpy(&hi, sep.value.data() + RHalfBytes(dim_) + static_cast<size_t>(d) * sizeof(double),
                sizeof(double));
    if (RLoAt(v, d) != lo || RHiAt(v, d) != hi) {
      return false;
    }
  }
  return true;
}

uint32_t RTreePageOps::EntryBytes(const TreeEntry& entry) const {
  const size_t total = kRHeaderBytes + entry.value.size() + sizeof(Slot);
  return static_cast<uint32_t>(total > 0xFFFFFFFFu ? 0xFFFFFFFFu : total);
}

// 单条预算 = 页容量 / 每页最大条目数。因为 capacity_ 已被 ByteCapacityLimit()
// 夹到 "页放得下 capacity_ 条最小条目"，这个预算必定 >= 最小条目开销，
// 于是"页在达到 capacity_ 条之前先被字节撑满"这件事不会发生 ——
// 溢出/下溢/合并的算术就和定长条目的 B+ 树完全一致。
uint32_t RTreePageOps::EntryByteBudget() const {
  if (capacity_ == 0) {
    return 0;
  }
  return static_cast<uint32_t>(kPagePayloadSize / capacity_);
}

PageId RTreePageOps::ChooseHalf(const TreeEntry& entry, PageId left, const PageHeader& left_header,
                                const char* left_payload, PageId right,
                                const PageHeader& right_header, const char* right_payload) const {
  // 分裂后把待插条目放进哪一半：与 ChooseChildSlot(kInsert) 同一判据 ——
  // 最小面积增量。用中性默认（谁放得下放谁）会把分裂退化成随机划分，
  // R 树的空间聚集性荡然无存。
  const size_t need = kRHeaderBytes + entry.value.size();
  if (entry.value.size() < RBodyBytes(dim_) + sizeof(KeyId)) {
    return kInvalidPageId;
  }
  std::vector<double> elo(dim_), ehi(dim_);
  for (uint16_t d = 0; d < dim_; ++d) {
    std::memcpy(&elo[d], entry.value.data() + static_cast<size_t>(d) * sizeof(double),
                sizeof(double));
    std::memcpy(&ehi[d], entry.value.data() + RHalfBytes(dim_) + static_cast<size_t>(d) * sizeof(double),
                sizeof(double));
  }
  std::vector<double> lo(dim_), hi(dim_);

  auto try_half = [&](PageId pid, const PageHeader& h, const char* p, double* out_metric,
                      double* out_area) -> bool {
    if (pid == kInvalidPageId || h.free_space < need) {
      return false;
    }
    RMbrAcc acc;
    acc.Reset(dim_);
    for (uint16_t i = 0; i < h.num_slots; ++i) {
      REntryView v;
      if (!RParseSlot(h, p, i, dim_, &v)) {
        continue;
      }
      RReadMbr(v, lo.data(), hi.data());
      acc.Add(lo.data(), hi.data(), dim_);
    }
    if (acc.empty) {
      *out_metric = 0.0;  // 空页：放进去就是它自己，增量为 0
      *out_area = 0.0;
      return true;
    }
    *out_area = RArea(acc.lo.data(), acc.hi.data(), dim_);
    *out_metric = RUnionArea(acc.lo.data(), acc.hi.data(), elo.data(), ehi.data(), dim_) - *out_area;
    return true;
  };

  double lm = 0.0;
  double la = 0.0;
  double rm = 0.0;
  double ra = 0.0;
  const bool lok = try_half(left, left_header, left_payload, &lm, &la);
  const bool rok = try_half(right, right_header, right_payload, &rm, &ra);
  if (!lok && !rok) {
    return kInvalidPageId;
  }
  if (!rok || (lok && (lm < rm || (lm == rm && la <= ra)))) {
    return left;
  }
  return right;
}

StatusOr<Mbr> RTreePageOps::EntryMbr(const PageHeader& header, const char* payload,
                                     SlotId slot) const {
  REntryView v;
  if (!RParseSlot(header, payload, slot, dim_, &v)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "entry is unparsable (slot " + std::to_string(slot) + ")");
  }
  Mbr m = Mbr::Invalid(dim_);
  for (uint16_t d = 0; d < dim_; ++d) {
    m.lo[d] = RLoAt(v, d);
    m.hi[d] = RHiAt(v, d);
  }
  return m;
}

// ---------------------------------------------------------------------------
// RTree 门面
//
// 结构维护半边全部转调 IndexTree（D3 指标的落点：新增树变体时
// src/index/tree_ops.cc 的 diff 为 0 行）。查询半边在这里自己实现 ——
// 单路径下钻对多维树不成立（见 tree_ops.h 的【诚实边界】）。
// ---------------------------------------------------------------------------
struct RTree::Impl {
  IndexTree tree;
  RTreePageOps* ops = nullptr;
  mutable std::mutex knn_stats_mutex;
  KnnProbeStats knn;
  LocateStats locate;

  // Keep traversal state private to each call. Publish only after all page
  // guards and callbacks have finished, including empty/error/early-stop exits.
  struct KnnStatsPublication {
    Impl& owner;
    bool brute;
    KnnProbeStats stats;
    KnnStatsPublication(Impl& impl, bool is_brute) : owner(impl), brute(is_brute) {}
    ~KnnStatsPublication() {
      std::lock_guard<std::mutex> lock(owner.knn_stats_mutex);
      if (brute) owner.knn.brute_pages = stats.brute_pages;
      else owner.knn = stats;
    }
    KnnStatsPublication(const KnnStatsPublication&) = delete;
    KnnStatsPublication& operator=(const KnnStatsPublication&) = delete;
  };
};

RTree::RTree() : impl_(new Impl()) {}
RTree::~RTree() = default;

Status RTree::Open(BufferPool* pool, TreePageOps* page_ops, const TreeBuildOptions& options,
                   TreeId tree_id) {
  if (options.mbr_dim == 0) {
    return InvalidArgument("RTree needs TreeBuildOptions::mbr_dim > 0");
  }
  // RTree 需要 RTreePageOps 提供的 dim()/split_policy()；类型不符时明确报错，
  // 而不是静默退化为通用算子（否则维度校验会失效，违反不变量 I9）。
  if (page_ops == nullptr || std::string(page_ops->name()) != std::string("rtree-page")) {
    return InvalidArgument(std::string("RTree needs an rtree-page page ops, got: ") +
                           (page_ops == nullptr ? "nullptr" : page_ops->name()));
  }
  if (pool == nullptr) {
    return InvalidArgument("RTree needs a buffer pool");
  }
  RTreePageOps* ops = static_cast<RTreePageOps*>(page_ops);
  if (ops->dim() != options.mbr_dim) {
    return Status::Error(StatusCode::kDimensionMismatch,
                         "page ops dim " + std::to_string(ops->dim()) + " != metadata dim " +
                             std::to_string(options.mbr_dim));
  }
  impl_->ops = ops;
  return impl_->tree.Open(pool, page_ops, options, tree_id);
}

Status RTree::Insert(const IndexEntry& entry) { return InsertInternal(entry, nullptr); }

StatusOr<PageId> RTree::InsertReturningLeaf(const IndexEntry& entry) {
  PageId leaf = kInvalidPageId;
  LTM_RETURN_IF_ERROR(InsertInternal(entry, &leaf));
  return leaf;
}

Status RTree::InsertInternal(const IndexEntry& entry, PageId* out_leaf) {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::Insert called before Open");
  }
  if (out_leaf != nullptr) {
    *out_leaf = kInvalidPageId;
  }
  // C1：写操作不取 C0（见 bplus_tree.cc 的 Insert 处说明）。
  if (entry.dim() != im.ops->dim()) {
    return Status::Error(StatusCode::kDimensionMismatch,
                         "record dim " + std::to_string(entry.dim()) + " != index dim " +
                             std::to_string(im.ops->dim()));
  }
  LTM_RETURN_IF_ERROR(entry.mbr.Validate());

  // 【upsert 必须是一个事务】下面的删旧 + 插新是两次门面调用，各自会开一个写事务。
  // 不裹这一层，"删掉旧的、插入新的"就裂成两个事务：中间崩溃会留下**记录消失**
  // （旧副本已删、新副本未提交）。这正是 D1 把事务放进 BufferPool 要防的
  // "提交一半"，只不过发生在单棵树内部。ScopedWriteOp 可重入，内层两次调用会
  // join 进这一个事务。
  ScopedWriteOp op(im.tree.pool());

  // 【主键唯一：R 树的 Insert 也必须是 upsert】
  // 通用引擎的插入契约是"同主键覆盖"（B+ 的 InsertLeafEntry 会返回 kOverwrote），
  // 但 R 树的**插入去向由 MBR 决定、与 key 无关**：同一条旧记录完全可能落在
  // 另一个叶页里。只做叶内去重就会留下"幽灵副本" —— Search 命中一份、ScanAll
  // 看到两份，Erase 只删掉一份，另一份永久残留。症状极具迷惑性：所有不变量都
  // 不报警，只有 num_entries 比真值表大（本次就是 934 vs 844）。
  // 因此这里先全局查一次同键条目，命中就按它的**精确 MBR** 先删掉，再走插入。
  //
  // 成本说明（不是笔误）：key 是 MBR 中心的 Z 序编码、**由 MBR 导出**，
  // 于是"这个键在不在树里"无法用 MBR 相交来剪枝，只能走叶链全扫。
  // R 树的 upsert 因此是 O(叶页数) 而非 O(log n)。这是保证 I8（混合索引中
  // 两棵树指向同一份记录集）所必须付的代价，已记入 docs/status.md 的已知局限。
  {
    PageId cur = im.tree.leftmost_leaf();
    while (cur != kInvalidPageId) {
      auto g = im.tree.Fetch(cur);
      if (!g.ok()) {
        return g.status();
      }
      PageGuard guard = std::move(g.value());
      SlotId hit = kInvalidSlot;
      for (uint16_t i = 0; i < guard.page().num_slots() && hit == kInvalidSlot; ++i) {
        if (im.ops->IsTombstone(guard.header(), guard.payload(), i)) {
          continue;
        }
        REntryView v;
        if (!RParseSlot(guard.header(), guard.payload(), i, im.ops->dim(), &v)) {
          continue;
        }
        if (v.key == entry.key) {
          hit = i;
        }
      }
      if (hit == kInvalidSlot) {
        cur = guard.header().next_page;
        continue;
      }
      // EntryMbr 返回的是**拷贝**（自带 vector），守卫释放后依然有效。
      auto old_mbr = im.ops->EntryMbr(guard.header(), guard.payload(), hit);
      if (!old_mbr.ok()) {
        return old_mbr.status();
      }
      IndexEntry stale;
      stale.key = entry.key;
      stale.mbr = old_mbr.value();
      guard.Release();
      LTM_RETURN_IF_ERROR(im.tree.Erase(stale));
      break;
    }
  }
  // 不能写成 `return im.tree.Insert(entry);`：内层 Commit 只把可重入深度减回去，
  // 真正提交要等外层。直接 return 会让本作用域的析构把它当成失败而 Abort 掉。
  LTM_RETURN_IF_ERROR(im.tree.InsertReturningLeaf(entry, out_leaf));
  return op.Commit();
}

// ---------------------------------------------------------------------------
// 混合索引入口：按键定位（方案 §4.9）
// ---------------------------------------------------------------------------
StatusOr<IndexEntry> RTree::Locate(KeyId key, PageId hint) {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::Locate called before Open");
  }
  const uint16_t dim = im.ops->dim();

  // 把页内某一条目拷成"物化"的记录（页守卫释放后依然有效）
  auto materialize = [dim](const PageHeader& h, const char* payload, SlotId slot, PageId page,
                           IndexEntry* out) -> bool {
    REntryView v;
    if (!RParseSlot(h, payload, slot, dim, &v)) {
      return false;
    }
    out->key = v.key;
    out->mbr = Mbr::Invalid(dim);
    out->mbr.lo.resize(dim);
    out->mbr.hi.resize(dim);
    for (uint16_t d = 0; d < dim; ++d) {
      out->mbr.lo[d] = RLoAt(v, d);
      out->mbr.hi[d] = RHiAt(v, d);
    }
    out->payload.assign(v.base + v.payload_off, v.base + v.payload_off + v.payload_len);
    out->rtree_leaf = page;
    return true;
  };

  // 1) 先信 hint —— 但必须**校验**：是 R 树叶页、属于本树、页里确实有这条键。
  //    三个条件缺一不可。少了"属于本树"这一条，一个被释放后重新分配给 B+ 树的
  //    页号会被当成 R 树叶页解析，读出的是别的树的字节。
  if (hint != kInvalidPageId) {
    auto g = im.tree.Fetch(hint);
    if (g.ok()) {
      PageGuard guard = std::move(g.value());
      const bool is_rtree_leaf = guard.header().level == 0 &&
                                 guard.header().type == PageType::kRTreeLeaf &&
                                 guard.header().tree_id == im.tree.tree_id();
      if (is_rtree_leaf) {
        for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
          REntryView v;
          if (!RParseSlot(guard.header(), guard.payload(), i, dim, &v)) {
            continue;
          }
          if (v.key != key) {
            continue;
          }
          IndexEntry found;
          if (materialize(guard.header(), guard.payload(), i, hint, &found)) {
            ++im.locate.hint_hits;
            return found;
          }
        }
      }
    }
    ++im.locate.hint_misses;
  }

  // 2) 退化：叶链全扫。R 树的叶内条目按分裂结果排列、与逻辑键无关，
  //    所以这里没有"定位到起点后顺序扫描"这回事，只能逐页逐槽找。
  ++im.locate.chain_scans;
  PageId cur = im.tree.leftmost_leaf();
  while (cur != kInvalidPageId) {
    auto g = im.tree.Fetch(cur);
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
      REntryView v;
      if (!RParseSlot(guard.header(), guard.payload(), i, dim, &v)) {
        continue;
      }
      if (v.key != key) {
        continue;
      }
      IndexEntry found;
      if (materialize(guard.header(), guard.payload(), i, cur, &found)) {
        return found;
      }
    }
    cur = guard.header().next_page;
  }
  return NotFound("R-tree record not found: key=" + std::to_string(key));
}

const RTree::LocateStats& RTree::locate_stats() const { return impl_->locate; }

void RTree::ResetLocateStats() { impl_->locate = LocateStats{}; }

Status RTree::Erase(const IndexEntry& entry) {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::Erase called before Open");
  }
  // C1：不取 C0（见 bplus_tree.cc 的 Insert 处说明）。
  if (entry.dim() != im.ops->dim()) {
    return Status::Error(StatusCode::kDimensionMismatch,
                         "record dim " + std::to_string(entry.dim()) + " != index dim " +
                             std::to_string(im.ops->dim()));
  }
  // 结构维护半边（含下溢借键/合并/根降级）由 IndexTree 承担；
  // 定位走 CandidateChildSlots，因此"记录可能在多个重叠子树里"由页算子表达。
  return im.tree.Erase(entry);
}

StatusOr<RecordRef> RTree::Search(const IndexEntry& probe) {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::Search called before Open");
  }
  // C1：读操作不取 C0；页闩由栈式遍历逐页取（共享）。
  if (probe.dim() != im.ops->dim()) {
    return Status::Error(StatusCode::kDimensionMismatch, "the probe dimension does not match the index dimension");
  }
  if (im.tree.root() == kInvalidPageId) {
    return Status::Error(StatusCode::kCorruption, "root is invalid");
  }

  // 深度优先，**展开**所有与探针相交的孩子。绝不能复用单路径下钻：
  // 一个探针可能同时落在多个孩子里，记录只存在其中一个 ——
  // 单路径会静默漏记录且不触发任何不变量报警（tree_ops.h【诚实边界】）。
  std::vector<PageId> stack;
  stack.push_back(im.tree.root());
  while (!stack.empty()) {
    const PageId pid = stack.back();
    stack.pop_back();
    auto g = im.tree.Fetch(pid);
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    if (guard.header().level == 0) {
      const SlotId s = im.ops->FindEntry(guard.header(), guard.payload(), probe);
      if (s != kInvalidSlot) {
        // 【必须在守卫还活着的时候把载荷拷出来】与 B+ 同理：返回时 guard 已经
        // 析构，payload 若仍指向页帧就是 use-after-free。见 RecordRef::owned_payload。
        auto ref = im.ops->DecodeLeafEntry(guard.header(), guard.payload(), s);
        if (!ref.ok()) {
          return ref.status();
        }
        return WithOwnedPayload(ref.value());
      }
      continue;
    }
    auto cand =
        im.ops->CandidateChildSlots(guard.header(), guard.payload(), probe, SlotChoice::kSearch);
    if (!cand.ok()) {
      return cand.status();
    }
    for (SlotId s : cand.value()) {
      const PageId c = im.ops->GetChild(guard.header(), guard.payload(), s);
      if (c != kInvalidPageId) {
        stack.push_back(c);
      }
    }
  }
  return NotFound("record not found: key=" + std::to_string(probe.key));
}

Status RTree::RangeScan(const Query& query, const std::function<bool(const RecordRef&)>& emit) {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::RangeScan called before Open");
  }
  // 多维树的叶内条目按分裂结果排列，与逻辑键无关（KeysAreOrdered()==false），
  // 不存在"定位到起点后顺序扫描"这回事。这里退化为全扫 + 键过滤。
  return ScanAll([&](const RecordRef& r) {
    if (r.key < query.key_lo || r.key > query.key_hi) {
      return true;
    }
    return emit(r);
  });
}

Status RTree::SpatialQuery(const Query& query,
                           const std::function<bool(const RecordRef&)>& emit) {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::SpatialQuery called before Open");
  }
  // C1：读操作不取 C0；页闩由遍历内部逐页取（共享）。
  const uint16_t dim = im.ops->dim();
  if (query.mbr.lo.dim() != dim || query.mbr.hi.dim() != dim) {
    return Status::Error(StatusCode::kDimensionMismatch, "the query range dimension does not match the index dimension");
  }
  if (im.tree.root() == kInvalidPageId) {
    return Status::Error(StatusCode::kCorruption, "root is invalid");
  }
  // 谓词由查询种类决定：kRangeSpatial = 记录 MBR 被查询框**包含**；
  // 其余（kIntersect / 其它）一律按**相交**处理（保守且单调）。
  const bool need_contain = (query.kind == QueryKind::kRangeSpatial);
  const std::vector<double>& qlo = query.mbr.lo.coords();
  const std::vector<double>& qhi = query.mbr.hi.coords();
  std::vector<double> lo(dim);
  std::vector<double> hi(dim);

  std::vector<PageId> stack;
  stack.push_back(im.tree.root());
  bool stop = false;
  while (!stack.empty() && !stop) {
    const PageId pid = stack.back();
    stack.pop_back();
    auto g = im.tree.Fetch(pid);
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    const bool is_leaf = (guard.header().level == 0);
    if (is_leaf) {
      std::vector<RecordRef> batch;
      for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
        REntryView v;
        if (!RParseSlot(guard.header(), guard.payload(), i, dim, &v)) {
          continue;
        }
        RReadMbr(v, lo.data(), hi.data());
        const bool hit = need_contain
                             ? RContains(qlo.data(), qhi.data(), lo.data(), hi.data(), dim)
                             : RIntersects(qlo.data(), qhi.data(), lo.data(), hi.data(), dim);
        if (!hit) {
          continue;
        }
        auto ref = im.ops->DecodeLeafEntry(guard.header(), guard.payload(), i);
        if (!ref.ok()) {
          continue;
        }
        batch.push_back(WithOwnedPayload(ref.value()));
      }
      guard.Release();
      for (const auto& ref : batch) {
        if (!emit(ref)) { stop = true; break; }
      }
      continue;
    }
    // 内部页：只进入与查询框相交的孩子 —— 父页 MBR 是子页条目 MBR 的精确并集
    // （I9），所以"父页 MBR 相交"是"子树内存在候选"的**必要**条件，剪枝安全。
    for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
      REntryView v;
      if (!RParseSlot(guard.header(), guard.payload(), i, dim, &v)) {
        continue;
      }
      RReadMbr(v, lo.data(), hi.data());
      if (RIntersects(qlo.data(), qhi.data(), lo.data(), hi.data(), dim)) {
        if (v.child != kInvalidPageId) {
          stack.push_back(v.child);
        }
      }
    }
  }
  return Status::Ok();
}

Status RTree::Knn(const Query& query, const std::function<bool(const RecordRef&, double)>& emit) {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::Knn called before Open");
  }
  // C1：读操作不取 C0；页闩由遍历内部逐页取（共享）。
  const uint16_t dim = im.ops->dim();
  if (query.probe.dim() != dim) {
    return Status::Error(StatusCode::kDimensionMismatch, "the KNN probe point dimension does not match the index dimension");
  }
  if (im.tree.root() == kInvalidPageId) {
    return Status::Error(StatusCode::kCorruption, "root is invalid");
  }
  const uint32_t k = query.k == 0 ? 1u : query.k;
  const std::vector<double>& p = query.probe.coords();

  struct Hit {
    double dist = 0.0;
    KeyId key = 0;
    std::vector<char> payload;
    Mbr mbr;
    PageId leaf = kInvalidPageId;
  };
  struct Node {
    double dist = 0.0;
    PageId pid = kInvalidPageId;
  };
  struct NodeCmp {
    bool operator()(const Node& a, const Node& b) const { return a.dist > b.dist; }  // 小顶堆
  };
  const auto better = [](const Hit& a, const Hit& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.key < b.key;
  };

  Impl::KnnStatsPublication probe(im, false);
  std::priority_queue<Node, std::vector<Node>, NodeCmp> queue;
  // A max-heap of the best k complete (distance, key) ranks. The worst retained
  // rank is available in O(1); admitting a better candidate takes O(log k).
  std::vector<Hit> hits;
  std::vector<double> lo(dim);
  std::vector<double> hi(dim);

  {
    auto g = im.tree.Fetch(im.tree.root());
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    RMbrAcc acc;
    acc.Reset(dim);
    for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
      REntryView v;
      if (!RParseSlot(guard.header(), guard.payload(), i, dim, &v)) {
        continue;
      }
      RReadMbr(v, lo.data(), hi.data());
      acc.Add(lo.data(), hi.data(), dim);
    }
    if (acc.empty) {
      return Status::Ok();  // 空树
    }
    queue.push(Node{RPointDistance(acc.lo.data(), acc.hi.data(), p.data(), dim),
                    im.tree.root()});
  }

  while (!queue.empty()) {
    const Node node = queue.top();
    queue.pop();
    // 剪枝：队列按"到子树的最近可能距离"排序，堆顶已经比第 k 名还远，
    // 队列里其余节点只会更远 —— 整个剩余队列都可以丢弃。
    if (hits.size() >= k && node.dist > hits.front().dist) {
      break;
    }
    auto g = im.tree.Fetch(node.pid);
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    ++probe.stats.pages_visited;
    if (guard.header().level == 0) {
      for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
        REntryView v;
        if (!RParseSlot(guard.header(), guard.payload(), i, dim, &v)) {
          continue;
        }
        RReadMbr(v, lo.data(), hi.data());
        ++probe.stats.entries_scanned;
        const double d = RPointDistance(lo.data(), hi.data(), p.data(), dim);
        if (hits.size() >= k) {
          const Hit& worst = hits.front();
          // Equal distances still compete on key, including across pages.
          if (d > worst.dist || (d == worst.dist && v.key >= worst.key)) continue;
        }
        auto ref = im.ops->DecodeLeafEntry(guard.header(), guard.payload(), i);
        if (!ref.ok()) {
          continue;
        }
        Hit h;
        h.dist = d;
        h.key = ref.value().key;
        h.mbr = ref.value().mbr;
        h.leaf = guard.header().page_id;
        h.payload.assign(ref.value().payload, ref.value().payload + ref.value().payload_len);
        if (hits.size() >= k) {
          std::pop_heap(hits.begin(), hits.end(), better);
          hits.back() = std::move(h);
        } else {
          hits.push_back(std::move(h));
        }
        std::push_heap(hits.begin(), hits.end(), better);
      }
      continue;
    }
    for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
      REntryView v;
      if (!RParseSlot(guard.header(), guard.payload(), i, dim, &v)) {
        continue;
      }
      RReadMbr(v, lo.data(), hi.data());
      const double d = RPointDistance(lo.data(), hi.data(), p.data(), dim);
      if (hits.size() >= k && d > hits.front().dist) {
        continue;  // 下界已经超过第 k 名，整棵子树不可能有更近的
      }
      if (v.child != kInvalidPageId) {
        queue.push(Node{d, v.child});
      }
    }
  }

  // 输出顺序按 (距离, 键) 排定 —— 与 KnnBruteForce 完全一致，
  // 这样"两者等价"的断言不会被浮点并列打乱。
  std::sort_heap(hits.begin(), hits.end(), better);
  for (const Hit& h : hits) {
    RecordRef ref;
    ref.key = h.key;
    ref.mbr = h.mbr;
    ref.payload = h.payload.data();
    ref.payload_len = static_cast<uint32_t>(h.payload.size());
    ref.rtree_leaf = h.leaf;
    if (!emit(ref, h.dist)) {
      return Status::Ok();
    }
  }
  return Status::Ok();
}

Status RTree::KnnBruteForce(const Query& query,
                            const std::function<bool(const RecordRef&, double)>& emit) const {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::KnnBruteForce called before Open");
  }
  const uint16_t dim = im.ops->dim();
  if (query.probe.dim() != dim) {
    return Status::Error(StatusCode::kDimensionMismatch, "the KNN probe point dimension does not match the index dimension");
  }
  const uint32_t k = query.k == 0 ? 1u : query.k;
  const std::vector<double>& p = query.probe.coords();

  struct Hit {
    double dist = 0.0;
    KeyId key = 0;
    std::vector<char> payload;
    Mbr mbr;
    PageId leaf = kInvalidPageId;
  };
  std::vector<Hit> hits;
  std::vector<double> lo(dim);
  std::vector<double> hi(dim);
  Impl::KnnStatsPublication probe(im, true);

  // 全量叶链扫描：不使用任何剪枝，作为 KNN 的等价性基准
  PageId cur = im.tree.leftmost_leaf();
  while (cur != kInvalidPageId) {
    auto g = im.tree.Fetch(cur);
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    ++probe.stats.brute_pages;
    for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
      REntryView v;
      if (!RParseSlot(guard.header(), guard.payload(), i, dim, &v)) {
        continue;
      }
      RReadMbr(v, lo.data(), hi.data());
      auto ref = im.ops->DecodeLeafEntry(guard.header(), guard.payload(), i);
      if (!ref.ok()) {
        continue;
      }
      Hit h;
      h.dist = RPointDistance(lo.data(), hi.data(), p.data(), dim);
      h.key = ref.value().key;
      h.mbr = ref.value().mbr;
      h.leaf = guard.header().page_id;
      h.payload.assign(ref.value().payload, ref.value().payload + ref.value().payload_len);
      hits.push_back(std::move(h));
    }
    cur = guard.header().next_page;
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    if (a.dist != b.dist) {
      return a.dist < b.dist;
    }
    return a.key < b.key;
  });
  const size_t limit = hits.size() < k ? hits.size() : k;
  for (size_t i = 0; i < limit; ++i) {
    RecordRef ref;
    ref.key = hits[i].key;
    ref.mbr = hits[i].mbr;
    ref.payload = hits[i].payload.data();
    ref.payload_len = static_cast<uint32_t>(hits[i].payload.size());
    ref.rtree_leaf = hits[i].leaf;
    if (!emit(ref, hits[i].dist)) {
      return Status::Ok();
    }
  }
  return Status::Ok();
}

Status RTree::ScanAll(const std::function<bool(const RecordRef&)>& emit) {
  Impl& im = *impl_;
  if (im.ops == nullptr) {
    return Status::Error(StatusCode::kCorruption, "RTree::ScanAll called before Open");
  }
  // C1：不取 C0。叶链遍历每页一次共享闩，读完先放闩再取下一页。
  if (im.tree.root() == kInvalidPageId) {
    return Status::Error(StatusCode::kCorruption, "root is invalid");
  }
  // R 树也有叶链（HasLeafChain()==true）：next_page/prev_page 由通用层维护，
  // 顺序只是"页的物理顺序"，与逻辑键无关 —— 调用方不得依赖顺序。
  PageId cur = im.tree.leftmost_leaf();
  while (cur != kInvalidPageId) {
    auto g = im.tree.Fetch(cur);
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    if (guard.header().level != 0) {
      return Status::Error(StatusCode::kCorruption, "a non-leaf page appeared on the leaf chain");
    }
    std::vector<RecordRef> batch;
    for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
      if (im.ops->IsTombstone(guard.header(), guard.payload(), i)) {
        continue;
      }
      auto ref = im.ops->DecodeLeafEntry(guard.header(), guard.payload(), i);
      if (!ref.ok()) {
        continue;
      }
      batch.push_back(WithOwnedPayload(ref.value()));
    }
    cur = guard.header().next_page;
    guard.Release();
    for (const auto& ref : batch) if (!emit(ref)) return Status::Ok();
  }
  return Status::Ok();
}

Status RTree::Validate() { return impl_->tree.Validate(); }

TreeStats RTree::stats() const { return impl_->tree.stats(); }

Status RTree::Destroy() { return impl_->tree.Destroy(); }

RTree::KnnProbeStats RTree::knn_stats() const {
  std::lock_guard<std::mutex> lock(impl_->knn_stats_mutex);
  return impl_->knn;
}

}  // namespace ltm

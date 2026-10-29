// src/index/bplus_tree.h
// 页式 B+ 树（方案 §4.7）。
//
// 页布局（payload 区，紧跟 32B PageHeader；页尾 16B 校验区见 page.h）：
//
//   +0   uint16 nkeys                      ---- 可选的自描述计数（便于断言）----
//   +4   槽目录 slot[0..n-1]  （每槽 4B：offset:u16, len:u16）
//   ...                                    ---- 向下增长 ----
//        (free space)
//        ---- 条目区向上增长 ----
//        叶条目  <key:u64 | rtree_leaf_page:u32 | payload_len:u16 | payload...>
//        内部条目<key:u64 | child:u32>
//   末尾  最后一个条目的最后一个字节
//
// 关键约定：
//   * 槽序 == 键序（升序），因此页内查找可二分。
//   * 内部页 keys[i] == children[i] 子树的最小键；children[0] 为最左子页，
//     其 keys[0] 填 kMinElemKey 哨兵（不参与比较、不提升给父页）。
//   * 叶页 next_page/prev_page 双向串联，范围扫描沿 next_page 推进。
//
// 关键不变量（tests/index/test_bplus_validate.cc 逐条自检）：
//   I1 非叶页第 i 键 == 第 i 子页最小键（中期缺陷的页式形态）
//   I2 叶链 next/prev 双向一致且键严格升序
//   I3 任意非根页填充率 >= 25%（页容量的一半以下即视为下溢）
//   I4 根到所有叶路径等长
#pragma once

#include <vector>

#include "index/tree_ops.h"

namespace ltm {

// ---------------------------------------------------------------------------
// 页内条目布局（POD；写盘用 memcpy，不做 reinterpret_cast 以避免对齐问题）
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct BPlusLeafEntry {
  ElemKey key;             // 8  键（同时是逻辑主键）
  PageId rtree_leaf_page;  // 4  指向 R 树叶页（方案 §4.9：取代独立键→条目映射）
  uint16_t payload_len;    // 2
  // char payload[payload_len];
};
struct BPlusInternalEntry {
  ElemKey key;   // 8 == children[i] 子树的最小键
  PageId child;  // 4
};
#pragma pack(pop)
static_assert(sizeof(BPlusLeafEntry) == 14, "BPlusLeafEntry layout changed");
static_assert(sizeof(BPlusInternalEntry) == 12, "BPlusInternalEntry layout changed");

constexpr uint16_t kBPlusMinCapacity = 4;

inline uint16_t BPlusLeafEntrySize(uint16_t payload_len) {
  return static_cast<uint16_t>(sizeof(BPlusLeafEntry) + payload_len);
}

// ---------------------------------------------------------------------------
// 页内算子
//
// 只实现**页内**语义：条目布局、槽序、分裂怎么切、借键往哪端插、合并怎么下压
// 分隔键、I1 的局部形态怎么校验。跨页算法（下钻/传播/下溢循环/根升降/自检骨架）
// 一律不在这里 —— 它们只写一遍，在 index/tree_ops.cc 的 IndexTree 里。
// ---------------------------------------------------------------------------
class BPlusPageOps final : public TreePageOps {
 public:
  explicit BPlusPageOps(uint16_t capacity);

  const char* name() const override { return "bplus-page"; }

  // ---- 树身份 ----
  MetaTreeKind MetaKind() const override { return MetaTreeKind::kBPlus; }
  PageType PageTypeForLevel(uint8_t level) const override {
    return level == 0 ? PageType::kBPlusLeaf : PageType::kBPlusInternal;
  }

  // ---- 页元数据 ----
  bool IsLeaf(const PageHeader& header) const override { return header.level == 0; }
  uint8_t Level(const PageHeader& header) const override { return header.level; }
  uint16_t Capacity() const override { return capacity_; }
  ElemKey FirstKey(const PageHeader& header, const char* payload) const override;
  // 0 号槽是最左子页哨兵：跨页算法刷新分隔条目时从 1 号槽起，
  // 且 InsertChildAt 的 0 号槽要写哨兵键。
  bool HasLeftmostSentinel() const override { return true; }
  // 一维键：插入不会改变父页分隔条目的值（既定的路由下，子页最小键只可能
  // 在删除时变大），所以一条也不刷新。覆写为 true 会让每层多一次抓取并
  // 把性能基线改掉，而收益是零。
  bool SeparatorMayChangeOnInsert() const override { return false; }

  // ---- 条目访问 ----
  SlotId LowerBound(const PageHeader& header, const char* payload, ElemKey key) const override;
  ElemKey GetKey(const PageHeader& header, const char* payload, SlotId slot) const override;
  PageId GetChild(const PageHeader& header, const char* payload, SlotId slot) const override;

  // ---- 页生命周期 ----
  Status InitPage(PageHeader* header, char* payload, bool is_leaf) override;

  // ---- 修改 ----
  Status InsertEntry(PageHeader* header, char* payload, SlotId at, ElemKey key, PageId child,
                     const std::vector<char>& value) override;
  Status EraseSlot(PageHeader* header, char* payload, SlotId slot) override;

  bool IsOverflow(const PageHeader& header, const char* payload) const override;
  bool IsUnderflow(const PageHeader& header, const char* payload) const override;
  // UnderflowThreshold 用基类默认实现（容量/2，下限 2），与 IsUnderflow 同口径。

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
  Status WriteSeparator(PageHeader* header, char* payload, SlotId slot,
                        const TreeEntry& sep) override;
  bool SeparatorEquals(const PageHeader& header, const char* payload, SlotId slot,
                       const TreeEntry& sep) const override;
  uint32_t EntryBytes(const TreeEntry& entry) const override;

  // ---- 供 BPlusTree 使用的叶条目 payload 读取 ----
  StatusOr<std::pair<PageId, const char*>> LeafPayload(const PageHeader& header,
                                                      const char* payload, SlotId slot) const;

 private:
  uint16_t capacity_;
};

// ---------------------------------------------------------------------------
// B+ 树 —— 薄门面
//
// 结构维护（插入/删除/分裂/借键/合并/根升降/自检/销毁）全部转调 IndexTree，
// 本类只保留两件自己的事：
//   1. 参数校验（一维记录、算子类型）
//   2. 一维树自己的查询半边：点查、区间扫描（单路径下钻对一维树成立）
// ---------------------------------------------------------------------------
class BPlusTree final : public TreeOps {
 public:
  BPlusTree();
  ~BPlusTree() override;

  const char* name() const override { return "bplus"; }

  Status Open(BufferPool* pool, TreePageOps* page_ops, const TreeBuildOptions& options,
              TreeId tree_id) override;
  Status Insert(const IndexEntry& entry) override;
  Status Erase(const IndexEntry& entry) override;
  StatusOr<RecordRef> Search(const IndexEntry& probe) override;
  Status RangeScan(const Query& query, const std::function<bool(const RecordRef&)>& emit) override;
  Status ScanAll(const std::function<bool(const RecordRef&)>& emit) override;
  Status Validate() override;
  TreeStats stats() const override;
  Status Destroy() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm

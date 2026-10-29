// src/index/mock_tree.h
// MockTree —— "通用性"验收用的最小树变体（方案 D3 验收项）。
//
// 它的存在只有一个目的：
//   证明「新增一个树变体只需实现 TreeOps / TreePageOps，不必改动 src/storage/**」。
// 因此它是一个**纯内存**、**不碰缓冲池**的实现：
//   * 不申请页、不写 WAL、不触发 I/O
//   * TreePageOps 实现全部返回 kNotSupported（它不用页）
//
// tests/index/test_registry.cc 用它在运行期验证注册表机制与契约可接入性。
#pragma once

#include <map>

#include "index/tree_ops.h"

namespace ltm {

class MockPageOps final : public TreePageOps {
 public:
  const char* name() const override { return "mock-page"; }

  // ---- 树身份 ----
  MetaTreeKind MetaKind() const override { return MetaTreeKind::kMock; }
  PageType PageTypeForLevel(uint8_t) const override { return PageType::kFree; }

  // ---- 页元数据 ----
  bool IsLeaf(const PageHeader&) const override { return true; }
  uint8_t Level(const PageHeader&) const override { return 0; }
  uint16_t Capacity() const override { return 0; }
  ElemKey FirstKey(const PageHeader&, const char*) const override { return kMinElemKey; }

  // ---- 条目访问 ----
  SlotId LowerBound(const PageHeader&, const char*, ElemKey) const override { return 0; }
  ElemKey GetKey(const PageHeader&, const char*, SlotId) const override { return kMinElemKey; }
  PageId GetChild(const PageHeader&, const char*, SlotId) const override { return kInvalidPageId; }

  // ---- 页生命周期 / 修改 ----
  Status InitPage(PageHeader*, char*, bool) override;
  Status InsertEntry(PageHeader*, char*, SlotId, ElemKey, PageId,
                     const std::vector<char>&) override;
  Status EraseSlot(PageHeader*, char*, SlotId) override;

  bool IsOverflow(const PageHeader&, const char*) const override { return false; }
  bool IsUnderflow(const PageHeader&, const char*) const override { return false; }

  // ---- 分裂 / 借键 / 合并 ----
  Status SplitInto(PageHeader*, char*, Page*) override;
  bool BorrowFrom(PageHeader*, char*, Page*, BorrowDirection, const TreeEntry&,
                  TreeEntry*) override {
    return false;
  }
  Status MergeFrom(PageHeader*, char*, const Page&, ElemKey) override;

  // ---- 自检 ----
  Status ValidatePage(const PageHeader&, const char*) const override;
  Status CheckParentKey(const PageHeader&, const char*, SlotId, const Page&) const override;

  // ---- v3 挂钩：纯内存树不使用页，全部以 kNotSupported 明确拒绝。
  // 这个"全部拒绝"本身就是验收的一部分：它证明新增树变体时**不必**实现任何
  // 页内语义也能通过契约编译，反过来说明跨页算法没有偷偷依赖某棵树的行为。
  Status EncodeEntry(const IndexEntry&, TreeEntry*) const override;
  StatusOr<RecordRef> DecodeLeafEntry(const PageHeader&, const char*, SlotId) const override;
  SlotId FindEntry(const PageHeader&, const char*, const IndexEntry&) const override {
    return kInvalidSlot;
  }
  StatusOr<LeafInsertResult> InsertLeafEntry(PageHeader*, char*, const TreeEntry&) override;
  SlotId ChooseChildSlot(const PageHeader&, const char*, const IndexEntry&, SlotChoice) const
      override {
    return 0;
  }
  Status WriteSeparator(PageHeader*, char*, SlotId, const TreeEntry&) override;
  bool SeparatorEquals(const PageHeader&, const char*, SlotId, const TreeEntry&) const override {
    return true;
  }
  uint32_t EntryBytes(const TreeEntry&) const override { return 0; }
};

// 纯内存实现：Open 之后即可 Insert/Search/ScanAll，无需 BufferPool
class MockTree final : public TreeOps {
 public:
  MockTree();
  ~MockTree() override;

  const char* name() const override { return "mock"; }

  Status Open(BufferPool* pool, TreePageOps* page_ops, const TreeBuildOptions& options,
              TreeId tree_id) override;
  Status Insert(const IndexEntry& entry) override;
  Status Erase(const IndexEntry& entry) override;
  StatusOr<RecordRef> Search(const IndexEntry& probe) override;
  Status RangeScan(const Query& query, const std::function<bool(const RecordRef&)>& emit) override;
  Status SpatialQuery(const Query& query,
                      const std::function<bool(const RecordRef&)>& emit) override;
  Status ScanAll(const std::function<bool(const RecordRef&)>& emit) override;
  Status Validate() override;
  TreeStats stats() const override;
  Status Destroy() override;

  // 供测试断言：Search 命中的条目直接取自内存表（避免悬垂指针）
  StatusOr<IndexEntry> GetCopy(KeyId key) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm

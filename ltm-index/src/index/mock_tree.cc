#include "index/mock_tree.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "util/log.h"

namespace ltm {

// ---------------------------------------------------------------------------
// MockPageOps：纯内存树不申请页，因此这些算子只用于满足契约
// ---------------------------------------------------------------------------
namespace {
Status NoPages() {
  return Status::Error(StatusCode::kNotSupported, "MockTree is a pure in-memory implementation and uses no pages");
}
}  // namespace

Status MockPageOps::InitPage(PageHeader*, char*, bool) { return NoPages(); }
Status MockPageOps::InsertEntry(PageHeader*, char*, SlotId, ElemKey, PageId,
                                const std::vector<char>&) {
  return NoPages();
}
Status MockPageOps::EraseSlot(PageHeader*, char*, SlotId) { return NoPages(); }
Status MockPageOps::SplitInto(PageHeader*, char*, Page*) { return NoPages(); }
Status MockPageOps::MergeFrom(PageHeader*, char*, const Page&, ElemKey) { return NoPages(); }
Status MockPageOps::ValidatePage(const PageHeader&, const char*) const { return NoPages(); }
Status MockPageOps::CheckParentKey(const PageHeader&, const char*, SlotId, const Page&) const {
  return NoPages();
}
Status MockPageOps::EncodeEntry(const IndexEntry&, TreeEntry*) const { return NoPages(); }
StatusOr<RecordRef> MockPageOps::DecodeLeafEntry(const PageHeader&, const char*, SlotId) const {
  return NoPages();
}
StatusOr<LeafInsertResult> MockPageOps::InsertLeafEntry(PageHeader*, char*, const TreeEntry&) {
  return NoPages();
}
Status MockPageOps::WriteSeparator(PageHeader*, char*, SlotId, const TreeEntry&) {
  return NoPages();
}

// ---------------------------------------------------------------------------
// MockTree：完整可用的内存实现（"第三种树变体"的最小样例）
// ---------------------------------------------------------------------------
struct MockTree::Impl {
  std::map<KeyId, IndexEntry> table;
  TreeBuildOptions options;
  TreeId tree_id = 0;
  bool opened = false;
};

MockTree::MockTree() : impl_(new Impl()) {}
MockTree::~MockTree() = default;

Status MockTree::Open(BufferPool*, TreePageOps* page_ops, const TreeBuildOptions& options,
                      TreeId tree_id) {
  LTM_UNUSED(page_ops);
  impl_->options = options;
  impl_->tree_id = tree_id;
  impl_->opened = true;
  LOG_INFO("MockTree opened (tree_id=", tree_id, ")");
  return Status::Ok();
}

Status MockTree::Insert(const IndexEntry& entry) {
  if (!impl_->opened) {
    return Status::Error(StatusCode::kCorruption, "MockTree::Insert called before Open");
  }
  LTM_RETURN_IF_ERROR(entry.Validate());
  impl_->table[entry.key] = entry;
  return Status::Ok();
}

Status MockTree::Erase(const IndexEntry& entry) {
  if (impl_->table.erase(entry.key) == 0) {
    return NotFound("MockTree has no such key, key=" + std::to_string(entry.key));
  }
  return Status::Ok();
}

StatusOr<RecordRef> MockTree::Search(const IndexEntry& probe) {
  auto it = impl_->table.find(probe.key);
  if (it == impl_->table.end()) {
    return NotFound("MockTree: no such key, key=" + std::to_string(probe.key));
  }
  RecordRef ref;
  ref.key = it->second.key;
  ref.mbr = it->second.mbr;
  ref.payload = it->second.payload.data();
  ref.payload_len = static_cast<uint32_t>(it->second.payload.size());
  // MockTree 的载荷指向 map 里的 vector（本身比页帧稳），但 Search 的契约是
  // "返回即自持"，这里统一走同一条路，免得两个树变体在这一点上语义不同。
  return WithOwnedPayload(ref);
}

StatusOr<IndexEntry> MockTree::GetCopy(KeyId key) const {
  auto it = impl_->table.find(key);
  if (it == impl_->table.end()) {
    return NotFound("MockTree: no such key, key=" + std::to_string(key));
  }
  return it->second;
}

Status MockTree::RangeScan(const Query& query,
                           const std::function<bool(const RecordRef&)>& emit) {
  for (const auto& kv : impl_->table) {
    if (kv.first < query.key_lo) {
      continue;
    }
    if (kv.first > query.key_hi) {
      break;
    }
    RecordRef ref;
    ref.key = kv.second.key;
    ref.mbr = kv.second.mbr;
    ref.payload = kv.second.payload.data();
    ref.payload_len = static_cast<uint32_t>(kv.second.payload.size());
    if (!emit(ref)) {
      break;
    }
  }
  return Status::Ok();
}

Status MockTree::SpatialQuery(const Query&, const std::function<bool(const RecordRef&)>&) {
  return Status::Error(StatusCode::kNotSupported, "MockTree does not support spatial queries");
}

Status MockTree::ScanAll(const std::function<bool(const RecordRef&)>& emit) {
  for (const auto& kv : impl_->table) {
    RecordRef ref;
    ref.key = kv.second.key;
    ref.mbr = kv.second.mbr;
    ref.payload = kv.second.payload.data();
    ref.payload_len = static_cast<uint32_t>(kv.second.payload.size());
    if (!emit(ref)) {
      break;
    }
  }
  return Status::Ok();
}

Status MockTree::Validate() { return Status::Ok(); }

TreeStats MockTree::stats() const {
  TreeStats s;
  s.num_entries = impl_->table.size();
  s.num_pages = impl_->table.empty() ? 0 : 1;
  s.order_or_capacity = impl_->options.order_or_capacity;
  s.dim = impl_->options.mbr_dim;
  return s;
}

Status MockTree::Destroy() {
  impl_->table.clear();
  impl_->opened = false;
  return Status::Ok();
}

}  // namespace ltm

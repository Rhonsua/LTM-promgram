#include "storage/page.h"

#include <cstring>

namespace ltm {

const char* PageTypeName(PageType t) {
  switch (t) {
    case PageType::kFree: return "kFree";
    case PageType::kMeta: return "kMeta";
    case PageType::kBPlusInternal: return "kBPlusInternal";
    case PageType::kBPlusLeaf: return "kBPlusLeaf";
    case PageType::kRTreeInternal: return "kRTreeInternal";
    case PageType::kRTreeLeaf: return "kRTreeLeaf";
    case PageType::kOverflow: return "kOverflow";
  }
  return "kUnknown";
}

namespace {

inline uint32_t SlotDirBytes(uint16_t n) {
  return static_cast<uint32_t>(n) * static_cast<uint32_t>(sizeof(Slot));
}

// 条目区从页尾部向上生长；已用字节 = 可用载荷 - 最低有效条目偏移
inline uint32_t EntryAreaUsed(const Page& p) {
  const PageHeader& h = p.header();
  uint32_t lowest = kPagePayloadSize;
  for (uint16_t i = 0; i < h.num_slots; ++i) {
    Slot s = p.ReadSlot(i);
    if (s.len == 0 || s.offset == kSlotEmpty) {
      continue;
    }
    if (s.offset < lowest) {
      lowest = s.offset;
    }
  }
  return kPagePayloadSize - lowest;
}

// free_space 的唯一权威定义：可用载荷 - 槽目录 - 条目区实际占用。
// 所有会改变 num_slots / 条目布局的操作（Append/Insert/Erase/Compact）都必须
// 在末尾调用它，而不是各自做增量减法 —— 增量减法在"删除槽后压缩槽目录"这类
// 场景下极易漏算（本实现第一版就在 Compact 上漏算了被删槽的槽目录空间，
// 由 tests/storage/test_page.cc 的 CompactRecoversSlotDirectorySpace 抓出）。
inline void RecomputeFreeSpace(Page* p) {
  PageHeader* h = p->mutable_header();
  const uint32_t used = EntryAreaUsed(*p);
  const uint32_t dir = SlotDirBytes(h->num_slots);
  if (dir + used > kPagePayloadSize) {
    h->free_space = 0;
    return;
  }
  h->free_space = static_cast<uint16_t>(kPagePayloadSize - dir - used);
}

}  // namespace

void Page::Init(PageId pid, PageType type, TreeId tree_id, uint8_t level) {
  std::memset(raw_, 0, kPageSize);
  PageHeader* h = mutable_header();
  h->page_id = pid;
  h->type = type;
  h->flags = 0;
  h->num_slots = 0;
  h->free_space = static_cast<uint16_t>(kPagePayloadSize);
  h->frag_bytes = 0;
  h->next_page = kInvalidPageId;
  h->prev_page = kInvalidPageId;
  h->tree_id = tree_id;
  h->level = level;
  h->reserved = 0;
  h->page_lsn = kInvalidLsn;
  UpdateChecksum();
}

Status Page::Validate() const {
  const PageHeader& h = header();
  const PageTrailer& t = trailer();
  if (h.page_id != t.page_id_copy) {
    return Status::Error(StatusCode::kCorruption,
                         "page self-description is inconsistent: header.page_id=" + std::to_string(h.page_id) +
                             " trailer.page_id_copy=" + std::to_string(t.page_id_copy));
  }
  if (h.page_lsn != t.page_lsn_copy) {
    return Status::Error(StatusCode::kCorruption, "page LSN copy mismatch (torn write suspected)");
  }
  if (SlotDirBytes(h.num_slots) + h.free_space > kPagePayloadSize) {
    return Status::Error(StatusCode::kCorruption, "slot directory plus free space exceeds the page capacity");
  }
  const uint32_t slot_dir_bytes = SlotDirBytes(h.num_slots);
  const uint32_t used = EntryAreaUsed(*this);
  if (slot_dir_bytes + h.free_space + used > kPagePayloadSize) {
    return Status::Error(StatusCode::kCorruption,
                         "in-page layout overlap: slots=" + std::to_string(slot_dir_bytes) +
                             " free=" + std::to_string(h.free_space) +
                             " used=" + std::to_string(used));
  }
  for (uint16_t i = 0; i < h.num_slots; ++i) {
    Slot s = ReadSlot(i);
    if (s.len == 0 || s.offset == kSlotEmpty) {
      continue;
    }
    if (static_cast<uint32_t>(s.offset) + s.len > kPagePayloadSize) {
      return Status::Error(StatusCode::kCorruption,
                           "slot " + std::to_string(i) + " out of range: off=" + std::to_string(s.offset) +
                               " len=" + std::to_string(s.len));
    }
  }
  const uint32_t expect = Crc32c(raw_, kPageSize - kPageTrailerSize);
  if (expect != t.checksum) {
    return Status::Error(StatusCode::kChecksumMismatch,
                         "page " + std::to_string(h.page_id) + " checksum mismatch: expected " +
                             std::to_string(expect) + " actual " + std::to_string(t.checksum));
  }
  return Status::Ok();
}

void Page::UpdateChecksum() {
  PageHeader* h = mutable_header();
  PageTrailer* t = mutable_trailer();
  t->page_id_copy = h->page_id;
  t->page_lsn_copy = h->page_lsn;
  t->checksum = 0;
  t->checksum = Crc32c(raw_, kPageSize - kPageTrailerSize);
}

Slot Page::ReadSlot(SlotId id) const {
  Slot s{};
  if (id >= header().num_slots) {
    s.offset = kSlotEmpty;
    s.len = 0;
    return s;
  }
  const char* base = payload() + static_cast<uint32_t>(id) * sizeof(Slot);
  std::memcpy(&s, base, sizeof(Slot));
  return s;
}

void Page::WriteSlot(SlotId id, Slot slot) {
  char* base = mutable_payload() + static_cast<uint32_t>(id) * sizeof(Slot);
  std::memcpy(base, &slot, sizeof(Slot));
}

uint16_t Page::live_slots() const {
  uint16_t n = 0;
  for (uint16_t i = 0; i < num_slots(); ++i) {
    Slot s = ReadSlot(i);
    if (s.len != 0 && s.offset != kSlotEmpty) {
      ++n;
    }
  }
  return n;
}

StatusOr<const char*> Page::Entry(SlotId id) const {
  Slot s = ReadSlot(id);
  if (s.len == 0 || s.offset == kSlotEmpty) {
    return NotFound("slot " + std::to_string(id) + " is empty or deleted");
  }
  return payload() + s.offset;
}

uint16_t Page::EntryLen(SlotId id) const {
  Slot s = ReadSlot(id);
  return (s.offset == kSlotEmpty) ? 0 : s.len;
}

Status Page::AppendEntry(const char* data, uint16_t len, SlotId* out_slot) {
  if (len == 0) {
    return InvalidArgument("entry length must not be 0");
  }
  PageHeader* h = mutable_header();
  const uint32_t need = static_cast<uint32_t>(len) + sizeof(Slot);
  if (need > h->free_space) {
    return Status::Error(StatusCode::kPageFull,
                         "page full: need " + std::to_string(need) + " available " +
                             std::to_string(h->free_space));
  }
  uint32_t lowest = kPagePayloadSize;
  for (uint16_t i = 0; i < h->num_slots; ++i) {
    Slot s = ReadSlot(i);
    if (s.len == 0 || s.offset == kSlotEmpty) {
      continue;
    }
    if (s.offset < lowest) {
      lowest = s.offset;
    }
  }
  const uint32_t new_off = lowest - len;
  std::memcpy(mutable_payload() + new_off, data, len);
  WriteSlot(h->num_slots, Slot{static_cast<uint16_t>(new_off), len});
  if (out_slot != nullptr) {
    *out_slot = h->num_slots;
  }
  ++h->num_slots;
  RecomputeFreeSpace(this);
  return Status::Ok();
}

Status Page::InsertEntryAt(SlotId at, const char* data, uint16_t len) {
  PageHeader* h = mutable_header();
  if (at > h->num_slots) {
    return InvalidArgument("insert slot out of range: " + std::to_string(at));
  }
  const uint32_t need = static_cast<uint32_t>(len) + sizeof(Slot);
  if (need > h->free_space) {
    return Status::Error(StatusCode::kPageFull,
                         "page full: need " + std::to_string(need) + " available " +
                             std::to_string(h->free_space));
  }
  uint32_t lowest = kPagePayloadSize;
  for (uint16_t i = 0; i < h->num_slots; ++i) {
    Slot s = ReadSlot(i);
    if (s.len == 0 || s.offset == kSlotEmpty) {
      continue;
    }
    if (s.offset < lowest) {
      lowest = s.offset;
    }
  }
  const uint32_t new_off = lowest - len;
  std::memcpy(mutable_payload() + new_off, data, len);
  for (uint16_t i = h->num_slots; i > at; --i) {
    WriteSlot(i, ReadSlot(static_cast<SlotId>(i - 1)));
  }
  WriteSlot(at, Slot{static_cast<uint16_t>(new_off), len});
  ++h->num_slots;
  RecomputeFreeSpace(this);
  return Status::Ok();
}

Status Page::EraseEntry(SlotId id) {
  if (id >= num_slots()) {
    return InvalidArgument("erase slot out of range: " + std::to_string(id));
  }
  Slot s = ReadSlot(id);
  if (s.len == 0 || s.offset == kSlotEmpty) {
    return NotFound("slot " + std::to_string(id) + " was already empty");
  }
  PageHeader* h = mutable_header();
  // 只清槽：空间记入 frag_bytes，等 Compact() 回收（方案 D1）。
  // 注意槽本身仍留在槽目录里（num_slots 不变），因此槽目录空间不变，
  // 但用统一重算保证 free_space 口径一致。
  h->frag_bytes = static_cast<uint16_t>(h->frag_bytes + s.len);
  WriteSlot(id, Slot{kSlotEmpty, 0});
  RecomputeFreeSpace(this);
  return Status::Ok();
}

void Page::Compact() {
  PageHeader* h = mutable_header();
  const uint16_t n = h->num_slots;
  if (n == 0 || h->frag_bytes == 0) {
    h->frag_bytes = 0;
    return;
  }
  // 先把存活槽的 (offset, len) 读出来，避免边读边写槽目录造成自覆盖
  static thread_local Slot slots[512];
  uint16_t live = 0;
  for (uint16_t i = 0; i < n && live < 512; ++i) {
    Slot s = ReadSlot(i);
    if (s.len == 0 || s.offset == kSlotEmpty) {
      continue;
    }
    slots[live++] = s;
  }
  // 再整体搬到临时缓冲，从尾部向上重新紧凑排列（保持槽序 = 键序）
  static thread_local char tmp[kPagePayloadSize];
  uint32_t cursor = kPagePayloadSize;
  for (uint16_t i = 0; i < live; ++i) {
    cursor -= slots[i].len;
    std::memcpy(tmp + cursor, payload() + slots[i].offset, slots[i].len);
    slots[i].offset = static_cast<uint16_t>(cursor);
  }
  std::memcpy(mutable_payload() + cursor, tmp + cursor, kPagePayloadSize - cursor);
  for (uint16_t i = 0; i < live; ++i) {
    WriteSlot(i, slots[i]);
  }
  h->num_slots = live;
  h->frag_bytes = 0;
  // 关键：压缩后槽目录变短（被删槽连同槽位一起消失），
  // free_space 必须重算，否则会漏掉这部分回收的空间。
  RecomputeFreeSpace(this);
}

uint32_t Page::used_bytes() const { return EntryAreaUsed(*this); }

uint32_t Page::frag_ratio_percent() const {
  // 口径：碎片 / (碎片 + 有效载荷)。
  // 注意不能除以 used_bytes() —— 当所有槽都被删除时 used_bytes() == 0，
  // 会得出 0% 碎片这种反直觉结果（本实现第一版即如此，被
  // tests/storage/test_page.cc 的 FragRatioIsSane 抓出）。
  const uint32_t frag = frag_bytes();
  const uint32_t used = used_bytes();
  const uint32_t total = frag + used;
  if (total == 0) {
    return 0;
  }
  return static_cast<uint32_t>((static_cast<uint64_t>(frag) * 100) / total);
}

}  // namespace ltm

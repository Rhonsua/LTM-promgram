#include "index/bplus_tree.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "concurrency/latch_manager.h"
#include "index/page_utils.h"
#include "util/log.h"

namespace ltm {

using pageutil::LoadKeyAt;
using pageutil::RecomputeFreeSpaceOn;
using pageutil::SlotIsEmpty;
using pageutil::SlotsOf;
using pageutil::StoreKeyAt;

// ===========================================================================
// 页面级 B+ 树实现
//
// 页内布局（payload 区，紧跟 32B PageHeader）：
//   +0    槽目录 slot[0..n-1]（每槽 4B：offset:u16, len:u16，向下增长）
//   ...   (free space)
//   ...   条目区（向上增长）
//   末尾  PageTrailer (16B)
//
// 条目编码：
//   叶条目  <key:u64 | rtree_leaf_page:u32 | payload_len:u16 | payload...>   (14+len B)
//   内部条目<key:u64 | child:u32>                                            (12 B)
//
// 【内部页的 0 号槽是"最左子页"哨兵】
//   与 B+ 树教科书写法一致：内部页 slots[0] 的键恒为 kMinElemKey（不参与比较），
//   它的 child 是本页最左子页；slots[i] (i>=1) 的键 == 该 child 子树的最小键。
//   因此查找时先对整个槽数组做 LowerBound：命中 0 号槽或落在两键之间都下钻到
//   前一个槽的 child。哨兵的存在让"小于最小键"的查找也能正确落位。
//
// ---------------------------------------------------------------------------
// 本文件只负责**页内**语义。跨页算法（下钻/分裂传播/借键合并/下溢循环/根升降/
// 自检骨架/销毁）全部只写一遍，在 index/tree_ops.cc 的 IndexTree 里，
// 本文件通过 TreePageOps 的挂钩与它对接。判断某段逻辑该放哪边的标准只有一条：
//   「写这段代码时，需要先知道叶条目的二进制结构吗？」
//   需要 → 留在这里；不需要 → 属于 tree_ops.cc。
//
// 本文件在调试中暴露并修掉的真实缺陷（改动前务必读完）：
//   (1) 内部页分裂：右页必须**保留哨兵槽**（指向它自己的最左子页），第一个被搬
//       过来的真实键放在 1 号槽。若让第一个搬来的键占 0 号槽，它就得同时扮演
//       哨兵和真实键 —— 一个槽放不下两个角色，实测右页哨兵丢失。
//       分隔条目不再由 SplitInto 返回：父页统一用 BuildSeparator(新页) 从新页
//       现状推导（双真相必然漂移，见 tree_ops.h 的 v3 修订记录）。
//   (2) InitPage 不得写 header->level：页层级由 BufferPool::NewPage(...,level,...)
//       按树高设定；用 is_leaf 压成 0/1 会把新根页应有的 level=2 覆盖成 1。
//   (3) 根分裂后要把旧根的 level 设为"原高度"（= 新高度 - 1）。
//   (4) 内部页条目搬运必须走 InsertEntry（知道 12 字节内部条目尺寸），
//       不能用 Page::AppendEntry（裸字节，按 8 字节键计算 → 条目区与槽目录互相覆盖）。
//   (5) 分裂搬运前必须先把源条目拷进临时缓冲：源页条目与目标页条目在同一帧的两端，
//       逐条直写会踩掉尚未读取的源条目（实测左页变 {68,68,68,68}、右页 n=0）。
//   (6) Page 是"某一帧"的轻量视图：递归下钻期间帧可能被淘汰复用，缓存的 Page 值
//       会变成悬垂视图并静默写坏别人的页。所有访问都要 guard.page() 现取。
//   (7) 借键顺序：先擦除源条目、再更新父页分隔条目（由调用方在 BorrowFrom 之后做）。
// ===========================================================================

// ---------------------------------------------------------------------------
// BPlusPageOps
// ---------------------------------------------------------------------------
BPlusPageOps::BPlusPageOps(uint16_t capacity)
    : capacity_(capacity < kBPlusMinCapacity ? kBPlusMinCapacity : capacity) {}

Status BPlusPageOps::InitPage(PageHeader* header, char* payload, bool is_leaf) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("BPlusPageOps::InitPage got a null pointer");
  }
  (void)payload;
  // 见文件头 (2)：不写 level，只校验调用方传参自洽
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

ElemKey BPlusPageOps::FirstKey(const PageHeader& header, const char* payload) const {
  if (header.num_slots == 0) {
    return kMinElemKey;
  }
  // 内部页的 0 号槽是哨兵；它的"最小键"由 1 号槽给出。
  // 叶页没有哨兵，0 号槽就是最小键。
  // 这条差值正是"父页分隔条目 = FirstKey(子页)"能同时服务两种页的原因。
  if (header.level > 0 && header.num_slots > 1) {
    return GetKey(header, payload, 1);
  }
  Slot s = SlotsOf(payload)[0];
  if (SlotIsEmpty(s)) {
    return kMinElemKey;
  }
  return LoadKeyAt(payload + s.offset);
}

SlotId BPlusPageOps::LowerBound(const PageHeader& header, const char* payload, ElemKey key) const {
  uint16_t lo = 0;
  uint16_t hi = header.num_slots;
  while (lo < hi) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
    Slot s = SlotsOf(payload)[mid];
    int c;
    if (SlotIsEmpty(s)) {
      c = -1;  // 已删除槽：保守右移
    } else {
      const ElemKey mk = LoadKeyAt(payload + s.offset);
      c = (mk < key) ? -1 : ((mk > key) ? 1 : 0);
    }
    if (c < 0) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      hi = mid;
    }
  }
  return lo;
}

ElemKey BPlusPageOps::GetKey(const PageHeader& header, const char* payload, SlotId slot) const {
  if (slot >= header.num_slots) {
    return kMinElemKey;
  }
  Slot s = SlotsOf(payload)[slot];
  if (SlotIsEmpty(s)) {
    return kMinElemKey;
  }
  return LoadKeyAt(payload + s.offset);
}

PageId BPlusPageOps::GetChild(const PageHeader& header, const char* payload, SlotId slot) const {
  if (header.level == 0 || slot >= header.num_slots) {
    return kInvalidPageId;
  }
  Slot s = SlotsOf(payload)[slot];
  if (SlotIsEmpty(s)) {
    return kInvalidPageId;
  }
  PageId child = kInvalidPageId;
  std::memcpy(&child, payload + s.offset + sizeof(ElemKey), sizeof(PageId));
  return child;
}

Status BPlusPageOps::InsertEntry(PageHeader* header, char* payload, SlotId at, ElemKey key,
                                 PageId child, const std::vector<char>& value) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("InsertEntry got a null pointer");
  }
  const bool is_leaf = (header->level == 0);
  const uint32_t entry_len =
      is_leaf ? BPlusLeafEntrySize(static_cast<uint16_t>(value.size()))
              : static_cast<uint32_t>(sizeof(BPlusInternalEntry));
  const uint32_t need = entry_len + sizeof(Slot);
  if (need > header->free_space) {
    return Status::Error(StatusCode::kPageFull, "page full (split required)");
  }
  if (at > header->num_slots) {
    return InvalidArgument("insert position out of range");
  }
  // 哨兵键只允许放在内部页的 0 号槽
  if (!is_leaf && key == kMinElemKey && at != 0) {
    return InvalidArgument("the sentinel key may only appear in slot 0 of an internal page");
  }
  // 0 号槽若放非哨兵键，会破坏最左子页约定
  if (!is_leaf && at == 0 && key != kMinElemKey) {
    return InvalidArgument("internal page slot 0 must be written with the sentinel key");
  }

  uint32_t lowest = kPagePayloadSize;
  for (uint16_t i = 0; i < header->num_slots; ++i) {
    Slot s = SlotsOf(payload)[i];
    if (SlotIsEmpty(s)) {
      continue;
    }
    if (s.offset < lowest) {
      lowest = s.offset;
    }
  }
  const uint32_t new_off = lowest - entry_len;
  char* dst = payload + new_off;
  if (is_leaf) {
    BPlusLeafEntry le{};
    le.key = key;
    le.rtree_leaf_page = child;
    le.payload_len = static_cast<uint16_t>(value.size());
    std::memcpy(dst, &le, sizeof(le));
    if (!value.empty()) {
      std::memcpy(dst + sizeof(le), value.data(), value.size());
    }
  } else {
    BPlusInternalEntry ie{};
    ie.key = key;
    ie.child = child;
    std::memcpy(dst, &ie, sizeof(ie));
  }

  for (uint16_t i = header->num_slots; i > at; --i) {
    SlotsOf(payload)[i] = SlotsOf(payload)[i - 1];
  }
  SlotsOf(payload)[at] = Slot{static_cast<uint16_t>(new_off), static_cast<uint16_t>(entry_len)};
  ++header->num_slots;
  header->free_space = static_cast<uint16_t>(header->free_space - need);
  return Status::Ok();
}

Status BPlusPageOps::EraseSlot(PageHeader* header, char* payload, SlotId slot) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("EraseSlot got a null pointer");
  }
  if (slot >= header->num_slots) {
    return InvalidArgument("erase slot out of range");
  }
  if (header->level == 0) {
    return Status::Error(StatusCode::kNotSupported,
                         "erase leaf entries through IndexTree::Erase (it maintains the contiguous entry area and the entry count)");
  }
  Slot removed = SlotsOf(payload)[slot];
  for (uint16_t i = slot; i + 1 < header->num_slots; ++i) {
    SlotsOf(payload)[i] = SlotsOf(payload)[i + 1];
  }
  --header->num_slots;
  if (!SlotIsEmpty(removed)) {
    header->frag_bytes = static_cast<uint16_t>(header->frag_bytes + removed.len);
  }
  RecomputeFreeSpaceOn(header, payload);
  return Status::Ok();
}

bool BPlusPageOps::IsOverflow(const PageHeader& header, const char* payload) const {
  (void)payload;
  return header.num_slots >= capacity_;
}

bool BPlusPageOps::IsUnderflow(const PageHeader& header, const char* payload) const {
  (void)payload;
  return header.num_slots < UnderflowThreshold();
}

Status BPlusPageOps::SplitInto(PageHeader* header, char* payload, Page* new_page) {
  if (header == nullptr || payload == nullptr || new_page == nullptr) {
    return InvalidArgument("SplitInto got a null pointer");
  }
  const bool is_leaf = (header->level == 0);
  const uint16_t n = header->num_slots;
  if (n < 3) {
    return Status::Error(StatusCode::kCorruption, "split of a page with fewer than 3 entries");
  }
  const uint16_t mid = static_cast<uint16_t>(n / 2);
  Slot* slots = SlotsOf(payload);

  // 【源页被搬空的槽必须记入 frag_bytes，否则 free_space 会塌成谎报】
  //
  // 条目区从页尾向下生长，槽的 offset 取决于**到达顺序**而与键序无关。因此
  // "丢掉槽 [mid, n)" 并不会丢掉一段连续的字节：被丢槽的偏移与保留槽交错，
  // 保留槽的条目之间留下空洞。而 free_space 的口径（page.cc 的 EntryAreaUsed）
  // 是"最高地址 - 最低偏移"的**跨度**，它把空洞算成已用 —— 实测一页 28 条、
  // 真实占用 897 字节，却被记成 3931 字节，free_space 只剩 5。
  //
  // 后果不是"空间浪费"而是**不变量被破坏**：页明明只装了 28/32 条就报 kPageFull，
  // 触发的却是我新加的「空间先满」兜底路径 —— 在 28 条处分裂成 14/14，两半都低于
  // 填充阈值 16，Validate 立刻报 I3「页 1 填充不足」。更早的版本里 B+ 遇到这种
  // 假 kPageFull 是直接报错返回的，所以这个缺陷一直没露头；它藏在**跨度口径 +
  // 分裂不压实**这个组合里，B+ 自己的验收与差分测试都没走到（那里每页都插到
  // capacity 才分裂，字节从未先满）。
  //
  // 处理：把腾出的字节记为碎片并压实源页。Compact() 会把存活条目重新紧凑排到
  // 页尾，空洞消失后跨度 == 真实占用，free_space 才重新可信。
  // 注意顺序：dropped 必须在任何槽被改写**之前**统计。
  uint32_t dropped = 0;
  for (uint16_t i = mid; i < n; ++i) {
    const Slot s = slots[i];
    if (!SlotIsEmpty(s)) {
      dropped += s.len;
    }
  }

  if (is_leaf) {
    // 先把要搬的条目拷进临时缓冲：源页与目标页的条目区在同一帧的两端，
    // 逐条直写会踩掉尚未读取的源条目（见文件头 (5)）。
    std::vector<std::vector<char>> moved;
    moved.reserve(static_cast<size_t>(n - mid));
    for (uint16_t i = mid; i < n; ++i) {
      Slot s = slots[i];
      if (SlotIsEmpty(s)) {
        continue;
      }
      moved.emplace_back(payload + s.offset, payload + s.offset + s.len);
    }
    header->num_slots = mid;
    for (const std::vector<char>& buf : moved) {
      const Status st = new_page->AppendEntry(buf.data(), static_cast<uint16_t>(buf.size()), nullptr);
      if (!st.ok()) {
        return Status::Error(st.code(), std::string("leaf split failed to move entries: ") + st.message());
      }
    }
    // 维护叶链：left -> new_page -> old_next（old_next 的 prev 由调用方修正）
    const PageId old_next = header->next_page;
    new_page->mutable_header()->next_page = old_next;
    new_page->mutable_header()->prev_page = header->page_id;
    header->next_page = new_page->header().page_id;
  } else {
    // 内部页分裂（见文件头 (1)）：
    //   源页槽序 [哨兵(c0), (k1,c1), ..., (k_{n-1}, c_{n-1})]，哨兵同时代表最左子页 c0。
    //   设 mid 为分裂点，被搬走的是槽 [mid, n)。
    //
    //   左页：保留槽 [0, mid) 原样（哨兵 c0 + 前 mid-1 对键值）
    //   右页：最左子页是"被搬走的第一条对应的 child"，布局为
    //           槽0 = <哨兵, moved[0].child>
    //           槽1 = <moved[0].key, moved[1].child>
    //           ...
    //           槽m = <moved[m-1].key, c0>          ← 最后一个接过源页原来的 c0
    //         即"键右移一位、child 左移一位"。
    //
    //   【关键】右页 1 号槽的 child 不能沿用 moved[0].child：那会与 0 号槽哨兵指向
    //   同一个子页，形成自环（实测 child={6,6,7,8,9}，环 6→6）。必须左移一位。
    std::vector<BPlusInternalEntry> moved;
    moved.reserve(static_cast<size_t>(n - mid));
    for (uint16_t i = mid; i < n; ++i) {
      Slot s = slots[i];
      if (SlotIsEmpty(s)) {
        continue;
      }
      BPlusInternalEntry ie{};
      if (s.len < sizeof(ie)) {
        return Status::Error(StatusCode::kCorruption,
                             "internal entry length " + std::to_string(s.len) + " is smaller than " +
                                 std::to_string(sizeof(ie)));
      }
      std::memcpy(&ie, payload + s.offset, sizeof(ie));
      moved.push_back(ie);
    }
    if (moved.size() < 2) {
      return Status::Error(StatusCode::kCorruption, "the internal page has too few entries to split");
    }
    const PageId src_leftmost = GetChild(*header, payload, 0);
    if (src_leftmost == kInvalidPageId) {
      return Status::Error(StatusCode::kCorruption, "the source internal page's sentinel points at an illegal child page");
    }
    header->num_slots = mid;  // 左页保留 [0, mid)

    // 右页逐条重建。子页引用的正确对应关系（按"slot[i].child 承接 [key_i, key_{i+1})"）：
    //   右页槽0（哨兵，键 kMinElemKey）的 child  ← moved[0].child
    //   右页槽1（键 moved[0].key）的 child       ← moved[0].child
    //   右页槽2（键 moved[1].key）的 child       ← moved[1].child
    //   ...
    // 即 child 与键**同位对齐**（不是左移、也不是右移）：
    // 源页里 moved[i].child 承接的正是 [moved[i].key, ·) 区间，搬到右页后区间不变。
    // 【前两次都错在这里】把哨兵的 child 设成 moved[0].child 看似自环，
    // 其实是因为槽 0 与槽 1 指向同一子页是"最左子页哨兵对"的正常形态；
    // 而把最后一条的 child 设成 src_leftmost 才是真错（把左子树挂到右子树下）。
    const std::vector<char> empty;
    Status st = InsertEntry(new_page->mutable_header(), new_page->mutable_payload(), 0,
                            kMinElemKey, moved[0].child, empty);
    if (!st.ok()) {
      return st;
    }
    for (size_t i = 0; i < moved.size(); ++i) {
      const uint16_t at = new_page->header().num_slots;
      st = InsertEntry(new_page->mutable_header(), new_page->mutable_payload(), at, moved[i].key,
                       moved[i].child, empty);
      if (!st.ok()) {
        return Status::Error(st.code(), std::string("internal page split failed to move entries: ") + st.message());
      }
    }
  }

  new_page->mutable_header()->level = header->level;
  new_page->mutable_header()->type = header->type;
  new_page->mutable_header()->tree_id = header->tree_id;

  // 记碎片 + 压实源页（理由见本函数开头 dropped 的注释）。
  // Compact() 在 frag_bytes == 0 时直接返回，所以必须先累加再调用。
  if (dropped > 0) {
    header->frag_bytes = static_cast<uint16_t>(header->frag_bytes + dropped);
    Page src(payload - kPageHeaderSize);
    src.Compact();
  }
  RecomputeFreeSpaceOn(header, payload);
  RecomputeFreeSpaceOn(new_page->mutable_header(), new_page->payload());

  // 内部页分裂后的自检：两页的 0 号槽都必须是哨兵
  if (!is_leaf) {
    if (header->num_slots == 0 || GetKey(*header, payload, 0) != kMinElemKey) {
      return Status::Error(StatusCode::kCorruption, "after an internal split the left page's slot 0 is not the sentinel");
    }
    if (new_page->header().num_slots == 0 ||
        GetKey(new_page->header(), new_page->payload(), 0) != kMinElemKey) {
      return Status::Error(StatusCode::kCorruption, "after an internal split the right page's slot 0 is not the sentinel");
    }
  }
  return Status::Ok();
}

// 借键的四种情形。方向由 dir 显式给出 —— 方向搞反会把键序弄反（缺陷 #18）：
//   借左兄弟 → 搬它的**末**条目，插到本页**开头**
//   借右兄弟 → 搬它的**首**条目，追加到本页**末尾**
// 两端的擦除与"哨兵对重开"这类布局手术一并在这里做完，跨页算法不参与 ——
// 哨兵槽的约定只有页算子知道。
bool BPlusPageOps::BorrowFrom(PageHeader* header, char* payload, Page* donor, BorrowDirection dir,
                              const TreeEntry& parent_sep, TreeEntry* moved_out) {
  if (header == nullptr || payload == nullptr || donor == nullptr) {
    return false;
  }
  const PageHeader& dh = donor->header();
  const uint16_t dn = dh.num_slots;
  if (dn == 0) {
    return false;
  }
  const bool is_leaf = (header->level == 0);
  const std::vector<char> empty;

  if (dir == BorrowDirection::kFromLeft) {
    const SlotId donor_slot = static_cast<SlotId>(dn - 1);
    Slot s = SlotsOf(donor->payload())[donor_slot];
    if (SlotIsEmpty(s)) {
      return false;
    }
    if (is_leaf) {
      Page self(payload - kPageHeaderSize);
      if (!self.InsertEntryAt(0, donor->payload() + s.offset, s.len).ok()) {
        return false;
      }
      if (moved_out != nullptr) {
        moved_out->key = LoadKeyAt(donor->payload() + s.offset);
        moved_out->child = kInvalidPageId;
        moved_out->value.assign(donor->payload() + s.offset, donor->payload() + s.offset + s.len);
      }
      if (!donor->EraseEntry(donor_slot).ok()) {
        return false;
      }
      donor->Compact();  // 不留墓碑，保持 num_slots == 存活条目数
      RecomputeFreeSpaceOn(donor->mutable_header(), donor->payload());
      return true;
    }
    // 内部页：本页在 slot>=1，因此它的 0/1 号槽是一对冗余的"最左子页"
    // （0 号槽持哨兵键、1 号槽持该子页的最小键）。借来的 c_last 成为新的最左
    // 子页，于是丢掉旧 0 号槽（旧 c0 仍由旧 1 号槽代表），在最前面重开一对。
    if (donor_slot == 0) {
      return false;  // 哨兵槽不外借
    }
    BPlusInternalEntry ie{};
    if (s.len < sizeof(ie)) {
      return false;
    }
    std::memcpy(&ie, donor->payload() + s.offset, sizeof(ie));
    if (ie.child == kInvalidPageId || header->num_slots < 2) {
      return false;
    }
    Page self(payload - kPageHeaderSize);
    if (!self.EraseEntry(0).ok()) {
      return false;
    }
    self.Compact();
    if (!InsertEntry(header, payload, 0, kMinElemKey, ie.child, empty).ok()) {
      return false;
    }
    if (!InsertEntry(header, payload, 1, ie.key, ie.child, empty).ok()) {
      return false;
    }
    if (moved_out != nullptr) {
      moved_out->key = ie.key;
      moved_out->child = ie.child;
    }
    if (!donor->EraseEntry(donor_slot).ok()) {
      return false;
    }
    donor->Compact();
    RecomputeFreeSpaceOn(donor->mutable_header(), donor->payload());
    return true;
  }

  // 向右兄弟借**首**条目（借来的键比本页现有全部键都大，追加到末尾）
  Slot s = SlotsOf(donor->payload())[0];
  if (SlotIsEmpty(s)) {
    return false;
  }
  if (is_leaf) {
    Page self(payload - kPageHeaderSize);
    if (!self.InsertEntryAt(header->num_slots, donor->payload() + s.offset, s.len).ok()) {
      return false;
    }
    if (moved_out != nullptr) {
      moved_out->key = LoadKeyAt(donor->payload() + s.offset);
      moved_out->child = kInvalidPageId;
      moved_out->value.assign(donor->payload() + s.offset, donor->payload() + s.offset + s.len);
    }
    if (!donor->EraseEntry(0).ok()) {
      return false;
    }
    donor->Compact();
    RecomputeFreeSpaceOn(donor->mutable_header(), donor->payload());
    return true;
  }
  // 内部页：把右兄弟的最左子页 d0 追加到本页末尾（键 = 父页指向右兄弟的
  // 分隔条目键 = d0 子树的最小键）。右兄弟随后丢掉 d0：它的 0/1 号槽（一对冗余）
  // 整体前移一位，用原来的 2 号槽重新配对，它自己的最小键随之抬升，
  // 由跨页算法在借键成功后写回父页。
  // donor 的 0 号槽是哨兵（键 kMinElemKey），它的 child 正是父页中指向 donor 的
  // 那个分隔条目所路由的子树，因此可以以 parent_sep.key 为键搬进本页。
  const PageId d0 = GetChild(dh, donor->payload(), 1);
  if (d0 == kInvalidPageId || dn < 3) {
    return false;
  }
  const PageId new_left = GetChild(dh, donor->payload(), 2);
  if (new_left == kInvalidPageId) {
    return false;
  }
  if (!InsertEntry(header, payload, header->num_slots, parent_sep.key, d0, empty).ok()) {
    return false;
  }
  if (!donor->EraseEntry(0).ok() || !donor->EraseEntry(1).ok()) {
    return false;
  }
  donor->Compact();
  if (!InsertEntry(donor->mutable_header(), donor->mutable_payload(), 0, kMinElemKey, new_left,
                   empty)
           .ok()) {
    return false;
  }
  if (moved_out != nullptr) {
    moved_out->key = parent_sep.key;
    moved_out->child = d0;
  }
  return true;
}

Status BPlusPageOps::MergeFrom(PageHeader* header, char* payload, const Page& donor,
                               ElemKey separator_for_internal) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("MergeFrom got a null pointer");
  }
  const bool is_leaf = (header->level == 0);
  const PageHeader& dh = donor.header();

  if (is_leaf) {
    std::vector<std::vector<char>> moved;
    for (uint16_t i = 0; i < dh.num_slots; ++i) {
      Slot s = SlotsOf(donor.payload())[i];
      if (SlotIsEmpty(s)) {
        continue;
      }
      moved.emplace_back(donor.payload() + s.offset, donor.payload() + s.offset + s.len);
    }
    Page self(payload - kPageHeaderSize);
    for (const std::vector<char>& buf : moved) {
      const Status st = self.AppendEntry(buf.data(), static_cast<uint16_t>(buf.size()), nullptr);
      if (!st.ok()) {
        return Status::Error(st.code(), std::string("leaf merge failed to move entries: ") + st.message());
      }
    }
    RecomputeFreeSpaceOn(header, payload);
    return Status::Ok();
  }

  // 内部页合并（右并入左）：
  //   donor 的 0 号槽是它自己的最左子页哨兵，对应的 child 正好是父页中
  //   separator_for_internal 所路由的那棵子树。因此把它以"真实键"的形式追加：
  //   <separator_for_internal, donor.child[0]>，然后再追加 donor 的 1..n-1 槽。
  //
  // 【键数守恒】父页少一个键、本页多一个键，总数不变 —— 但这条守恒**不是**
  // 通用层可以依赖的性质：多维树的内部页没有哨兵，那条分隔条目是直接删除的。
  // 所以通用层从不做键数账，只按 num_slots 与 IsUnderflow 判定。
  if (dh.num_slots == 0) {
    return Status::Error(StatusCode::kCorruption, "the internal page being merged is empty");
  }
  std::vector<BPlusInternalEntry> moved;
  moved.reserve(dh.num_slots);
  {
    BPlusInternalEntry first{};
    first.key = separator_for_internal;
    first.child = GetChild(dh, donor.payload(), 0);
    if (first.child == kInvalidPageId) {
      return Status::Error(StatusCode::kCorruption, "the internal page being merged into has an illegal leftmost child");
    }
    moved.push_back(first);
  }
  for (uint16_t i = 1; i < dh.num_slots; ++i) {
    Slot s = SlotsOf(donor.payload())[i];
    if (SlotIsEmpty(s)) {
      continue;
    }
    BPlusInternalEntry ie{};
    if (s.len < sizeof(ie)) {
      return Status::Error(StatusCode::kCorruption, "the internal page being merged into has an illegal entry length");
    }
    std::memcpy(&ie, donor.payload() + s.offset, sizeof(ie));
    moved.push_back(ie);
  }
  const std::vector<char> empty;
  ElemKey last_key = kMinElemKey;
  PageId last_child = kInvalidPageId;
  bool has_last = false;
  for (const BPlusInternalEntry& ie : moved) {
    // 右页若由分裂产生，槽 0（哨兵）与槽 1 指向同一个子页，于是
    // (separator_for_internal, donor.child[0]) 与 donor 的槽 1 是同一条目。
    // 盲追加会在合并后的页里留下两个同键同子页的条目，
    // ValidatePage 的"键严格升序"检查会直接判为损坏。
    if (has_last && ie.key == last_key && ie.child == last_child) {
      continue;
    }
    const uint16_t at = header->num_slots;
    const Status st = InsertEntry(header, payload, at, ie.key, ie.child, empty);
    if (!st.ok()) {
      return st;
    }
    last_key = ie.key;
    last_child = ie.child;
    has_last = true;
  }
  RecomputeFreeSpaceOn(header, payload);
  return Status::Ok();
}

Status BPlusPageOps::ValidatePage(const PageHeader& header, const char* payload) const {
  const uint16_t n = header.num_slots;
  if (static_cast<uint32_t>(n) * sizeof(Slot) + header.free_space > kPagePayloadSize) {
    return Status::Error(StatusCode::kCorruption, "slot directory plus free space exceeds the page capacity");
  }
  if (n > capacity_) {
    return Status::Error(StatusCode::kCorruption,
                         "entries in page " + std::to_string(n) + " exceeds capacity " +
                             std::to_string(capacity_) + " (should have been split already)");
  }
  bool first_seen = false;
  ElemKey prev = kMinElemKey;
  for (uint16_t i = 0; i < n; ++i) {
    Slot s = SlotsOf(payload)[i];
    if (SlotIsEmpty(s)) {
      continue;
    }
    const uint32_t expect_len =
        (header.level == 0) ? sizeof(BPlusLeafEntry) : sizeof(BPlusInternalEntry);
    if (s.len < expect_len || static_cast<uint32_t>(s.offset) + s.len > kPagePayloadSize) {
      return Status::Error(StatusCode::kCorruption, "slot " + std::to_string(i) + " has an illegal length/offset");
    }
    const ElemKey k = LoadKeyAt(payload + s.offset);
    if (header.level != 0 && i == 0) {
      if (k != kMinElemKey) {
        return Status::Error(StatusCode::kCorruption, "internal page slot 0 must be the sentinel key");
      }
    } else {
      if (first_seen && k <= prev) {
        return Status::Error(StatusCode::kCorruption,
                             "key order is broken: slot " + std::to_string(i) + " key " + std::to_string(k) +
                                 " <= the previous one " + std::to_string(prev));
      }
      prev = k;
      first_seen = true;
    }
    if (header.level == 0) {
      BPlusLeafEntry le{};
      std::memcpy(&le, payload + s.offset, sizeof(le));
      if (static_cast<uint32_t>(s.len) != BPlusLeafEntrySize(le.payload_len)) {
        return Status::Error(StatusCode::kCorruption,
                             "leaf entry: s.len disagrees with payload_len (slot " + std::to_string(i) + ")");
      }
    } else {
      BPlusInternalEntry ie{};
      std::memcpy(&ie, payload + s.offset, sizeof(ie));
      if (ie.child == kInvalidPageId) {
        return Status::Error(StatusCode::kCorruption,
                             "internal entry child is invalid (slot " + std::to_string(i) + ")");
      }
    }
  }
  return Status::Ok();
}

Status BPlusPageOps::CheckParentKey(const PageHeader& parent, const char* parent_payload, SlotId i,
                                    const Page& child) const {
  if (i >= parent.num_slots) {
    return InvalidArgument("CheckParentKey slot out of range");
  }
  const ElemKey pk = GetKey(parent, parent_payload, i);
  // 0 号槽是哨兵，只校验它确实是哨兵；它不承载路由语义
  if (i == 0) {
    if (pk != kMinElemKey) {
      return Status::Error(StatusCode::kCorruption,
                           "internal page slot 0 is not the sentinel key (the leftmost-child convention is broken)");
    }
    return Status::Ok();
  }
  // 其余槽：父页键必须等于该子页的最小键。
  // 【单一来源】这里用的就是 BuildSeparator 的同一个推导（FirstKey），
  // 而不是另写一份"取最小键"的逻辑 —— 两处各算一次就会出现
  // "写入时按一种口径、校验时按另一种口径"，表现为 I1 随机报错。
  const ElemKey ck = FirstKey(child.header(), child.payload());
  if (pk != ck) {
    return Status::Error(StatusCode::kCorruption,
                         "invariant I1 violated: parent key[" + std::to_string(i) + "]=" +
                             std::to_string(pk) + " != the child's minimum key " + std::to_string(ck));
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// v3 挂钩：逻辑记录 <-> 页内条目
// ---------------------------------------------------------------------------
Status BPlusPageOps::EncodeEntry(const IndexEntry& entry, TreeEntry* out) const {
  if (out == nullptr) {
    return InvalidArgument("EncodeEntry got a null pointer");
  }
  if (entry.dim() != 0) {
    return InvalidArgument("BPlusPageOps only accepts 1-D records (dim must be 0)");
  }
  out->key = entry.key;
  // 【方案 §4.9 的落地】B+ 叶条目的 `rtree_leaf_page` 字段直接存"R 树里承载同一条
  // 记录的叶页号"，于是混合索引按键定位 R 树记录时不必再全树搜索，也不需要维护
  // 一张独立的键→条目映射表（中期实现正是靠手工映射，属脆弱点）。
  // 该值由调用方（HybridIndex）从 `RTree::InsertReturningLeaf` 拿到后填进
  // `IndexEntry::rtree_leaf`；普通 B+ 用法下它是 kInvalidPageId。
  out->child = entry.rtree_leaf;
  out->value = entry.payload;
  return Status::Ok();
}

StatusOr<RecordRef> BPlusPageOps::DecodeLeafEntry(const PageHeader& header, const char* payload,
                                                  SlotId slot) const {
  auto pl = LeafPayload(header, payload, slot);
  if (!pl.ok()) {
    return pl.status();
  }
  BPlusLeafEntry le{};
  std::memcpy(&le, pl.value().second - sizeof(BPlusLeafEntry), sizeof(le));
  RecordRef ref;
  ref.key = le.key;
  ref.mbr = Mbr::Invalid(0);
  ref.rtree_leaf = pl.value().first;
  ref.payload = pl.value().second;
  ref.payload_len = le.payload_len;
  return ref;
}

SlotId BPlusPageOps::FindEntry(const PageHeader& header, const char* payload,
                               const IndexEntry& probe) const {
  if (header.level != 0) {
    return kInvalidSlot;
  }
  const SlotId idx = LowerBound(header, payload, probe.key);
  if (idx >= header.num_slots || SlotIsEmpty(SlotsOf(payload)[idx])) {
    return kInvalidSlot;
  }
  if (GetKey(header, payload, idx) != probe.key) {
    return kInvalidSlot;
  }
  return idx;
}

StatusOr<LeafInsertResult> BPlusPageOps::InsertLeafEntry(PageHeader* header, char* payload,
                                                         const TreeEntry& entry) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("InsertLeafEntry got a null pointer");
  }
  if (header->level != 0) {
    return Status::Error(StatusCode::kCorruption, "InsertLeafEntry got a non-leaf page");
  }
  Page self(payload - kPageHeaderSize);
  SlotId idx = LowerBound(*header, payload, entry.key);
  bool overwrote = false;
  // 重复键：覆盖（替换已有条目，条目总数不变）
  if (idx < header->num_slots && !SlotIsEmpty(SlotsOf(payload)[idx]) &&
      GetKey(*header, payload, idx) == entry.key) {
    LTM_RETURN_IF_ERROR(self.EraseEntry(idx));
    self.Compact();
    idx = LowerBound(*header, payload, entry.key);
    overwrote = true;
  }
  Status st = InsertEntry(header, payload, idx, entry.key, entry.child, entry.value);
  if (st.code() == StatusCode::kPageFull) {
    self.Compact();
    idx = LowerBound(*header, payload, entry.key);
    st = InsertEntry(header, payload, idx, entry.key, entry.child, entry.value);
  }
  LTM_RETURN_IF_ERROR(st);
  return overwrote ? LeafInsertResult::kOverwrote : LeafInsertResult::kInserted;
}

SlotId BPlusPageOps::ChooseChildSlot(const PageHeader& header, const char* payload,
                                     const IndexEntry& target, SlotChoice purpose) const {
  LTM_UNUSED(purpose);  // 一维树：插入与查找的去向是同一个
  if (header.num_slots == 0) {
    return 0;
  }
  // 约定：slots[i] 的 child 承接 [keys[i], keys[i+1]) 区间；slots[0] 是哨兵，
  // 承接 (-inf, keys[1])。因此承载槽是**最后一个 keys[i] <= key 的槽**。
  //
  // 【遗留缺陷 #2 的根因】这里原来返回的是"第一个 keys[i] >= key 的槽"
  // （即 LowerBound 的原始结果），方向正好反了：它总把键路由到右侧兄弟，
  // 于是树第一次分裂之后，只有最右子页里的键能被找到，
  // 其余键的点查与删除都报 kNotFound。因为顺序插入永远命中最后一个槽，
  // 所以"顺序插入 + 全量扫描"用例掩盖了这个错误（扫描只依赖叶链）。
  const SlotId lb = LowerBound(header, payload, target.key);
  if (lb >= header.num_slots) {
    return static_cast<SlotId>(header.num_slots - 1);
  }
  // lb 命中精确相等的键时，该槽正是区间 [keys[lb], keys[lb+1]) 的起点；
  // 否则键落在 keys[lb-1] 与 keys[lb] 之间，应下钻到 lb - 1。
  // keys[0] == kMinElemKey == 0 <= 任何键，所以 lb == 0 时只可能是 key == 0。
  if (!SlotIsEmpty(SlotsOf(payload)[lb]) && GetKey(header, payload, lb) == target.key) {
    return lb;
  }
  return (lb == 0) ? 0 : static_cast<SlotId>(lb - 1);
}

Status BPlusPageOps::WriteSeparator(PageHeader* header, char* payload, SlotId slot,
                                    const TreeEntry& sep) {
  if (header == nullptr || payload == nullptr) {
    return InvalidArgument("WriteSeparator got a null pointer");
  }
  if (slot >= header->num_slots) {
    return InvalidArgument("WriteSeparator slot out of range");
  }
  Slot s = SlotsOf(payload)[slot];
  if (SlotIsEmpty(s)) {
    return Status::Error(StatusCode::kCorruption, "WriteSeparator points at an empty slot");
  }
  // 分隔条目是定长的（键 8 字节 + child 4 字节），所以可以就地改写键，
  // 不需要搬动条目区 —— 这也是通用层敢做"判等后原地刷新"的前提。
  StoreKeyAt(payload + s.offset, sep.key);
  return Status::Ok();
}

bool BPlusPageOps::SeparatorEquals(const PageHeader& header, const char* payload, SlotId slot,
                                   const TreeEntry& sep) const {
  if (slot >= header.num_slots) {
    return false;
  }
  Slot s = SlotsOf(payload)[slot];
  if (SlotIsEmpty(s)) {
    return false;
  }
  return LoadKeyAt(payload + s.offset) == sep.key;
}

uint32_t BPlusPageOps::EntryBytes(const TreeEntry& entry) const {
  const size_t vlen = entry.value.size();
  if (vlen > 0xFFFFu) {
    return 0xFFFFFFFFu;
  }
  // 预检只问"单条能不能放得下"，取叶条目（两种条目里更大的那种）保守估计：
  // 内部条目固定 12 字节，叶条目 14 + 载荷。高估无害，低估会漏检。
  return static_cast<uint32_t>(
      BPlusLeafEntrySize(static_cast<uint16_t>(vlen)) + sizeof(Slot));
}

StatusOr<std::pair<PageId, const char*>> BPlusPageOps::LeafPayload(const PageHeader& header,
                                                                   const char* payload,
                                                                   SlotId slot) const {
  if (header.level != 0) {
    return Status::Error(StatusCode::kInvalidArgument, "LeafPayload needs a leaf page");
  }
  if (slot >= header.num_slots) {
    return Status::Error(StatusCode::kInvalidArgument, "LeafPayload slot out of range");
  }
  Slot s = SlotsOf(payload)[slot];
  if (SlotIsEmpty(s)) {
    return NotFound("LeafPayload slot was deleted");
  }
  BPlusLeafEntry le{};
  std::memcpy(&le, payload + s.offset, sizeof(le));
  return std::make_pair(le.rtree_leaf_page, payload + s.offset + sizeof(le));
}

// ===========================================================================
// BPlusTree —— 薄门面
//
// 结构维护全部转调 IndexTree；本类只保留参数校验与**一维树自己的查询半边**。
// ===========================================================================
struct BPlusTree::Impl {
  IndexTree tree;
  BPlusPageOps* ops = nullptr;
};

BPlusTree::BPlusTree() : impl_(new Impl()) {}
BPlusTree::~BPlusTree() = default;

Status BPlusTree::Open(BufferPool* pool, TreePageOps* page_ops, const TreeBuildOptions& options,
                       TreeId tree_id) {
  if (pool == nullptr || page_ops == nullptr) {
    return InvalidArgument("BPlusTree::Open needs a BufferPool and TreePageOps");
  }
  if (std::string(page_ops->name()) != std::string("bplus-page")) {
    return InvalidArgument(std::string("BPlusTree needs a bplus-page page ops, got: ") +
                           page_ops->name());
  }
  impl_->ops = static_cast<BPlusPageOps*>(page_ops);
  return impl_->tree.Open(pool, page_ops, options, tree_id);
}

Status BPlusTree::Insert(const IndexEntry& entry) {
  Impl& im = *impl_;
  if (!im.tree.opened()) {
    return Status::Error(StatusCode::kCorruption, "BPlusTree::Insert called before Open");
  }
  // 一维树的输入约束：dim 必须为 0（维度检查属页算子的 EncodeEntry，这里早失败）
  if (entry.dim() != 0) {
    return InvalidArgument("BPlusTree only accepts 1-D records (dim must be 0)");
  }
  // C1：写操作不取 C0 —— 写串行化由 BufferPool::write_mutex（D4 单写者）承担，
  // 写与读的互斥下移到页闩。
  return im.tree.Insert(entry);
}

Status BPlusTree::Erase(const IndexEntry& entry) {
  Impl& im = *impl_;
  if (!im.tree.opened()) {
    return Status::Error(StatusCode::kCorruption, "BPlusTree::Erase called before Open");
  }
  // C1：同 Insert。
  return im.tree.Erase(entry);
}

// ---------------------------------------------------------------------------
// 查询半边：一维树可以直接单路径下钻（多维树不行，见 tree_ops.h 的诚实边界）
// ---------------------------------------------------------------------------
StatusOr<RecordRef> BPlusTree::Search(const IndexEntry& probe) {
  Impl& im = *impl_;
  if (!im.tree.opened()) {
    return Status::Error(StatusCode::kCorruption, "BPlusTree::Search called before Open");
  }
  // C1：读操作不取全局闩；页闩在 DescendToLeaf 里逐页取（共享）。
  auto leaf = im.tree.DescendToLeaf(probe, SlotChoice::kSearch);
  if (!leaf.ok()) {
    return leaf.status();
  }
  PageGuard guard = std::move(leaf.value());
  const SlotId idx = im.ops->FindEntry(guard.header(), guard.payload(), probe);
  if (idx == kInvalidSlot) {
    return NotFound("no such key: " + std::to_string(probe.key));
  }
  auto ref = im.ops->DecodeLeafEntry(guard.header(), guard.payload(), idx);
  if (!ref.ok()) {
    return ref.status();
  }
  // 【必须在守卫还活着的时候把载荷拷出来】返回时 guard 已经析构，payload 若仍
  // 指向页帧就是 use-after-free（别的线程一次淘汰就能换掉这块帧）。
  // 理由与实测见 RecordRef::owned_payload 的注释。
  return WithOwnedPayload(ref.value());
}

Status BPlusTree::RangeScan(const Query& query,
                            const std::function<bool(const RecordRef&)>& emit) {
  Impl& im = *impl_;
  if (!im.tree.opened()) {
    return Status::Error(StatusCode::kCorruption, "BPlusTree::RangeScan called before Open");
  }
  const ElemKey lo = query.key_lo;
  const ElemKey hi = query.key_hi;
  // C1：范围扫描不取全局闩；叶链遍历每页一次共享闩，读完先放闩再取下一页。
  IndexEntry probe;
  probe.key = lo;
  auto leaf_or = im.tree.DescendToLeaf(probe, SlotChoice::kSearch);
  if (!leaf_or.ok()) {
    return leaf_or.status();
  }
  PageGuard guard = std::move(leaf_or.value());
  // 起点定位用页内二分：**叶内按键有序**是一维树的性质，也正是
  // KeysAreOrdered() 声明的内容。多维树用它自己的空间查询，不走这条路径。
  SlotId start = im.ops->LowerBound(guard.header(), guard.payload(), lo);
  bool done = false;

  // 【C1：一次只持一个叶页闩】叶链遍历若"持着当前叶去取下一叶"，就同时持有两个
  // 同层页闩；而写路径的合并是"父独占 + 两个兄弟独占"，两者可能成环（读者左→右、
  // 写者右→左）。所以这里读完一页先**放掉**它（闩 + pin），再去取下一页。
  // 代价是"两次取页之间树可能变" —— 那只导致读到偏旧的版本（漏掉并发新写入），
  // 不会读到撕裂的页；判据要的是"不撕裂"，不是"快照一致"。
  for (;;) {
    std::vector<RecordRef> batch;
    for (SlotId i = start; i < guard.page().num_slots(); ++i) {
      if (im.ops->IsTombstone(guard.header(), guard.payload(), i)) {
        continue;
      }
      const ElemKey k = im.ops->GetKey(guard.header(), guard.payload(), i);
      if (k > hi) {
        done = true;
        break;
      }
      auto ref = im.ops->DecodeLeafEntry(guard.header(), guard.payload(), i);
      if (!ref.ok()) {
        continue;
      }
      batch.push_back(WithOwnedPayload(ref.value()));
    }
    const PageId next = guard.header().next_page;
    guard.Release();  // 先放闩/pin，再取下一页
    // Bound temporary storage to one page; callbacks may perform other reads.
    for (const auto& ref : batch) if (!emit(ref)) return Status::Ok();
    if (done || next == kInvalidPageId) {
      break;
    }
    auto g = im.tree.Fetch(next);
    if (!g.ok()) {
      return g.status();
    }
    guard = std::move(g.value());
    start = 0;
  }
  return Status::Ok();
}

Status BPlusTree::ScanAll(const std::function<bool(const RecordRef&)>& emit) {
  Query q;
  q.kind = QueryKind::kRange1D;
  q.key_lo = 0;
  q.key_hi = ~static_cast<ElemKey>(0);
  return RangeScan(q, emit);
}

Status BPlusTree::Validate() {
  // 只读遍历。C1：不取全局闩；页闩由遍历内部逐页取（共享）。
  return impl_->tree.Validate();
}

TreeStats BPlusTree::stats() const {
  TreeStats s = impl_->tree.stats();
  s.dim = 0;
  return s;
}

Status BPlusTree::Destroy() { return impl_->tree.Destroy(); }

}  // namespace ltm

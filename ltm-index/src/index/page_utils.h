// src/index/page_utils.h
// 页内布局的**共用**底层工具：槽目录访问、键读写、free_space 重算。
//
// 为什么单独一个头文件：这些函数在被上提之前散落在 bplus_tree.cc 的匿名
// namespace 里。R 树需要一模一样的记账逻辑（页内记账是硬需求，不是可选优化），
// 复制一份会让"页记账恒等式"出现两个实现、两种口径。因此显式共享，
// 而不是让第二棵树再抄一遍（详见 docs/status.md 的重复实现陷阱一节）。
//
// 这里**只**放与树变体无关的裸页工具；任何带语义的页操作都属于 TreePageOps。
#pragma once

#include <cstdint>
#include <cstring>

#include "storage/page.h"
#include "util/types.h"

namespace ltm {
namespace pageutil {

inline Slot* SlotsOf(char* payload) { return reinterpret_cast<Slot*>(payload); }
inline const Slot* SlotsOf(const char* payload) {
  return reinterpret_cast<const Slot*>(payload);
}

// 槽是否为空/已删除（墓碑）。墓碑不是条目：不占条目计数、不参与键序与容量判定。
inline bool SlotIsEmpty(const Slot& s) { return s.len == 0 || s.offset == kSlotEmpty; }

inline ElemKey LoadKeyAt(const char* base) {
  ElemKey k = kMinElemKey;
  std::memcpy(&k, base, sizeof(ElemKey));
  return k;
}

inline void StoreKeyAt(char* base, ElemKey k) { std::memcpy(base, &k, sizeof(ElemKey)); }

// 依据槽目录重算某页的 free_space。任何直接改动槽数组（而不是走
// InsertEntry/EraseEntry）之后都必须调用它，否则页记账恒等式不成立。
//
//   num_slots * sizeof(Slot) + free_space + used_bytes == kPagePayloadSize
//   used_bytes == kPagePayloadSize - min{ slot.offset | slot 有效 }
inline void RecomputeFreeSpaceOn(PageHeader* header, const char* payload) {
  uint32_t lowest = kPagePayloadSize;
  for (uint16_t i = 0; i < header->num_slots; ++i) {
    const Slot s = SlotsOf(payload)[i];
    if (SlotIsEmpty(s)) {
      continue;
    }
    if (s.offset < lowest) {
      lowest = s.offset;
    }
  }
  const uint32_t span = kPagePayloadSize - lowest;
  const uint32_t dir = static_cast<uint32_t>(header->num_slots) * sizeof(Slot);
  header->free_space = (dir + span > kPagePayloadSize)
                           ? 0
                           : static_cast<uint16_t>(kPagePayloadSize - dir - span);
}

// 存活（非墓碑）槽数。用于条目计数与填充率 —— 必须与 IsUnderflow 的口径一致。
inline uint16_t LiveSlots(const PageHeader& header, const char* payload) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < header.num_slots; ++i) {
    if (!SlotIsEmpty(SlotsOf(payload)[i])) {
      ++n;
    }
  }
  return n;
}

}  // namespace pageutil
}  // namespace ltm

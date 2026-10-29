// src/storage/meta_page.h
// 元数据页（页 0）的唯一权威布局（方案 §4.3）。
//
// 为什么单独抽出这个文件：
//   页 0 同时承载"物理页管理状态"（空闲页链、分配游标）与"索引状态"
//   （各棵树/混合索引的根页号、高度、维度）。如果让 BufferPool 和上层各自
//   用自己的 POD 结构往页 0 里写，就会发生"各写一半、互相覆盖"的事故 ——
//   BPlusTree::Open 曾经先于缓冲池初始化去读页 0，读到全零页并当成损坏。
//   现在统一为下面的布局：BufferPool 提供 Read/Write 访问器，上层只改自己的字段。
#pragma once

#include <cstdint>
#include <cstring>

#include "util/status.h"
#include "util/types.h"

namespace ltm {

constexpr uint32_t kMetaPageMagic = 0x4C544D4D;  // 'LTMM'
constexpr uint32_t kMetaPageFormatVersion = 1;

// 每个受支持的索引实体在元数据页里保留一个槽位
constexpr uint32_t kMaxTreeSlots = 8;

#pragma pack(push, 1)
struct TreeMetaSlot {
  uint8_t in_use;         // 0 = 未使用
  uint8_t kind;           // MetaTreeKind
  uint8_t height;         // 树高
  uint8_t reserved;
  uint16_t order_capacity;// 阶数 / 每页最大条目数
  uint16_t dim;           // 仅 R 树：维度
  PageId root;            // 根页号
  PageId leftmost_leaf;   // 叶链起点（B+ 树范围扫描用）
  uint64_t num_entries;   // 逻辑条目数
  uint32_t reserved2;
};
static_assert(sizeof(TreeMetaSlot) == 28, "TreeMetaSlot layout changed; update the docs");

struct MetaPage {
  uint32_t magic;
  uint32_t format_version;

  // ---- 物理页管理（由 BufferPool 独占读写）----
  PageId free_list_head;    // 空闲页链头
  PageId next_unallocated;  // 分配游标：下一个从未用过的页号
  uint64_t free_pages;      // 空闲页总数
  uint64_t physical_pages;  // 已分配过的物理页数（含空闲链）

  // ---- 索引状态（由上层通过 BufferPool 的访问器读写）----
  TreeMetaSlot trees[kMaxTreeSlots];

  uint32_t reserved[8];
};
#pragma pack(pop)

static_assert(sizeof(MetaPage) <= kPagePayloadSize,
              "MetaPage does not fit in one page payload (shrink kMaxTreeSlots or reserved)");

enum class MetaTreeKind : uint8_t { kNone = 0, kBPlus = 1, kRTree = 2, kMock = 3 };

// 把页 0 的载荷解释成 MetaPage
inline MetaPage* MetaOf(char* payload) { return reinterpret_cast<MetaPage*>(payload); }
inline const MetaPage* MetaOf(const char* payload) {
  return reinterpret_cast<const MetaPage*>(payload);
}

}  // namespace ltm

// src/storage/page.h
// 页的物理布局 —— 全项目唯一权威定义（方案 §4.3）。
//
//   +--------------------------------------------------+ offset 0
//   | PageHeader (32B, POD)                            |
//   +--------------------------------------------------+ offset 32
//   | 槽目录 slot[k] 向下增长 ->                        |  每槽 4B: (offset:u16, len:u16)
//   |          ...                                     |
//   |          (free space)                            |
//   |          ...                                     |
//   | <- 条目区向上增长，条目按插入顺序紧凑排列          |
//   +--------------------------------------------------+ offset 4080
//   | PageTrailer (16B): page_id_copy / crc / lsn_copy |
//   +--------------------------------------------------+ offset 4096
//
// 设计要点（见方案 D1）：
//   * 槽目录自上而下、条目区自下而上，`free_space` 为两者之间的空隙。
//   * 删除只把槽标记为 (0,0)，不改动其他条目布局；`frag_bytes` 累计已废弃空间。
//   * 槽序 == 键序（由 KeyComparator 决定），因此页内查找可二分。
//   * 任何写操作都必须走 PageGuard 的 mutable 接口，否则不计脏。
#pragma once

#include <cstdint>
#include <cstring>

#include <cassert>

#include "util/crc32c.h"
#include "util/status.h"
#include "util/types.h"

namespace ltm {

enum class PageType : uint8_t {
  kFree = 0,
  kMeta = 1,
  kBPlusInternal = 2,
  kBPlusLeaf = 3,
  kRTreeInternal = 4,
  kRTreeLeaf = 5,
  kOverflow = 6,
};

const char* PageTypeName(PageType t);

// 页头标志位
constexpr uint8_t kPageFlagDirty = 0x01;
constexpr uint8_t kPageFlagRoot = 0x02;
constexpr uint8_t kPageFlagHasOverflow = 0x04;

#pragma pack(push, 1)

struct PageHeader {
  uint32_t page_id;        //  4 页自描述：物理写错位置可被发现（不变量 I5）
  PageType type;           //  1
  uint8_t flags;           //  1
  uint16_t num_slots;      //  2 含已删除槽（deleted 槽用 len==0 表达）
  uint16_t free_space;     //  2 槽目录与条目区之间的空隙字节数
  uint16_t frag_bytes;     //  2 已回收但未压实的字节数
  PageId next_page;        //  4 叶链右兄弟 / 空闲页链 / overflow 链
  PageId prev_page;        //  4 叶链左兄弟
  TreeId tree_id;          //  2 混合索引中区分哪棵子树
  uint8_t level;           //  1 0 = 叶
  uint8_t reserved;        //  1
  uint64_t page_lsn;       //  8 WAL 重放幂等判据（不变量 I6）
};

struct PageTrailer {
  uint32_t page_id_copy;   // 撕裂页检测
  // 【本条注释曾被写错，M4-2 修正】原注释是 "crc32c(header 前 24B + payload 区)
  // —— 不含自身与 lsn_copy"，与实现不符：page.cc:126 是
  // Crc32c(raw_, kPageSize - kPageTrailerSize)，即覆盖**整个页头（32B）与
  // payload 区**，只排除本 trailer 的 16B。也就是说它**包含 page_lsn**。
  // 这个区别对 M4-3 是硬要求：重放一条 REDO 时必须先写入区间、再把 page_lsn
  // 置为记录自身的 LSN、**最后重算校验和**；照旧注释去做会漏掉后两步，
  // 让每个被重放的页在下次打开时被判为校验和不匹配。
  uint32_t checksum;       // crc32c(页面前 4080B)：整个页头（含 page_lsn）+ payload 区
  uint64_t page_lsn_copy;
};

#pragma pack(pop)

static_assert(sizeof(PageHeader) == 32, "PageHeader must be 32 bytes");
static_assert(sizeof(PageTrailer) == 16, "PageTrailer must be 16 bytes");

// 槽描述
struct Slot {
  uint16_t offset;  // 相对 payload 起点的偏移；0xFFFF 表示空槽
  uint16_t len;     // 0 表示已删除
};
constexpr uint16_t kSlotEmpty = 0xFFFF;

// ---------------------------------------------------------------------------
// 裸页操作（不涉及缓冲池、不涉及 I/O）
// 页内偏移计算与空间管理都在这里；PageGuard 与测试都直接复用
// ---------------------------------------------------------------------------
class Page {
 public:
  explicit Page(char* raw) : raw_(raw) {}

  static uint32_t header_offset() { return 0; }
  static uint32_t payload_offset() { return kPageHeaderSize; }
  static uint32_t trailer_offset() { return kPageSize - kPageTrailerSize; }
  static uint32_t slot_dir_offset() { return kPageHeaderSize; }
  static uint32_t entry_area_end() { return Page::trailer_offset(); }

  // ---- 生命周期 ----
  // 初始化一个空页：写 header + 清零槽目录 + 写 trailer
  void Init(PageId pid, PageType type, TreeId tree_id, uint8_t level);
  // 格式校验：magic 字段一致性 + 校验和（不变量 I5）
  Status Validate() const;
  // 重算并写入校验和（所有改动页内容的操作结束时调用）
  void UpdateChecksum();

  // ---- 访问器 ----
  // 访问器。注意 mutable_* 返回**指针**（与 PageGuard 保持一致），
  // 这样跨页算法里 `ops->InsertEntry(guard.mutable_header(), guard.mutable_payload(), ...)`
  // 可以自然书写，不会出现 `&page().mutable_header()` 这类"对临时对象取地址"的错误。
  const PageHeader& header() const {
    return *reinterpret_cast<const PageHeader*>(raw_ + header_offset());
  }
  PageHeader* mutable_header() {
    // 契约：Page 只是某个页帧的轻量视图。绝不能让它的生命周期长于持有该帧的
    // PageGuard —— 帧被淘汰复用后，这里的指针仍指向旧帧（内容已换成别的页），
    // 于是会静默写坏别人的页。本断言专门抓这类悬垂视图。
    assert(raw_ != nullptr);
    return reinterpret_cast<PageHeader*>(raw_ + header_offset());
  }
  const char* payload() const { return raw_ + payload_offset(); }
  char* mutable_payload() { return raw_ + payload_offset(); }
  const PageTrailer& trailer() const {
    return *reinterpret_cast<const PageTrailer*>(raw_ + trailer_offset());
  }
  PageTrailer* mutable_trailer() { return reinterpret_cast<PageTrailer*>(raw_ + trailer_offset()); }

  Slot ReadSlot(SlotId id) const;
  void WriteSlot(SlotId id, Slot slot);

  uint16_t num_slots() const { return header().num_slots; }
  uint32_t free_space() const { return header().free_space; }
  uint32_t frag_bytes() const { return header().frag_bytes; }
  bool is_leaf() const { return header().level == 0; }

  // 有效（未删除）槽数量
  uint16_t live_slots() const;

  // 槽内容视图；已删除槽返回 kNotFound
  StatusOr<const char*> Entry(SlotId id) const;
  uint16_t EntryLen(SlotId id) const;

  // ---- 修改 ----
  // 在 [lo, hi] 槽区间内二分找到第一个 >= key 的槽（由 cmp 决定序）
  // cmp(entry_ptr, entry_len, key) -> int
  template <class Cmp>
  SlotId LowerBound(const void* key, Cmp cmp) const {
    uint16_t lo = 0;
    uint16_t hi = num_slots();
    while (lo < hi) {
      uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
      Slot s = ReadSlot(mid);
      int c;
      if (s.len == 0) {
        // 已删除槽：跳过（保守右移）
        c = -1;
      } else {
        c = cmp(payload() + s.offset, s.len, key);
      }
      if (c < 0) {
        lo = static_cast<uint16_t>(mid + 1);
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  // 追加一个条目到条目区顶部（不维护槽序，供 Init/批量装载/追加型布局使用）
  Status AppendEntry(const char* data, uint16_t len, SlotId* out_slot);

  // 在指定槽位插入条目并右移后续槽（保持槽序 = 键序）
  Status InsertEntryAt(SlotId at, const char* data, uint16_t len);

  // 删除槽（只清槽、不搬移其它条目）
  Status EraseEntry(SlotId id);

  // 页内压实：把有效条目重排到条目区顶部并重建槽目录，frag_bytes 归零
  //
  // 【契约】AppendEntry / InsertEntryAt / EraseEntry / Compact 都**不会**自动重算
  // 校验和 —— 校验和由 BufferPool 在脏页 Unpin 时统一调用 UpdateChecksum() 重算
  // （每页每次驻留只算一次，而不是每次写都算）。因此：
  //   * 只读路径可以随时 Validate()；
  //   * 绕过缓冲池直接改页的测试/工具，必须在改完后自己调用 UpdateChecksum()。
  void Compact();

  // 触达一次写入：设置 level/tree_id 之外的元信息由调用者负责
  char* raw() { return raw_; }
  const char* raw() const { return raw_; }

  // 空间统计（用于验收 I3 填充率）
  uint32_t used_bytes() const;
  uint32_t frag_ratio_percent() const;

 private:
  char* raw_;
};

}  // namespace ltm

#include <cstring>
#include <string>
#include <vector>

#include "tests/harness/test_harness.h"
#include "storage/page.h"

using namespace ltm;

namespace {

// 测试夹具：一块 4KB 缓冲 + 已初始化的页
struct PageFixture {
  std::vector<char> raw;
  Page page;

  PageFixture(PageId pid = 1, PageType type = PageType::kBPlusLeaf, uint8_t level = 0)
      : raw(kPageSize, 0), page(raw.data()) {
    page.Init(pid, type, 1, level);
  }
};

std::vector<char> Blob(uint16_t len, char fill) { return std::vector<char>(len, fill); }

}  // namespace

TEST(Page, LayoutSizesMatchContract) {
  // 页布局是冻结契约，尺寸变化必须被立刻发现
  EXPECT_EQ(sizeof(PageHeader), 32u);
  EXPECT_EQ(sizeof(PageTrailer), 16u);
  EXPECT_EQ(sizeof(Slot), 4u);
  EXPECT_EQ(kPagePayloadSize, kPageSize - kPageHeaderSize - kPageTrailerSize);
  EXPECT_EQ(Page::payload_offset(), kPageHeaderSize);
  EXPECT_EQ(Page::trailer_offset(), kPageSize - kPageTrailerSize);
}

TEST(Page, InitProducesValidEmptyPage) {
  PageFixture f(7, PageType::kBPlusLeaf);
  EXPECT_EQ(f.page.header().page_id, 7u);
  EXPECT_TRUE(f.page.header().type == PageType::kBPlusLeaf);
  EXPECT_EQ(f.page.header().tree_id, 1);
  EXPECT_TRUE(f.page.is_leaf());
  EXPECT_EQ(f.page.num_slots(), 0u);
  EXPECT_EQ(f.page.free_space(), kPagePayloadSize);
  EXPECT_EQ(f.page.frag_bytes(), 0u);
  EXPECT_EQ(f.page.header().next_page, kInvalidPageId);
  EXPECT_EQ(f.page.header().prev_page, kInvalidPageId);
  EXPECT_TRUE(f.page.Validate().ok());
}

TEST(Page, AppendEntryStoresContentAndGrowsDownwards) {
  PageFixture f;
  std::vector<char> a = Blob(64, 'a');
  std::vector<char> b = Blob(32, 'b');
  SlotId sa = kInvalidSlot, sb = kInvalidSlot;
  ASSERT_TRUE(f.page.AppendEntry(a.data(), 64, &sa).ok());
  ASSERT_TRUE(f.page.AppendEntry(b.data(), 32, &sb).ok());
  EXPECT_EQ(sa, 0u);
  EXPECT_EQ(sb, 1u);
  EXPECT_EQ(f.page.num_slots(), 2u);
  // 后插入的条目地址更低（条目区自下而上生长）
  EXPECT_TRUE(f.page.ReadSlot(1).offset < f.page.ReadSlot(0).offset);
  EXPECT_EQ(f.page.free_space(), kPagePayloadSize - 2u * sizeof(Slot) - 96u);
  EXPECT_EQ(f.page.used_bytes(), 96u);

  auto ga = f.page.Entry(0);
  ASSERT_TRUE(ga.ok());
  EXPECT_EQ(std::memcmp(ga.value(), a.data(), 64), 0);
  EXPECT_EQ(f.page.EntryLen(1), 32u);
  f.page.UpdateChecksum();
  EXPECT_TRUE(f.page.Validate().ok());
}

TEST(Page, InsertEntryAtKeepsGivenOrder) {
  PageFixture f;
  // 模拟"槽序 == 键序"：按 mid 位置插入，验证后续槽位右移
  std::vector<char> k1 = Blob(8, '1');
  std::vector<char> k3 = Blob(8, '3');
  std::vector<char> k2 = Blob(8, '2');
  ASSERT_TRUE(f.page.InsertEntryAt(0, k1.data(), 8).ok());
  ASSERT_TRUE(f.page.InsertEntryAt(1, k3.data(), 8).ok());
  ASSERT_TRUE(f.page.InsertEntryAt(1, k2.data(), 8).ok());  // 插到中间
  EXPECT_EQ(f.page.num_slots(), 3u);
  char c0 = 0, c1 = 0, c2 = 0;
  ASSERT_TRUE(f.page.Entry(0).ok());
  std::memcpy(&c0, f.page.Entry(0).value(), 1);
  std::memcpy(&c1, f.page.Entry(1).value(), 1);
  std::memcpy(&c2, f.page.Entry(2).value(), 1);
  EXPECT_EQ(c0, '1');
  EXPECT_EQ(c1, '2');
  EXPECT_EQ(c2, '3');
}

TEST(Page, InsertEntryAtRejectsOutOfRange) {
  PageFixture f;
  std::vector<char> k = Blob(8, 'x');
  EXPECT_TRUE(f.page.InsertEntryAt(1, k.data(), 8).code() == StatusCode::kInvalidArgument);
  EXPECT_TRUE(f.page.InsertEntryAt(0, k.data(), 8).ok());
  EXPECT_TRUE(f.page.InsertEntryAt(1, k.data(), 8).ok());
  EXPECT_TRUE(f.page.InsertEntryAt(3, k.data(), 8).code() == StatusCode::kInvalidArgument);
}

TEST(Page, EraseEntryKeepsFragmentationAccounting) {
  PageFixture f;
  std::vector<char> a = Blob(100, 'a');
  std::vector<char> b = Blob(50, 'b');
  f.page.AppendEntry(a.data(), 100, nullptr);
  f.page.AppendEntry(b.data(), 50, nullptr);

  ASSERT_TRUE(f.page.EraseEntry(0).ok());
  EXPECT_EQ(f.page.frag_bytes(), 100u);
  EXPECT_EQ(f.page.free_space(), kPagePayloadSize - 2u * sizeof(Slot) - 150u);  // 空间未回收
  EXPECT_EQ(f.page.live_slots(), 1u);
  EXPECT_TRUE(f.page.Entry(0).status().code() == StatusCode::kNotFound);
  f.page.UpdateChecksum();
  EXPECT_TRUE(f.page.Validate().ok());

  // 二次删除同一槽必须报 kNotFound，而不是静默成功
  EXPECT_TRUE(f.page.EraseEntry(0).code() == StatusCode::kNotFound);
  // 越界删除
  EXPECT_TRUE(f.page.EraseEntry(99).code() == StatusCode::kInvalidArgument);
}

TEST(Page, CompactReclaimsSpaceAndPreservesOrder) {
  PageFixture f;
  // 插入 20 个条目，再删掉偶数槽，压实后必须仍剩奇数槽且顺序不变
  for (int i = 0; i < 20; ++i) {
    std::vector<char> v = Blob(static_cast<uint16_t>(20 + i), static_cast<char>('A' + i));
    ASSERT_TRUE(f.page.AppendEntry(v.data(), static_cast<uint16_t>(v.size()), nullptr).ok());
  }
  uint32_t used_before = f.page.used_bytes();
  for (int i = 0; i < 20; i += 2) {
    ASSERT_TRUE(f.page.EraseEntry(static_cast<SlotId>(i)).ok());
  }
  f.page.Compact();
  EXPECT_EQ(f.page.num_slots(), 10u);
  EXPECT_EQ(f.page.frag_bytes(), 0u);
  EXPECT_EQ(f.page.live_slots(), 10u);
  EXPECT_TRUE(f.page.used_bytes() < used_before);

  // 剩下的应是原来的奇数槽，内容与顺序都保持
  for (uint16_t i = 0; i < 10; ++i) {
    auto e = f.page.Entry(i);
    ASSERT_TRUE(e.ok());
    char c = 0;
    std::memcpy(&c, e.value(), 1);
    EXPECT_EQ(c, static_cast<char>('A' + (2 * i + 1)));
  }
  // 校验和由缓冲池在脏页 Unpin 时统一重算，页级算子不自动更新，故此处显式重算
  f.page.UpdateChecksum();
  EXPECT_TRUE(f.page.Validate().ok());
}

TEST(Page, CompactRecoversSlotDirectorySpace) {
  // 专门回归 bug：Compact 把已删槽从槽目录移除后，必须把那段槽目录空间
  // 也算回 free_space。第一版实现漏算，导致压实后 free_space 偏小。
  PageFixture f;
  std::vector<char> v(64, 'z');
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(f.page.AppendEntry(v.data(), 64, nullptr).ok());
  }
  const uint32_t free_all_live = f.page.free_space();
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(f.page.EraseEntry(static_cast<SlotId>(i)).ok());
  }
  f.page.Compact();
  EXPECT_EQ(f.page.num_slots(), 0u);
  // 全部删空并压实后，空闲空间必须回到整页载荷（含被回收的 8 个槽位）
  EXPECT_EQ(f.page.free_space(), kPagePayloadSize);
  EXPECT_TRUE(f.page.free_space() > free_all_live);
  f.page.UpdateChecksum();
  EXPECT_TRUE(f.page.Validate().ok());
}

TEST(Page, CompactOnCleanPageIsNoop) {
  PageFixture f;
  std::vector<char> a = Blob(40, 'a');
  f.page.AppendEntry(a.data(), 40, nullptr);
  const uint16_t slots = f.page.num_slots();
  const uint32_t free_before = f.page.free_space();
  f.page.Compact();  // frag_bytes == 0，应直接返回
  EXPECT_EQ(f.page.num_slots(), slots);
  EXPECT_EQ(f.page.free_space(), free_before);
}

TEST(Page, PageFullIsReportedPrecisely) {
  PageFixture f;
  std::vector<char> big(static_cast<size_t>(kPagePayloadSize), 'x');
  Status s = f.page.AppendEntry(big.data(), static_cast<uint16_t>(big.size()), nullptr);
  EXPECT_TRUE(s.code() == StatusCode::kPageFull);
  EXPECT_EQ(f.page.num_slots(), 0u);
  EXPECT_TRUE(f.page.free_space() == kPagePayloadSize);
}

TEST(Page, RejectsZeroLengthEntry) {
  PageFixture f;
  EXPECT_TRUE(f.page.AppendEntry(nullptr, 0, nullptr).code() == StatusCode::kInvalidArgument);
  EXPECT_TRUE(f.page.AppendEntry("", 0, nullptr).code() == StatusCode::kInvalidArgument);
}

TEST(Page, ValidateDetectsHeaderTampering) {
  PageFixture f(3, PageType::kBPlusLeaf);
  f.page.mutable_header()->page_id = 99;  // 与 trailer.page_id_copy 不一致
  Status s = f.page.Validate();
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.code() == StatusCode::kCorruption);
}

TEST(Page, ValidateDetectsPayloadTampering) {
  PageFixture f;
  std::vector<char> a = Blob(64, 'a');
  f.page.AppendEntry(a.data(), 64, nullptr);
  f.page.UpdateChecksum();
  ASSERT_TRUE(f.page.Validate().ok());
  // 改一个字节但不重算校验和
  f.page.mutable_payload()[100] = static_cast<char>(f.page.mutable_payload()[100] ^ 0x5A);
  Status s = f.page.Validate();
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.code() == StatusCode::kChecksumMismatch);
}

TEST(Page, UpdateChecksumMakesTamperedPageValidAgain) {
  PageFixture f;
  std::vector<char> a = Blob(64, 'a');
  f.page.AppendEntry(a.data(), 64, nullptr);
  f.page.mutable_payload()[100] = static_cast<char>(f.page.mutable_payload()[100] ^ 0x5A);
  EXPECT_FALSE(f.page.Validate().ok());
  f.page.UpdateChecksum();
  EXPECT_TRUE(f.page.Validate().ok());
}

TEST(Page, ValidateDetectsTornPageLsnMismatch) {
  PageFixture f;
  f.page.mutable_header()->page_lsn = 12345;
  // 只更新 header 里的 lsn，不动 trailer 副本 → 视为撕裂写
  Status s = f.page.Validate();
  EXPECT_FALSE(s.ok());
}

TEST(Page, SlotReadOutOfRangeIsSafe) {
  PageFixture f;
  Slot s = f.page.ReadSlot(5);
  EXPECT_EQ(s.len, 0);
  EXPECT_EQ(s.offset, kSlotEmpty);
  EXPECT_EQ(f.page.EntryLen(5), 0u);
  EXPECT_TRUE(f.page.Entry(5).status().code() == StatusCode::kNotFound);
}

TEST(Page, FragRatioIsSane) {
  PageFixture f;
  std::vector<char> a = Blob(100, 'a');
  f.page.AppendEntry(a.data(), 100, nullptr);
  EXPECT_EQ(f.page.frag_ratio_percent(), 0u);
  f.page.EraseEntry(0);
  EXPECT_EQ(f.page.frag_ratio_percent(), 100u);
}

TEST(Page, LowerBoundFindsFirstGreaterOrEqual) {
  PageFixture f;
  // 用 KeyId 作为条目内容，验证二分定位（供 B+ 树页内查找复用）
  const uint64_t keys[] = {10, 20, 30, 40, 50};
  for (uint64_t k : keys) {
    ASSERT_TRUE(f.page.AppendEntry(reinterpret_cast<const char*>(&k), sizeof(k), nullptr).ok());
  }
  auto cmp = [](const void* entry, uint16_t len, const void* key) -> int {
    if (len < sizeof(uint64_t)) {
      return 0;
    }
    uint64_t a = 0;
    std::memcpy(&a, entry, sizeof(uint64_t));
    const uint64_t b = *static_cast<const uint64_t*>(key);
    return (a < b) ? -1 : (a > b ? 1 : 0);
  };
  for (int i = 0; i < 5; ++i) {
    const uint64_t probe = keys[i];
    EXPECT_EQ(f.page.LowerBound(&probe, cmp), static_cast<SlotId>(i));
  }
  const uint64_t k15 = 15, k55 = 55, k5 = 5;
  EXPECT_EQ(f.page.LowerBound(&k15, cmp), 1u);
  EXPECT_EQ(f.page.LowerBound(&k55, cmp), 5u);
  EXPECT_EQ(f.page.LowerBound(&k5, cmp), 0u);
}

TEST(Page, RandomOperationsPreserveInvariants) {
  // 属性测试：随机 Append/InsertAt/Erase/Compact 序列后，页必须始终满足
  //   * Validate() 只在校验和重算后通过
  //   * num_slots / free_space / frag_bytes / used_bytes 相互自洽
  //   * 所有存活槽都能完整读回，且内容与写入内容一致
  PageFixture f(11, PageType::kBPlusLeaf);
  struct Rec {
    SlotId slot;
    std::vector<char> data;
  };
  std::vector<Rec> live;
  uint32_t rng = 12345;
  auto next = [&rng]() {
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7FFF;
  };

  for (int step = 0; step < 3000; ++step) {
    const uint32_t op = next() % 100;
    if (op < 55 || live.empty()) {
      // 追加
      const uint16_t len = static_cast<uint16_t>(1 + next() % 40);
      std::vector<char> data(len, static_cast<char>('a' + (next() % 26)));
      SlotId s = kInvalidSlot;
      if (f.page.AppendEntry(data.data(), len, &s).ok()) {
        live.push_back(Rec{s, data});
      }
    } else if (op < 75) {
      // 删除一个随机存活槽
      const size_t idx = next() % live.size();
      if (f.page.EraseEntry(live[idx].slot).ok()) {
        live.erase(live.begin() + static_cast<long>(idx));
      }
    } else if (op < 80) {
      f.page.Compact();
      // 压实会重排槽号：用内容重建槽号映射
      for (size_t i = 0; i < live.size(); ++i) {
        live[i].slot = static_cast<SlotId>(i);
      }
    }
    // 每步都校验记账自洽
    uint32_t live_bytes = 0;
    for (uint16_t i = 0; i < f.page.num_slots(); ++i) {
      Slot s = f.page.ReadSlot(i);
      if (s.len == 0 || s.offset == kSlotEmpty) {
        continue;
      }
      live_bytes += s.len;
    }
    // 记账恒等式（free_space 的权威定义）
    ASSERT_EQ(static_cast<uint32_t>(f.page.num_slots()) * sizeof(Slot) + f.page.free_space() +
                  f.page.used_bytes(),
              static_cast<uint32_t>(kPagePayloadSize));
    // used_bytes() 是"条目区跨度"（含碎片空洞），故必然 >= 存活条目字节之和。
    // 这里断言下界而不是相等 —— 相等只在压实后成立（见下面的 CompactionExactness）。
    EXPECT_GE(f.page.used_bytes(), live_bytes);
    EXPECT_LE(f.page.used_bytes(), kPagePayloadSize);
  }

  // 全部存量数据必须能按内容读回
  for (const Rec& r : live) {
    auto e = f.page.Entry(r.slot);
    ASSERT_TRUE(e.ok());
    ASSERT_EQ(f.page.EntryLen(r.slot), static_cast<uint16_t>(r.data.size()));
    EXPECT_EQ(std::memcmp(e.value(), r.data.data(), r.data.size()), 0);
  }
  f.page.UpdateChecksum();
  EXPECT_TRUE(f.page.Validate().ok());
}

TEST(Page, CompactionExactnessAfterRandomOps) {
  // 随机操作后压实：此时 used_bytes() 必须**恰好**等于存活条目字节之和
  // （碎片全部回收，条目区无空洞）。
  PageFixture f(12, PageType::kBPlusLeaf);
  uint32_t rng = 999;
  auto next = [&rng]() {
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7FFF;
  };
  std::vector<uint16_t> lens;
  for (int i = 0; i < 40; ++i) {
    const uint16_t len = static_cast<uint16_t>(1 + next() % 60);
    std::vector<char> data(len, 'q');
    if (f.page.AppendEntry(data.data(), len, nullptr).ok()) {
      lens.push_back(len);
    }
  }
  for (size_t i = 0; i < lens.size(); i += 2) {
    f.page.EraseEntry(static_cast<SlotId>(i));
    lens[i] = 0;
  }
  f.page.Compact();
  uint32_t expect = 0;
  for (uint16_t i = 0; i < f.page.num_slots(); ++i) {
    Slot s = f.page.ReadSlot(i);
    if (s.len != 0 && s.offset != kSlotEmpty) {
      expect += s.len;
    }
  }
  EXPECT_EQ(f.page.frag_bytes(), 0u);
  EXPECT_EQ(f.page.used_bytes(), expect);
  EXPECT_EQ(static_cast<uint32_t>(f.page.num_slots()) * sizeof(Slot) + f.page.free_space() +
                f.page.used_bytes(),
            static_cast<uint32_t>(kPagePayloadSize));
  f.page.UpdateChecksum();
  EXPECT_TRUE(f.page.Validate().ok());
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

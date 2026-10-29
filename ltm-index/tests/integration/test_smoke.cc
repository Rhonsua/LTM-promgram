#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "tests/harness/test_harness.h"
#include "index/hybrid_index.h"
#include "migration/hot_cold_tracker.h"
#include "migration/migration_policy.h"
#include "storage/disk_manager.h"

using namespace ltm;

// 集成层冒烟：验证各层能被组装起来、参数能贯通、生命周期不崩。
// M0 阶段这些用例只断言"明确未实现 + 不崩溃"，M1..M5 完成后逐步改为真实断言。

TEST(Integration, DiskManagerConstructsAndReportsOptions) {
  DiskOptions o;
  o.page_size = 4096;
  o.page_file = "data/_test_index.data";
  o.wal_file = "data/_test_index.wal";
  DiskManager dm(o);
  EXPECT_EQ(dm.page_size(), 4096u);
  EXPECT_EQ(dm.num_pages(), o.num_pages);
  EXPECT_EQ(dm.stats().page_reads, 0u);
  EXPECT_EQ(dm.stats().fsyncs, 0u);
  // 页读写已实现：全新数据文件读第 0 页应成功并补零（不报错、不返回假成功）
  std::vector<char> buf(kPageSize, 0);
  EXPECT_TRUE(dm.ReadPage(0, buf.data()).ok());
  EXPECT_EQ(dm.stats().page_reads, 1u);
  EXPECT_TRUE(dm.WritePage(0, buf.data()).ok());
}

TEST(Integration, HotColdTrackerDecaysAndCools) {
  MigrationPolicyOptions o;
  o.hot_delta = 8;
  o.decay_shift = 3;
  o.cold_threshold = 32;
  HotColdTracker tracker(o);

  EXPECT_EQ(tracker.heat(100), 0);
  EXPECT_TRUE(tracker.IsCold(100));

  // 连续触达应让页变热
  for (int i = 0; i < 5; ++i) {
    tracker.Touch(100, false);
  }
  EXPECT_EQ(tracker.heat(100), 40);
  EXPECT_FALSE(tracker.IsCold(100));
  EXPECT_TRUE(tracker.ref_bit(100));

  // 衰减：40 -> 40-5=35
  tracker.DecayAll();
  EXPECT_EQ(tracker.heat(100), 35);
  EXPECT_FALSE(tracker.ref_bit(100));  // 衰减同时清参考位（供 CLOCK 使用）

  // 反复衰减最终必然变冷
  for (int i = 0; i < 20; ++i) {
    tracker.DecayAll();
  }
  EXPECT_TRUE(tracker.IsCold(100));
}

TEST(Integration, HotColdTrackerPrefetchWeightedLower) {
  MigrationPolicyOptions o;
  o.hot_delta = 8;
  o.prefetch_weight_pct = 50;
  HotColdTracker t(o);
  t.Touch(1, /*is_prefetch=*/false);
  t.Touch(2, /*is_prefetch=*/true);
  EXPECT_EQ(t.heat(1), 8);
  EXPECT_EQ(t.heat(2), 4);  // 预取只按 50% 计，防止顺序扫描污染热集
  EXPECT_EQ(t.stats().prefetch_touches, 1u);
}

TEST(Integration, HotColdTrackerHeatSaturates) {
  MigrationPolicyOptions o;
  o.hot_delta = 8;
  HotColdTracker t(o);
  for (int i = 0; i < 100; ++i) {
    t.Touch(1, false);
  }
  EXPECT_EQ(t.heat(1), 255);  // 必须饱和在 uint8 上限
}

TEST(Integration, HotColdTrackerForgetShrinksTable) {
  MigrationPolicyOptions o;
  HotColdTracker t(o);
  for (PageId p = 0; p < 100; ++p) {
    t.Touch(p, false);
  }
  EXPECT_EQ(t.tracked_pages(), 100u);
  for (PageId p = 0; p < 100; ++p) {
    t.Forget(p);
  }
  EXPECT_EQ(t.tracked_pages(), 0u);
}

TEST(Integration, ColdestInPoolOnlyConsidersPooledPages) {
  MigrationPolicyOptions o;
  o.hot_delta = 10;
  o.cold_threshold = 32;
  HotColdTracker t(o);
  for (PageId p = 1; p <= 5; ++p) {
    t.Touch(p, false);
    t.SetInPool(p, true);
  }
  t.SetInPool(1, false);  // 页 1 已不在池内
  auto cold = t.ColdestInPool(2);
  ASSERT_EQ(cold.size(), 2u);
  // 所有页热度相同，返回顺序按 (heat, pid) 稳定排序，不应包含已离池的页 1
  for (PageId p : cold) {
    EXPECT_TRUE(p != 1u);
  }
}

TEST(Integration, MigrationPolicyScoresNonLeafAsCostlierToEvict) {
  MigrationPolicy p(MigrationPolicyOptions{});
  PageHeader leaf{};
  leaf.level = 0;
  PageHeader internal{};
  internal.level = 1;
  const int32_t leaf_score = p.EvictScore(leaf, /*heat=*/50, false);
  const int32_t internal_score = p.EvictScore(internal, 50, false);
  // 分值越小越该被驱逐；非叶页应更"不该被驱逐"
  EXPECT_TRUE(internal_score > leaf_score);
  // 钉住页不可驱逐
  EXPECT_TRUE(p.EvictScore(leaf, 0, true) > 1000000);
  // 热度越高越不该被驱逐（方向极易写反，必须锁住）
  EXPECT_TRUE(p.EvictScore(leaf, 200, false) > p.EvictScore(leaf, 10, false));
}

TEST(Integration, MigrationPolicyTriggersFlushAtHighWater) {
  MigrationPolicyOptions o;
  o.dirty_flush_high_water = 512;
  MigrationPolicy p(o);
  EXPECT_FALSE(p.ShouldFlushDirty(100));
  EXPECT_TRUE(p.ShouldFlushDirty(512));
  EXPECT_TRUE(p.ShouldFlushDirty(1000));
}

TEST(Integration, EvictionPolicyPicksColdestLeafVictim) {
  MigrationPolicyOptions o;
  o.nonleaf_evict_penalty = 40;
  MigrationPolicy p(o);
  ClockEvictor ep;
  // 四个帧：0=叶热, 1=叶冷, 2=非叶冷, 3=叶冷且钉住
  struct FrameInfo {
    uint8_t heat;
    uint8_t level;
    bool pinned;
  };
  // 五个帧，覆盖策略里全部三条规则：
  //   0 叶 热(200)          -> 不该被驱逐
  //   1 叶 最冷(0)          -> 首选
  //   2 非叶 冷(0)          -> 因 nonleaf_evict_penalty 排在叶之后
  //   3 叶 冷(0) 但被钉住   -> 必须跳过
  //   4 叶 冷(0)            -> 次选
  FrameInfo frames[5] = {{200, 0, false}, {0, 0, false}, {0, 1, false}, {0, 0, true}, {0, 0, false}};
  auto probe = [](uint32_t) { return true; };
  auto info = [&frames](uint32_t idx, PageHeader* header, uint8_t* heat, bool* pinned) {
    *header = PageHeader{};
    header->level = frames[idx].level;
    header->flags = 0;
    *heat = frames[idx].heat;
    *pinned = frames[idx].pinned;
  };
  uint32_t hand = 0;
  uint32_t victim = 999;
  ASSERT_TRUE(ep.PickVictim(5, &hand, probe, info, p, &victim));
  // 选中的必须是未钉住的叶页（1 或 4）；被钉住的 3 号绝不能被选中。
  // 失败时先看 MigrationPolicy::EvictScore 的热度符号方向 —— 本测试专门锁这个方向。
  EXPECT_TRUE(victim == 1u || victim == 4u);
  EXPECT_TRUE(victim != 3u);

  // 打分单调性（把策略语义固化成断言，防止后来者改坏）：
  //   1) 同层页，热度越高越不该被驱逐
  //   2) 热度都高于 nonleaf_evict_penalty 时，非叶页整体晚于叶页
  //   3) 非叶页冷到低于 nonleaf_evict_penalty 时，惩罚被取消（允许换出）
  //   4) 钉住页分值极大，等价于"不可驱逐"
  PageHeader leaf{};
  leaf.level = 0;
  PageHeader internal{};
  internal.level = 1;
  EXPECT_TRUE(p.EvictScore(leaf, 200, false) > p.EvictScore(leaf, 10, false));
  EXPECT_TRUE(p.EvictScore(leaf, 100, false) < p.EvictScore(internal, 100, false));  // 非叶更晚
  EXPECT_TRUE(p.EvictScore(internal, 0, false) < p.EvictScore(internal, 100, false));  // 极冷非叶可换出
  EXPECT_TRUE(p.EvictScore(leaf, 0, true) > 1000000);
}

TEST(Integration, EvictionPolicyReportsNoVictimWhenAllPinned) {
  MigrationPolicy p(MigrationPolicyOptions{});
  ClockEvictor ep;
  auto probe = [](uint32_t) { return true; };
  auto info = [](uint32_t, PageHeader* header, uint8_t* heat, bool* pinned) {
    *header = PageHeader{};
    *heat = 0;
    *pinned = true;
  };
  uint32_t hand = 0;
  uint32_t victim = 0;
  EXPECT_FALSE(ep.PickVictim(4, &hand, probe, info, p, &victim));
}

// M5 起 HybridIndex 已经真的能打开：这里检查"占位已经撤掉"这条最小事实。
// 完整的混合索引验收在 tests/index/test_hybrid_diff.cc（差分 + I8 + 持久化）。
TEST(Integration, HybridIndexOpensWithDefaultOptions) {
  const std::string page_file = "log/_smoke_hybrid.data";
  const std::string wal_file = "log/_smoke_hybrid.wal";
#if defined(_WIN32)
  std::system("if not exist log mkdir log");
#else
  std::system("mkdir -p log");
#endif
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());

  HybridIndex idx;
  EXPECT_FALSE(idx.is_open());
  HybridOptions o;
  o.config.disk.page_file = page_file;
  o.config.disk.wal_file = wal_file;
  o.config.pool.pool_frames = 16;
  Status s = idx.Open(o);
  EXPECT_TRUE(s.ok());
  EXPECT_TRUE(idx.is_open());
  if (s.ok()) {
    // 默认 2 维：一条记录写得进、查得到、两棵树一致
    IndexEntry e;
    e.key = 42;
    e.mbr = Mbr::Invalid(2);
    e.mbr.lo.resize(2);
    e.mbr.hi.resize(2);
    for (uint16_t d = 0; d < 2; ++d) {
      e.mbr.lo[d] = 1.0;
      e.mbr.hi[d] = 2.0;
    }
    e.payload = {'o', 'k'};
    EXPECT_TRUE(idx.Put(e).ok());
    auto got = idx.Get(42);
    EXPECT_TRUE(got.ok());
    EXPECT_TRUE(idx.Validate().ok());
    EXPECT_TRUE(idx.Close().ok());
    EXPECT_FALSE(idx.is_open());
  }
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

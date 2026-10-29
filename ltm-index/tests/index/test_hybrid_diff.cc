// tests/index/test_hybrid_diff.cc
// M5：混合索引的差分测试 + I8 双树一致性 + 持久化端到端（方案 §6 的 M5 验收）。
//
// 【判据】混合索引对外只承诺三件事，这里逐条对拍：
//   1. Get(key) 返回的记录（键 / MBR / 载荷）与真值表逐字段相同；
//   2. 一维范围查询、空间相交/包含查询、KNN 与暴力扫描**逐元素等价**；
//   3. 两棵树始终指向同一份记录集（不变量 I8）—— 两个方向都查，且**这条检查
//      本身是可证伪的**（用例 I 会故意制造一次不一致，证明它真的会报）。
//
// 【"B+ 叶条目里存 R 树叶页号"这条设计的实测口径】它只是提示：R 树的借键与合并
// 会把记录搬到兄弟页、并释放原页。用例 E 专门把 R 树压到发生合并，再逐条 Get，
// 证明提示失效时会退化为叶链全扫（结果仍然正确），并给出命中率 ——
// 这个数字是 M6 基准里"提示到底值不值"的依据。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "index/hybrid_index.h"
#include "index/rtree.h"
#include "storage/buffer_pool.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {

constexpr uint16_t kDim = 2;
constexpr uint64_t kKeys = 400;

struct Env {
  std::string page_file;
  std::string wal_file;
  std::unique_ptr<HybridIndex> index;
  // 崩溃现场用例要在析构后**保留**文件，让下一次 Open 能对同一份现场做恢复
  bool keep_files = false;

  explicit Env(const std::string& tag, uint32_t frames = 48, bool wipe = true)
      : page_file("log/_hybrid_" + tag + ".data"), wal_file("log/_hybrid_" + tag + ".wal") {
#if defined(_WIN32)
    std::system("if not exist log mkdir log");
#else
    std::system("mkdir -p log");
#endif
    if (wipe) {
      std::remove(page_file.c_str());
      std::remove(wal_file.c_str());
    }
    HybridOptions opt;
    opt.config.disk.page_file = page_file;
    opt.config.disk.wal_file = wal_file;
    opt.config.disk.num_pages = 1u << 14;
    opt.config.pool.pool_frames = frames;  // 故意小：逼出"事务中途淘汰脏页"
    opt.bplus.order_or_capacity = 32;
    opt.rtree.order_or_capacity = 32;
    opt.rtree.mbr_dim = kDim;
    opt.rtree.split_policy = "quadratic";
    index.reset(new HybridIndex());
    const Status s = index->Open(opt);
    if (!s.ok()) {
      std::printf("  HybridIndex::Open failed: %s\n", s.ToString().c_str());
    }
  }

  ~Env() {
    index.reset();
    if (!keep_files) {
      std::remove(page_file.c_str());
      std::remove(wal_file.c_str());
    }
  }

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
};

Mbr RandomMbr(std::mt19937_64* rng, double span = 1000.0) {
  std::uniform_real_distribution<double> coord(0.0, span);
  std::uniform_real_distribution<double> extent(0.0, 8.0);
  Mbr m = Mbr::Invalid(kDim);
  m.lo.resize(kDim);
  m.hi.resize(kDim);
  for (uint16_t d = 0; d < kDim; ++d) {
    const double c = coord(*rng);
    const double e = extent(*rng);
    m.lo[d] = c - e;
    m.hi[d] = c + e;
  }
  return m;
}

std::vector<char> PayloadFor(uint64_t key, size_t len) {
  std::vector<char> p(len, 'x');
  for (size_t i = 0; i < len; ++i) {
    p[i] = static_cast<char>('a' + ((key + i * 3) % 26));
  }
  return p;
}

IndexEntry MakeEntry(uint64_t key, const Mbr& mbr) {
  IndexEntry e;
  e.key = key;
  e.mbr = mbr;
  e.payload = PayloadFor(key, 8 + (key % 17));
  return e;
}

using Truth = std::map<uint64_t, IndexEntry>;

bool SameMbr(const Mbr& a, const Mbr& b) {
  if (a.lo.dim() != b.lo.dim() || a.hi.dim() != b.hi.dim()) {
    return false;
  }
  for (uint16_t d = 0; d < a.lo.dim(); ++d) {
    if (a.lo[d] != b.lo[d] || a.hi[d] != b.hi[d]) {
      return false;
    }
  }
  return true;
}

bool MbrIntersects(const Mbr& a, const Mbr& b) {
  for (uint16_t d = 0; d < a.lo.dim(); ++d) {
    if (a.hi[d] < b.lo[d] || b.hi[d] < a.lo[d]) {
      return false;
    }
  }
  return true;
}

bool MbrContains(const Mbr& outer, const Mbr& inner) {
  for (uint16_t d = 0; d < outer.lo.dim(); ++d) {
    if (inner.lo[d] < outer.lo[d] || inner.hi[d] > outer.hi[d]) {
      return false;
    }
  }
  return true;
}

double MbrDistance(const Mbr& a, const Point& p) {
  double sum = 0.0;
  for (uint16_t d = 0; d < a.lo.dim(); ++d) {
    double delta = 0.0;
    if (p[d] < a.lo[d]) {
      delta = a.lo[d] - p[d];
    } else if (p[d] > a.hi[d]) {
      delta = p[d] - a.hi[d];
    }
    sum += delta * delta;
  }
  return sum;  // 平方距离足够比较（省一次开方）
}

// 全量对照：真值表里每一条都必须能被 Get 到，且键/MBR/载荷逐字段相同
size_t CrossCheckAll(HybridIndex* idx, const Truth& truth, const char* what) {
  size_t bad = 0;
  for (const auto& kv : truth) {
    auto got = idx->Get(kv.first);
    if (!got.ok()) {
      ++bad;
      if (bad <= 3) {
        std::printf("  [%s] key %llu not found: %s\n", what,
                    static_cast<unsigned long long>(kv.first), got.status().ToString().c_str());
      }
      continue;
    }
    const IndexEntry& e = got.value();
    const IndexEntry& want = kv.second;
    if (e.payload != want.payload) {
      ++bad;
      if (bad <= 3) {
        std::printf("  [%s] key %llu payload mismatch\n", what,
                    static_cast<unsigned long long>(kv.first));
      }
    } else if (!SameMbr(e.mbr, want.mbr)) {
      ++bad;
      if (bad <= 3) {
        std::printf("  [%s] key %llu MBR mismatch\n", what,
                    static_cast<unsigned long long>(kv.first));
      }
    }
  }
  return bad;
}

}  // namespace

// ---------------------------------------------------------------------------
// A. 基本往返：Put / Get / Erase
// ---------------------------------------------------------------------------
TEST(HybridDiff, A_PutGetEraseRoundTrip) {
  Env env("basic");
  std::mt19937_64 rng(7);
  Truth truth;
  for (uint64_t k = 1; k <= 200; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng));
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }
  ASSERT_TRUE(env.index->Validate().ok());
  EXPECT_EQ(CrossCheckAll(env.index.get(), truth, "A"), static_cast<size_t>(0));

  // 覆盖式 Put：同键换一个 MBR 与载荷，两棵树都必须更新
  for (uint64_t k = 1; k <= 50; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng, 50.0));
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }
  EXPECT_EQ(CrossCheckAll(env.index.get(), truth, "A-overwrite"), static_cast<size_t>(0));

  // 删一半
  for (uint64_t k = 1; k <= 100; ++k) {
    ASSERT_TRUE(env.index->Erase(k).ok());
    truth.erase(k);
  }
  EXPECT_EQ(CrossCheckAll(env.index.get(), truth, "A-erase"), static_cast<size_t>(0));
  auto gone = env.index->Get(1);
  EXPECT_FALSE(gone.ok());
  EXPECT_TRUE(gone.status().code() == StatusCode::kNotFound);
  ASSERT_TRUE(env.index->Validate().ok());
  EXPECT_EQ(env.index->stats().bplus.num_entries,
            static_cast<uint64_t>(truth.size()));
  EXPECT_EQ(env.index->stats().rtree.num_entries,
            static_cast<uint64_t>(truth.size()));
}

// ---------------------------------------------------------------------------
// B. 随机差分：Put / Erase / Get 与真值表对拍
// ---------------------------------------------------------------------------
TEST(HybridDiff, B_RandomDifferentialAgainstTruthTable) {
  Env env("diff");
  std::mt19937_64 rng(20240927);
  Truth truth;
  const uint64_t kSpace = 500;
  size_t inserted = 0;
  size_t erased = 0;
  for (int round = 0; round < 12; ++round) {
    for (int i = 0; i < 120; ++i) {
      const uint64_t key = 1 + (rng() % kSpace);
      if (rng() % 100 < 35) {
        const Status s = env.index->Erase(key);
        if (s.ok()) {
          truth.erase(key);
          ++erased;
        } else if (s.code() != StatusCode::kNotFound) {
          LTM_FAIL_(std::string("Erase failed: ") + s.ToString());
          return;
        }
      } else {
        IndexEntry e = MakeEntry(key, RandomMbr(&rng));
        ASSERT_TRUE(env.index->Put(e).ok());
        truth[key] = e;
        ++inserted;
      }
    }
    const size_t bad = CrossCheckAll(env.index.get(), truth, "B");
    if (bad != 0) {
      LTM_FAIL_("differential mismatch after round " + std::to_string(round));
      return;
    }
    ASSERT_TRUE(env.index->Validate().ok());
  }
  std::printf("  differential: %zu inserts / %zu erases, %zu records left, both trees agree\n",
              inserted, erased, truth.size());
  // 混合负载下"B+ 叶条目里存的 R 树叶页号"到底有多准 —— 这个数字进结题报告，
  // 与用例 E 的极端场景（删掉 3/4、页大幅合并）形成对照。
  const IndexStats st = env.index->stats();
  std::printf("  locate under a mixed workload: hint hits %llu, stale hints %llu, chain scans %llu\n",
              static_cast<unsigned long long>(st.locate_hint_hits),
              static_cast<unsigned long long>(st.locate_hint_misses),
              static_cast<unsigned long long>(st.locate_chain_scans));
  ASSERT_TRUE(env.index->Validate().ok());
  EXPECT_EQ(env.index->stats().bplus.num_entries, static_cast<uint64_t>(truth.size()));
  EXPECT_EQ(env.index->stats().rtree.num_entries, static_cast<uint64_t>(truth.size()));
}

// ---------------------------------------------------------------------------
// C. 一维范围查询 vs 暴力
// ---------------------------------------------------------------------------
TEST(HybridDiff, C_RangeQueryMatchesBruteForce) {
  Env env("range");
  std::mt19937_64 rng(11);
  Truth truth;
  for (uint64_t k = 1; k <= kKeys; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng));
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }
  const uint64_t ranges[][2] = {{1, 50}, {100, 200}, {0, kKeys}, {380, 400}, {401, 900}};
  for (const auto& r : ranges) {
    Query q;
    q.kind = QueryKind::kRange1D;
    q.key_lo = r[0];
    q.key_hi = r[1];
    std::vector<uint64_t> got;
    ASSERT_TRUE(env.index->Query(q, [&got](const IndexEntry& e) {
      got.push_back(e.key);
      return true;
    }).ok());
    std::vector<uint64_t> want;
    for (const auto& kv : truth) {
      if (kv.first >= r[0] && kv.first <= r[1]) {
        want.push_back(kv.first);
      }
    }
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got.size(), want.size());
    if (got != want) {
      LTM_FAIL_("range [" + std::to_string(r[0]) + "," + std::to_string(r[1]) + "] mismatch");
    }
  }
}

// ---------------------------------------------------------------------------
// D. 空间查询与 KNN vs 暴力
// ---------------------------------------------------------------------------
TEST(HybridDiff, D_SpatialAndKnnMatchBruteForce) {
  Env env("spatial");
  std::mt19937_64 rng(13);
  Truth truth;
  for (uint64_t k = 1; k <= kKeys; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng));
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }

  // 相交
  {
    Mbr window = Mbr::Invalid(kDim);
    window.lo.resize(kDim);
    window.hi.resize(kDim);
    for (uint16_t d = 0; d < kDim; ++d) {
      window.lo[d] = 300.0;
      window.hi[d] = 700.0;
    }
    Query q;
    q.kind = QueryKind::kIntersect;
    q.mbr = window;
    std::vector<uint64_t> got;
    ASSERT_TRUE(env.index->Query(q, [&got](const IndexEntry& e) {
      got.push_back(e.key);
      return true;
    }).ok());
    std::vector<uint64_t> want;
    for (const auto& kv : truth) {
      if (MbrIntersects(kv.second.mbr, window)) {
        want.push_back(kv.first);
      }
    }
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    std::printf("  spatial intersect: query returned %zu, brute force %zu\n", got.size(),
                want.size());
    EXPECT_EQ(got.size(), want.size());
    if (got != want) {
      LTM_FAIL_("spatial intersect mismatch");
    }
  }

  // 包含
  {
    Mbr window = Mbr::Invalid(kDim);
    window.lo.resize(kDim);
    window.hi.resize(kDim);
    for (uint16_t d = 0; d < kDim; ++d) {
      window.lo[d] = 200.0;
      window.hi[d] = 800.0;
    }
    Query q;
    q.kind = QueryKind::kRangeSpatial;  // = 记录 MBR 被查询框包含
    q.mbr = window;
    std::vector<uint64_t> got;
    ASSERT_TRUE(env.index->Query(q, [&got](const IndexEntry& e) {
      got.push_back(e.key);
      return true;
    }).ok());
    std::vector<uint64_t> want;
    for (const auto& kv : truth) {
      if (MbrContains(window, kv.second.mbr)) {
        want.push_back(kv.first);
      }
    }
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    std::printf("  spatial contains: query returned %zu, brute force %zu\n", got.size(),
                want.size());
    EXPECT_EQ(got.size(), want.size());
    if (got != want) {
      LTM_FAIL_("spatial contains mismatch");
    }
  }

  // KNN：与暴力按距离排序后逐元素比对（距离容差 1e-9）
  {
    Point probe(kDim);
    probe[0] = 500.0;
    probe[1] = 500.0;
    Query q;
    q.kind = QueryKind::kKnn;
    q.probe = probe;
    std::vector<IndexEntry> got;
    ASSERT_TRUE(env.index->Knn(q, 10, &got).ok());
    std::vector<std::pair<double, uint64_t>> want;
    for (const auto& kv : truth) {
      want.emplace_back(MbrDistance(kv.second.mbr, probe), kv.first);
    }
    std::sort(want.begin(), want.end());
    EXPECT_EQ(got.size(), static_cast<size_t>(10));
    if (want.size() >= 10) {
      for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].key != want[i].second) {
          LTM_FAIL_("KNN rank " + std::to_string(i) + " mismatch");
          break;
        }
      }
      // 距离也要对得上：索引侧重新算一遍（用返回记录的 MBR）
      for (size_t i = 0; i < got.size(); ++i) {
        const double d = MbrDistance(got[i].mbr, probe);
        if (d < want[i].first - 1e-9 || d > want[i].first + 1e-9) {
          LTM_FAIL_("KNN distance mismatch at rank " + std::to_string(i));
          break;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// E. Get/Erase 不访问 R 树；提示失效由 I8 交叉校验兜住
// ---------------------------------------------------------------------------
// 【两条断言，各测一件事】
//   1. **Get 完全不碰 R 树**：连做 100 次 Get，R 树的定位计数必须纹丝不动。
//      这条是"把 MBR 内联进 B+ 载荷"这个设计的直接证据 —— 方案 §4.9 原本打算
//      "只存 R 树叶页号、用时再去 R 树取 MBR"，实测那条路走不通（见 .cc 的注释）：
//      R 树分裂会把一半条目搬到新页，纯插入 2000 条之后提示命中率只有 ~48%。
//   2. **提示失效不会伤到正确性**：把 R 树压到必然发生合并（容量 32、删掉 3/4），
//      此时大量记录的所在页已经改变，B+ 里那份提示成了陈旧值；`Validate()` 的
//      I8 交叉校验必须仍然全绿（失效时退化为叶链全扫），且要能看到"确实失效过"。
TEST(HybridDiff, E_GetDoesNotTouchTheRTreeAndStaleHintsAreCaught) {
  Env env("stale", /*frames=*/32);
  std::mt19937_64 rng(17);
  Truth truth;
  for (uint64_t k = 1; k <= kKeys; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng, 100.0));  // 范围小 → 页内条目多 → 分裂/合并频繁
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }

  // 1) Get 不访问 R 树：定位计数必须不变
  {
    const IndexStats before = env.index->stats();
    uint64_t hits = 0;
    for (uint64_t k = 1; k <= 100; ++k) {
      auto got = env.index->Get(k);
      if (got.ok()) {
        ++hits;
      }
    }
    const IndexStats after = env.index->stats();
    EXPECT_EQ(hits, static_cast<uint64_t>(100));
    EXPECT_EQ(after.locate_hint_hits, before.locate_hint_hits);
    EXPECT_EQ(after.locate_hint_misses, before.locate_hint_misses);
    EXPECT_EQ(after.locate_chain_scans, before.locate_chain_scans);
    if (after.locate_chain_scans != before.locate_chain_scans) {
      LTM_FAIL_("Get went to the R tree; the MBR must come from the B+ side");
    }
  }

  // 2) 删掉 3/4：R 树的页大幅合并，大量记录的所在页因此改变
  for (uint64_t k = 1; k <= kKeys; ++k) {
    if (k % 4 != 0) {
      ASSERT_TRUE(env.index->Erase(k).ok());
      truth.erase(k);
    }
  }
  ASSERT_TRUE(env.index->Validate().ok());
  const IndexStats before = env.index->stats();
  EXPECT_EQ(CrossCheckAll(env.index.get(), truth, "E"), static_cast<size_t>(0));
  // I8 交叉校验（Validate）才用提示：这里再跑一次并统计它的代价
  ASSERT_TRUE(env.index->Validate().ok());
  const IndexStats after = env.index->stats();
  const uint64_t hits = after.locate_hint_hits - before.locate_hint_hits;
  const uint64_t misses = after.locate_hint_misses - before.locate_hint_misses;
  const uint64_t scans = after.locate_chain_scans - before.locate_chain_scans;
  std::printf("  I8 cross-check after heavy merges: hint hits %llu, stale hints %llu, chain scans %llu\n",
              static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses),
              static_cast<unsigned long long>(scans));
  // 提示确实会失效（否则这条用例没有测到"兜底"这件事）
  EXPECT_TRUE(misses > 0);
  EXPECT_EQ(hits + misses, static_cast<uint64_t>(truth.size()));
  ASSERT_TRUE(env.index->Validate().ok());
}

// ---------------------------------------------------------------------------
// F. 关闭后重开：全量数据可查（M5-4 持久化端到端）
// ---------------------------------------------------------------------------
TEST(HybridDiff, F_SurvivesCloseAndReopen) {
  Env env("reopen");
  std::mt19937_64 rng(19);
  Truth truth;
  for (uint64_t k = 1; k <= kKeys; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng));
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }
  ASSERT_TRUE(env.index->Close().ok());
  env.index.reset();  // 析构：池的析构会再确认一次干净关闭

  // 重开同一份文件
  HybridOptions opt;
  opt.config.disk.page_file = env.page_file;
  opt.config.disk.wal_file = env.wal_file;
  opt.config.disk.num_pages = 1u << 14;
  opt.config.pool.pool_frames = 48;
  opt.bplus.order_or_capacity = 32;
  opt.rtree.order_or_capacity = 32;
  opt.rtree.mbr_dim = kDim;
  HybridIndex again;
  ASSERT_TRUE(again.Open(opt).ok());
  ASSERT_TRUE(again.Validate().ok());
  EXPECT_EQ(CrossCheckAll(&again, truth, "F"), static_cast<size_t>(0));
  EXPECT_EQ(again.stats().bplus.num_entries, static_cast<uint64_t>(truth.size()));
  EXPECT_EQ(again.stats().rtree.num_entries, static_cast<uint64_t>(truth.size()));
  // 空间查询在重开后依然可用
  Mbr window = Mbr::Invalid(kDim);
  window.lo.resize(kDim);
  window.hi.resize(kDim);
  for (uint16_t d = 0; d < kDim; ++d) {
    window.lo[d] = 250.0;
    window.hi[d] = 750.0;
  }
  Query q;
  q.kind = QueryKind::kIntersect;
  q.mbr = window;
  size_t hits = 0;
  ASSERT_TRUE(again.Query(q, [&hits](const IndexEntry&) {
    ++hits;
    return true;
  }).ok());
  size_t want = 0;
  for (const auto& kv : truth) {
    if (MbrIntersects(kv.second.mbr, window)) {
      ++want;
    }
  }
  EXPECT_EQ(hits, want);
}

// ---------------------------------------------------------------------------
// G. 检查点前后查询结果一致（方案 §6 的一致性验收）
// ---------------------------------------------------------------------------
TEST(HybridDiff, G_CheckpointDoesNotChangeQueryResults) {
  Env env("ckpt");
  std::mt19937_64 rng(23);
  Truth truth;
  for (uint64_t k = 1; k <= 200; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng));
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }
  Query q;
  q.kind = QueryKind::kRange1D;
  q.key_lo = 0;
  q.key_hi = kKeys;
  std::vector<uint64_t> before;
  ASSERT_TRUE(env.index->Query(q, [&before](const IndexEntry& e) {
    before.push_back(e.key);
    return true;
  }).ok());
  const uint64_t wal_before = env.index->disk()->WalSize();
  ASSERT_TRUE(env.index->Checkpoint().ok());
  const uint64_t wal_after = env.index->disk()->WalSize();
  EXPECT_TRUE(wal_after < wal_before);
  std::vector<uint64_t> after;
  ASSERT_TRUE(env.index->Query(q, [&after](const IndexEntry& e) {
    after.push_back(e.key);
    return true;
  }).ok());
  EXPECT_EQ(before.size(), after.size());
  EXPECT_TRUE(before == after);
  // 检查点之后继续写，两棵树仍然一致
  for (uint64_t k = 201; k <= 260; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng));
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }
  ASSERT_TRUE(env.index->Validate().ok());
  EXPECT_EQ(CrossCheckAll(env.index.get(), truth, "G"), static_cast<size_t>(0));
  std::printf("  checkpoint truncated the log from %llu to %llu bytes\n",
              static_cast<unsigned long long>(wal_before), static_cast<unsigned long long>(wal_after));
}

// ---------------------------------------------------------------------------
// H. 双树原子性：未提交的 Put 必须**两棵树一起**回滚
// ---------------------------------------------------------------------------
// 保留真实 Put 的双树页镜像和 WAL 前缀，移除它的最后一条 Commit，并标记非正常关闭。
// 这是提交记录尚未落盘时的恢复夹具，不冒称实际强杀（强杀另由 crash_matrix 覆盖）。
// 不在持有底层写锁时调用 Hybrid API，也不析构仍持锁的缓冲池。
// 重开时恢复必须把这条记录从**两棵树**里一起回滚掉：
//   * 若只回滚一棵，I8 立刻被破坏（Validate 会报，Get 也会报）；
//   * 若两棵都留下，则"未提交的数据落盘了"，同样能被本用例抓住。
TEST(HybridDiff, H_UncommittedPutIsRolledBackInBothTrees) {
  const std::string tag = "atomic";
  std::mt19937_64 rng(29);
  Truth committed;
  IndexEntry uncommitted;
  uint64_t commit_offset = 0;
  {
    Env env(tag);
    env.keep_files = true;
    for (uint64_t k = 1; k <= 150; ++k) {
      IndexEntry e = MakeEntry(k, RandomMbr(&rng));
      ASSERT_TRUE(env.index->Put(e).ok());
      committed[k] = e;
    }
    uncommitted = MakeEntry(90001, RandomMbr(&rng));
    ASSERT_TRUE(env.index->Put(uncommitted).ok());
    const uint64_t total = env.index->disk()->WalSize();
    uint64_t offset = kWalDataStart;
    WalRecordType last_type = WalRecordType::kPageRedo;
    ASSERT_TRUE(ForEachWalRecord(env.index->disk(), [&](const WalRecordHeader& h, const char*) {
      if (h.type == WalRecordType::kCommit) commit_offset = offset;
      offset += h.total_len;
      last_type = h.type;
      return true;
    }).ok());
    ASSERT_EQ(offset, total);
    ASSERT_TRUE(last_type == WalRecordType::kCommit);
    ASSERT_TRUE(commit_offset > kWalDataStart);
    ASSERT_TRUE(env.index->Get(uncommitted.key).ok());
    ASSERT_TRUE(env.index->Validate().ok());
    ASSERT_TRUE(env.index->Close().ok());
    ASSERT_EQ(env.index->disk()->WalSize(), total);
  }
  {
    DiskOptions disk_options;
    disk_options.page_file = "log/_hybrid_" + tag + ".data";
    disk_options.wal_file = "log/_hybrid_" + tag + ".wal";
    DiskManager disk(disk_options);
    WalLogHeader header;
    ASSERT_TRUE(ReadWalLogHeader(&disk, &header).ok());
    header.clean_shutdown = 0;
    ASSERT_TRUE(WriteWalLogHeader(&disk, header).ok());
  }
  std::error_code truncate_error;
  std::filesystem::resize_file("log/_hybrid_" + tag + ".wal", commit_offset, truncate_error);
  ASSERT_TRUE(!truncate_error);
  {
    HybridOptions opt;
    opt.config.disk.page_file = "log/_hybrid_" + tag + ".data";
    opt.config.disk.wal_file = "log/_hybrid_" + tag + ".wal";
    opt.config.disk.num_pages = 1u << 14;
    opt.config.pool.pool_frames = 48;
    opt.bplus.order_or_capacity = 32;
    opt.rtree.order_or_capacity = 32;
    opt.rtree.mbr_dim = kDim;
    HybridIndex again;
    ASSERT_TRUE(again.Open(opt).ok());
    // 未提交的那条必须**两棵树都没有**
    auto gone = again.Get(90001);
    EXPECT_FALSE(gone.ok());
    EXPECT_TRUE(gone.status().code() == StatusCode::kNotFound);
    // I8 必须成立（这正是"只回滚了一棵"会当场暴露的地方）
    const Status v = again.Validate();
    if (!v.ok()) {
      LTM_FAIL_(std::string("I8 violated after rolling back an uncommitted put: ") + v.ToString());
    }
    // 已提交的必须一条不少
    EXPECT_EQ(CrossCheckAll(&again, committed, "H"), static_cast<size_t>(0));
    EXPECT_EQ(again.stats().bplus.num_entries, static_cast<uint64_t>(committed.size()));
    EXPECT_EQ(again.stats().rtree.num_entries, static_cast<uint64_t>(committed.size()));
    ASSERT_TRUE(again.Close().ok());
  }
  // BufferPool 析构仍会同步文件，必须等 HybridIndex 完全释放后再删除。
  std::remove(("log/_hybrid_" + tag + ".data").c_str());
  std::remove(("log/_hybrid_" + tag + ".wal").c_str());
}

// ---------------------------------------------------------------------------
// I. I8 交叉校验必须可证伪：故意让两棵树对同一条记录"说不同的话"
// ---------------------------------------------------------------------------
// 【这条用例是给"检查本身"做体检】一个从不报警的检查与没有检查完全一样
// （本项目缺陷 #37 的形状：ASAN 开关静默失效，一条门槛从未真正执行却被记成通过）。
// 所以这里走一条**真实的缝**：混合索引暴露了 pool()，任何拿到池的代码都能再绑一棵
// R 树到同一个元数据槽位上去 —— 这不是测试专用开关，而是公开 API 的真实能力。
//
// 【为什么制造"MBR 不一致"而不是"少一条记录"】少一条记录会被**两棵树各自的**
// 结构自检先抓住（"页里条目数 != 元数据计数"），那样就证明不了 I8 交叉校验本身在
// 干活。把 key 7 删掉再以**不同的 MBR** 插回去，两棵树各自都完全自洽
// （条目数不变、MBR 并集不变），只有"两棵树说的是不是同一条记录"这一层能发现它。
TEST(HybridDiff, I_TheI8CheckIsFalsifiable) {
  Env env("falsify");
  std::mt19937_64 rng(31);
  Truth truth;
  for (uint64_t k = 1; k <= 60; ++k) {
    IndexEntry e = MakeEntry(k, RandomMbr(&rng));
    ASSERT_TRUE(env.index->Put(e).ok());
    truth[k] = e;
  }
  ASSERT_TRUE(env.index->Validate().ok());

  // 绑一棵独立的 R 树到同一个槽位（同一份数据、同一棵结构），把 key 7 换成另一个 MBR
  RTreePageOps ops(kDim, RSplitPolicy::kQuadratic, 32);
  RTree side;
  TreeBuildOptions topt;
  topt.order_or_capacity = 32;
  topt.mbr_dim = kDim;
  topt.split_policy = "quadratic";
  topt.caller_tree_slot = 1;  // 与 HybridIndex 的 rtree_slot 一致
  ASSERT_TRUE(side.Open(env.index->pool(), &ops, topt, /*tree_id=*/2).ok());
  auto located = side.Locate(7, kInvalidPageId);
  ASSERT_TRUE(located.ok());
  ASSERT_TRUE(side.Erase(located.value()).ok());
  IndexEntry moved = located.value();
  for (uint16_t d = 0; d < kDim; ++d) {
    moved.mbr.lo[d] += 100.0;  // 同一条键，另一个空间位置
    moved.mbr.hi[d] += 100.0;
  }
  ASSERT_TRUE(side.Insert(moved).ok());

  // 两棵树各自的结构自检都该是绿的（不一致只在"两棵树之间"）
  ASSERT_TRUE(side.Validate().ok());
  // I8 交叉校验必须报，而且要说清楚是"两棵树对同一条记录说法不同"
  const Status v = env.index->Validate();
  EXPECT_FALSE(v.ok());
  EXPECT_TRUE(v.code() == StatusCode::kCorruption);
  const std::string msg = v.message();
  EXPECT_TRUE(msg.find("I8") != std::string::npos);
  EXPECT_TRUE(msg.find("7") != std::string::npos);
  std::printf("  I8 falsification fired as designed: %s\n", msg.c_str());
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

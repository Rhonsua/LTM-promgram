// tests/index/test_rtree_diff.cc
// R 树随机差分测试 + 回归钉（方案 §5.1 的 M3 必做项）。
//
// 与 test_bplus_diff.cc 同一个套路：用真值表对同一组随机操作序列在两边各做一遍，
// 逐步比对全量遍历 / 点查 / 空间查询 / KNN。差别在于多维树有两条**独立**的
// "看到全部记录"的通路 —— 叶链（ScanAll）与从根下钻（全空间查询）——
// 它们必须永远一致；只走其一的话，叶链断裂这类错误完全看不见。
//
// 最后三个 TEST 是针对本次实现中真实踩到的三个缺陷钉下的回归钉，
// 每个都对应一次"不变量全绿但答案错"的故障，见各自的注释。
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "index/rtree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {

constexpr double kEps = 1e-9;

struct Fixture {
  std::string page_file;
  std::string wal_file;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<WalWriter> wal;
  std::unique_ptr<BufferPool> pool;
  RTreePageOps ops;
  RTree tree;
  uint16_t dim;

  explicit Fixture(const std::string& tag, uint16_t dim_in = 2, uint32_t frames = 64,
                   uint16_t capacity = 16, RSplitPolicy policy = RSplitPolicy::kQuadratic)
      : page_file("log/_rtree_" + tag + ".data"), wal_file("log/_rtree_" + tag + ".wal"),
        ops(dim_in, policy, capacity), dim(dim_in) {
#if defined(_WIN32)
    std::system("if not exist log mkdir log");
#else
    std::system("mkdir -p log");
#endif
    std::remove(page_file.c_str());
    std::remove(wal_file.c_str());

    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    dopt.num_pages = 1 << 14;
    disk.reset(new DiskManager(dopt));
    WalOptions wopt;
    wal.reset(new WalWriter(wopt, disk.get()));
    BufferPoolOptions popt;
    popt.pool_frames = frames;
    pool.reset(new BufferPool(popt, disk.get(), wal.get(), nullptr, nullptr, nullptr));

    TreeBuildOptions topt;
    topt.order_or_capacity = capacity;
    topt.mbr_dim = dim_in;
    topt.split_policy = SplitPolicyName(policy);
    const Status s = tree.Open(pool.get(), &ops, topt, /*tree_id=*/1);
    if (!s.ok()) {
      std::printf("Fixture open failed: %s\n", s.ToString().c_str());
    }
  }

  ~Fixture() {
    pool.reset();
    disk.reset();
    std::remove(page_file.c_str());
    std::remove(wal_file.c_str());
  }
};

struct Truth {
  Mbr mbr;
  std::string payload;
};

Mbr MakeMbr(uint16_t dim, const std::vector<double>& c, const std::vector<double>& e) {
  Mbr m;
  m.lo.resize(dim);
  m.hi.resize(dim);
  for (uint16_t d = 0; d < dim; ++d) {
    m.lo[d] = c[d] - e[d];
    m.hi[d] = c[d] + e[d];
  }
  return m;
}

bool SameMbr(const Mbr& a, const Mbr& b) {
  if (a.lo.dim() != b.lo.dim()) {
    return false;
  }
  for (uint16_t d = 0; d < a.lo.dim(); ++d) {
    if (std::fabs(a.lo[d] - b.lo[d]) > kEps || std::fabs(a.hi[d] - b.hi[d]) > kEps) {
      return false;
    }
  }
  return true;
}

IndexEntry MakeEntry(uint64_t key, const Mbr& mbr, const std::string& payload) {
  IndexEntry e;
  e.key = key;
  e.mbr = mbr;
  e.payload.assign(payload.begin(), payload.end());
  return e;
}

std::string MakePayload(uint64_t key, size_t len) {
  std::string s;
  s.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    s.push_back(static_cast<char>('a' + ((key + i) % 26)));
  }
  return s;
}

// 真值表 <-> 树 的全量比对。返回不一致的说明；一致时返回空串。
std::string DiffScanAll(RTree& tree, const std::map<uint64_t, Truth>& truth) {
  std::map<uint64_t, Truth> got;
  const Status ss = tree.ScanAll([&got](const RecordRef& r) {
    got[r.key] = Truth{r.mbr, std::string(r.payload, r.payload_len)};
    return true;
  });
  if (!ss.ok()) {
    return "ScanAll failed: " + ss.ToString();
  }
  if (got.size() != truth.size()) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "the leaf chain holds %zu entries, the truth table %zu", got.size(), truth.size());
    return buf;
  }
  for (const auto& kv : truth) {
    auto g = got.find(kv.first);
    if (g == got.end()) {
      return "key=" + std::to_string(kv.first) + " is not on the leaf chain";
    }
    if (!SameMbr(g->second.mbr, kv.second.mbr)) {
      return "key=" + std::to_string(kv.first) + ": MBR differs";
    }
    if (g->second.payload != kv.second.payload) {
      return "key=" + std::to_string(kv.first) + ": payload differs";
    }
  }
  return "";
}

// 从根下钻枚举全部记录（全空间相交查询）。与 ScanAll 是两条独立通路。
std::string DiffDescent(RTree& tree, uint16_t dim, const std::map<uint64_t, Truth>& truth) {
  Query q;
  q.kind = QueryKind::kIntersect;
  q.mbr = MakeMbr(dim, std::vector<double>(dim, 0.0), std::vector<double>(dim, 1e18));
  std::map<uint64_t, int> seen;
  const Status s = tree.SpatialQuery(q, [&seen](const RecordRef& r) {
    ++seen[r.key];
    return true;
  });
  if (!s.ok()) {
    return "full-space query failed: " + s.ToString();
  }
  if (seen.size() != truth.size()) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "the root descent sees %zu entries, the truth table has %zu (leaf chain broken or descent missing branches)",
                  seen.size(), truth.size());
    return buf;
  }
  for (const auto& kv : seen) {
    if (kv.second != 1) {
      return "key=" + std::to_string(kv.first) + " reachable by root descent: " +
             std::to_string(kv.second) + " times (duplicate entry)";
    }
  }
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// 基础差分
// ---------------------------------------------------------------------------

TEST(RTreeDiff, BulkInsertThenScanMatchesTruth) {
  Fixture f("bulk");
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> coord(0.0, 1000.0);
  for (uint64_t k = 0; k < 400; ++k) {
    const std::vector<double> c{coord(rng), coord(rng)};
    const std::vector<double> e{1.0, 2.0};
    const Mbr m = MakeMbr(2, c, e);
    const std::string p = MakePayload(k, 8 + (k % 20));
    truth[k] = Truth{m, p};
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, m, p)).ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());
  EXPECT_EQ(DiffScanAll(f.tree, truth), "");
  EXPECT_EQ(DiffDescent(f.tree, 2, truth), "");
  std::printf("  [info] bulk load: %zu entries, pages %llu, splits %llu\n", truth.size(),
              static_cast<unsigned long long>(f.tree.stats().num_pages),
              static_cast<unsigned long long>(f.tree.stats().num_splits));
}

TEST(RTreeDiff, RandomInsertEraseMatchesTruth) {
  Fixture f("rand", /*dim=*/2, /*frames=*/24, /*capacity=*/16);
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(20240927);
  std::uniform_int_distribution<uint64_t> key_dist(0, 3000);
  std::uniform_real_distribution<double> coord(0.0, 1000.0);
  std::uniform_real_distribution<double> half(0.1, 5.0);

  for (int step = 0; step < 5000; ++step) {
    const uint64_t k = key_dist(rng);
    const bool do_insert = (rng() % 100) < 55 || truth.empty();
    if (do_insert) {
      const std::vector<double> c{coord(rng), coord(rng)};
      const std::vector<double> e{half(rng), half(rng)};
      const Mbr m = MakeMbr(2, c, e);
      const std::string p = MakePayload(k, 1 + (rng() % 30));
      truth[k] = Truth{m, p};
      const Status s = f.tree.Insert(MakeEntry(k, m, p));
      if (!s.ok()) {
        std::printf("  [err] Insert key=%llu failed: %s\n", static_cast<unsigned long long>(k),
                    s.ToString().c_str());
      }
      ASSERT_TRUE(s.ok());
    } else {
      auto it = truth.lower_bound(k);
      if (it == truth.end()) {
        it = truth.begin();
      }
      const uint64_t victim = it->first;
      const Mbr vm = it->second.mbr;
      truth.erase(it);
      const Status s = f.tree.Erase(MakeEntry(victim, vm, ""));
      if (!s.ok()) {
        std::printf("  [err] Erase key=%llu failed: %s\n", static_cast<unsigned long long>(victim),
                    s.ToString().c_str());
      }
      ASSERT_TRUE(s.ok());
    }

    if (step % 400 == 0) {
      const Status vs = f.tree.Validate();
      if (!vs.ok()) {
        std::printf("  [err] Validate failed at step %d: %s\n", step, vs.ToString().c_str());
      }
      ASSERT_TRUE(vs.ok());
    }
  }

  const Status vs = f.tree.Validate();
  if (!vs.ok()) {
    std::printf("  [err] Validate failed: %s\n", vs.ToString().c_str());
  }
  ASSERT_TRUE(vs.ok());

  const std::string d1 = DiffScanAll(f.tree, truth);
  EXPECT_EQ(d1, "");
  const std::string d2 = DiffDescent(f.tree, 2, truth);
  EXPECT_EQ(d2, "");

  // 点查逐条命中
  for (const auto& kv : truth) {
    auto r = f.tree.Search(MakeEntry(kv.first, kv.second.mbr, ""));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().key, kv.first);
    EXPECT_TRUE(SameMbr(r.value().mbr, kv.second.mbr));
    EXPECT_EQ(std::string(r.value().payload, r.value().payload_len), kv.second.payload);
  }

  const TreeStats st = f.tree.stats();
  std::printf("  [info] random insert/erase: final %zu entries, height %u, splits %llu, merges %llu, borrows %llu\n", truth.size(),
              st.height, static_cast<unsigned long long>(st.num_splits),
              static_cast<unsigned long long>(st.num_merges),
              static_cast<unsigned long long>(st.num_borrows));
}

TEST(RTreeDiff, SpatialQueryMatchesBruteForce) {
  Fixture f("spatial", /*dim=*/2, /*frames=*/16, /*capacity=*/12);
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(99);
  std::uniform_real_distribution<double> coord(0.0, 500.0);
  std::uniform_real_distribution<double> half(0.5, 8.0);
  for (uint64_t k = 0; k < 600; ++k) {
    const std::vector<double> c{coord(rng), coord(rng)};
    const std::vector<double> e{half(rng), half(rng)};
    const Mbr m = MakeMbr(2, c, e);
    truth[k] = Truth{m, MakePayload(k, 6)};
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, m, MakePayload(k, 6))).ok());
  }

  for (int round = 0; round < 20; ++round) {
    const std::vector<double> c{coord(rng), coord(rng)};
    // 一半相交窗口、一半包含窗口；包含窗口用大框保证非空
    const bool wide = (round % 2) == 1;
    const std::vector<double> e{wide ? 150.0 : 20.0, wide ? 150.0 : 20.0};
    const Mbr w = MakeMbr(2, c, e);

    std::set<uint64_t> exp_i;
    std::set<uint64_t> exp_c;
    for (const auto& kv : truth) {
      if (kv.second.mbr.Intersects(w)) {
        exp_i.insert(kv.first);
      }
      if (w.Contains(kv.second.mbr)) {
        exp_c.insert(kv.first);
      }
    }

    std::set<uint64_t> got_i;
    Query qi;
    qi.kind = QueryKind::kIntersect;
    qi.mbr = w;
    ASSERT_TRUE(f.tree.SpatialQuery(qi, [&got_i](const RecordRef& r) {
      got_i.insert(r.key);
      return true;
    }).ok());
    if (got_i != exp_i) {
      std::printf("  [err] round %d intersect query: expected %zu, got %zu\n", round, exp_i.size(),
                  got_i.size());
    }
    EXPECT_TRUE(got_i == exp_i);

    std::set<uint64_t> got_c;
    Query qc;
    qc.kind = QueryKind::kRangeSpatial;
    qc.mbr = w;
    ASSERT_TRUE(f.tree.SpatialQuery(qc, [&got_c](const RecordRef& r) {
      got_c.insert(r.key);
      return true;
    }).ok());
    if (got_c != exp_c) {
      std::printf("  [err] round %d contains query: expected %zu, got %zu\n", round, exp_c.size(),
                  got_c.size());
    }
    EXPECT_TRUE(got_c == exp_c);
  }
}

TEST(RTreeDiff, KnnMatchesBruteForceAndOracle) {
  Fixture f("knn", /*dim=*/2, /*frames=*/16, /*capacity=*/12);
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(1234);
  std::uniform_real_distribution<double> coord(0.0, 500.0);
  std::uniform_real_distribution<double> half(0.5, 6.0);
  for (uint64_t k = 0; k < 500; ++k) {
    const std::vector<double> c{coord(rng), coord(rng)};
    const std::vector<double> e{half(rng), half(rng)};
    const Mbr m = MakeMbr(2, c, e);
    truth[k] = Truth{m, MakePayload(k, 6)};
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, m, MakePayload(k, 6))).ok());
  }

  const uint32_t k = 10;
  for (int round = 0; round < 10; ++round) {
    Point probe;
    probe.resize(2);
    probe[0] = coord(rng);
    probe[1] = coord(rng);

    // 独立预言机：真值表全扫
    std::vector<double> oracle;
    oracle.reserve(truth.size());
    for (const auto& kv : truth) {
      oracle.push_back(kv.second.mbr.DistanceTo(probe));
    }
    std::sort(oracle.begin(), oracle.end());
    oracle.resize(k);

    Query q;
    q.kind = QueryKind::kKnn;
    q.probe = probe;
    q.k = k;

    std::vector<double> fast;
    ASSERT_TRUE(f.tree.Knn(q, [&fast](const RecordRef&, double d) {
      fast.push_back(d);
      return true;
    }).ok());
    ASSERT_EQ(fast.size(), oracle.size());
    for (size_t i = 0; i < fast.size(); ++i) {
      EXPECT_TRUE(std::fabs(fast[i] - oracle[i]) <= kEps);
    }

    std::vector<std::pair<uint64_t, double>> alt;
    ASSERT_TRUE(f.tree.KnnBruteForce(q, [&alt](const RecordRef& r, double d) {
      alt.emplace_back(r.key, d);
      return true;
    }).ok());
    ASSERT_EQ(alt.size(), fast.size());
    for (size_t i = 0; i < alt.size(); ++i) {
      EXPECT_TRUE(std::fabs(alt[i].second - fast[i]) <= kEps);
    }
  }
}

TEST(RTreeDiff, KnnTopKBreaksLeafTiesByKey) {
  Fixture f("knn_topk_leaf", 2, 12, 16);
  std::vector<KeyId> keys{0, 1, 2, 3, 4, 5, 6,
                        std::numeric_limits<KeyId>::max() - 1,
                        std::numeric_limits<KeyId>::max()};
  const Mbr box = MakeMbr(2, {1, 0}, {0, 0});
  // No splits: slot order is deliberately the reverse of the required tie order.
  for (auto it = keys.rbegin(); it != keys.rend(); ++it)
    ASSERT_TRUE(f.tree.Insert(MakeEntry(*it, box, MakePayload(*it, 6))).ok());
  for (uint32_t k : {0u, 1u, 3u, 8u, 9u, 20u}) {
    Query q; q.kind = QueryKind::kKnn; q.probe = Point(2); q.k = k;
    std::vector<KeyId> got;
    ASSERT_TRUE(f.tree.Knn(q, [&](const RecordRef& ref, double distance) {
      got.push_back(ref.key);
      EXPECT_EQ(distance, 1.0);
      EXPECT_TRUE(SameMbr(ref.mbr, box));
      EXPECT_EQ(std::string(ref.payload, ref.payload_len), MakePayload(ref.key, 6));
      return true;
    }).ok());
    const size_t limit = std::min<size_t>(k == 0 ? 1u : k, keys.size());
    EXPECT_TRUE(got == std::vector<KeyId>(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(limit)));
  }
  ASSERT_TRUE(f.tree.Validate().ok());
}

TEST(RTreeDiff, KnnTopKMatchesCompleteOracleAcrossSplitPolicies) {
  for (RSplitPolicy policy : {RSplitPolicy::kEven, RSplitPolicy::kLinear, RSplitPolicy::kQuadratic}) {
    Fixture f(std::string("knn_topk_") + SplitPolicyName(policy), 3, 12, 8, policy);
    std::map<KeyId, Truth> truth;
    for (KeyId key = 96; key-- > 0;) {
      std::vector<double> center(3, 0.0);
      center[key % 3] = (key % 2 == 0 ? 1.0 : -1.0) * (static_cast<double>(key % 4) + 0.25);
      const Mbr box = MakeMbr(3, center, {0.25, 0.25, 0.25});
      const std::string payload = MakePayload(key, static_cast<size_t>(6 + key % 9));
      truth[key] = Truth{box, payload};
      ASSERT_TRUE(f.tree.Insert(MakeEntry(key, box, payload)).ok());
    }
    ASSERT_TRUE(f.tree.Validate().ok());
    EXPECT_TRUE(f.tree.stats().num_pages > 12u);
    // Exercise both a full tree and a tree changed by deletion/rebalancing.
    for (int stage = 0; stage < 2; ++stage) {
      for (double offset : {0.0, 0.5}) {
        Query q; q.kind = QueryKind::kKnn; q.probe = Point(3);
        q.probe[0] = offset;
        std::vector<std::pair<double, KeyId>> oracle;
        for (const auto& item : truth) {
          double squared = 0.0;
          for (uint16_t d = 0; d < 3; ++d) {
            const double coordinate = q.probe[d];
            const double delta = std::max({item.second.mbr.lo[d] - coordinate,
                                           coordinate - item.second.mbr.hi[d], 0.0});
            squared += delta * delta;
          }
          oracle.emplace_back(std::sqrt(squared), item.first);
        }
        std::sort(oracle.begin(), oracle.end());
        for (uint32_t k : {0u, 1u, 2u, 7u, 23u, 24u, 25u, 65u, 95u, 96u, 1000u}) {
          q.k = k;
          const size_t limit = std::min<size_t>(k == 0 ? 1u : k, truth.size());
          const std::vector<std::pair<double, KeyId>> expected(oracle.begin(), oracle.begin() +
                                                             static_cast<std::ptrdiff_t>(limit));
          for (bool brute : {false, true}) {
            std::vector<std::pair<double, KeyId>> got;
            const auto emit = [&](const RecordRef& ref, double distance) {
              got.emplace_back(distance, ref.key);
              const auto it = truth.find(ref.key);
              EXPECT_TRUE(it != truth.end());
              if (it != truth.end()) {
                EXPECT_TRUE(SameMbr(ref.mbr, it->second.mbr));
                EXPECT_EQ(std::string(ref.payload, ref.payload_len), it->second.payload);
              }
              return true;
            };
            ASSERT_TRUE((brute ? f.tree.KnnBruteForce(q, emit) : f.tree.Knn(q, emit)).ok());
            EXPECT_TRUE(got == expected);
          }
        }
      }
      if (stage == 0) {
        for (KeyId key = 5; key < 96; key += 7) {
          const auto entry = truth.at(key);
          ASSERT_TRUE(f.tree.Erase(MakeEntry(key, entry.mbr, entry.payload)).ok());
          truth.erase(key);
        }
      }
      ASSERT_TRUE(f.tree.Validate().ok());
    }
  }
}

TEST(RTreeDiff, EraseDownToEmptyThenReinsert) {
  Fixture f("shrink", /*dim=*/2, /*frames=*/16, /*capacity=*/8);
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(5);
  std::uniform_real_distribution<double> coord(0.0, 100.0);
  for (uint64_t k = 0; k < 300; ++k) {
    const std::vector<double> c{coord(rng), coord(rng)};
    const std::vector<double> e{1.5, 1.5};
    const Mbr m = MakeMbr(2, c, e);
    truth[k] = Truth{m, MakePayload(k, 6)};
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, m, MakePayload(k, 6))).ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());

  // 逆序删空，逼出借键 / 合并 / 根降级
  for (uint64_t k = 300; k-- > 0;) {
    const Mbr m = truth[k].mbr;
    truth.erase(k);
    const Status s = f.tree.Erase(MakeEntry(k, m, ""));
    ASSERT_TRUE(s.ok());
    if (k % 40 == 0) {
      ASSERT_TRUE(f.tree.Validate().ok());
    }
  }
  EXPECT_TRUE(truth.empty());
  ASSERT_TRUE(f.tree.Validate().ok());

  uint64_t left = 0;
  ASSERT_TRUE(f.tree.ScanAll([&left](const RecordRef&) {
    ++left;
    return true;
  }).ok());
  EXPECT_EQ(left, 0u);
  EXPECT_EQ(f.tree.stats().height, 0u);  // 应当降级回空叶根

  // 空树再插回去：真值表同步重建，两条通路都要与它一致
  std::map<uint64_t, Truth> again;
  for (uint64_t k = 0; k < 120; ++k) {
    const std::vector<double> c{coord(rng), coord(rng)};
    const Mbr m = MakeMbr(2, c, std::vector<double>(2, 1.0));
    const std::string p = MakePayload(k, 5);
    again[k] = Truth{m, p};
    const Status s = f.tree.Insert(MakeEntry(k, m, p));
    ASSERT_TRUE(s.ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());
  EXPECT_EQ(DiffScanAll(f.tree, again), "");
  EXPECT_EQ(DiffDescent(f.tree, 2, again), "");
  uint64_t count = 0;
  ASSERT_TRUE(f.tree.ScanAll([&count](const RecordRef&) {
    ++count;
    return true;
  }).ok());
  EXPECT_EQ(count, 120u);
}

TEST(RTreeDiff, SurvivesReopenWithSmallBufferPool) {
  const std::string tag = "reopen";
  const std::string page_file = "log/_rtree_" + tag + ".data";
  const std::string wal_file = "log/_rtree_" + tag + ".wal";
#if defined(_WIN32)
  std::system("if not exist log mkdir log");
#else
  std::system("mkdir -p log");
#endif
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());

  const uint64_t kN = 600;
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(31337);
  std::uniform_real_distribution<double> coord(0.0, 400.0);
  for (uint64_t k = 0; k < kN; ++k) {
    const std::vector<double> c{coord(rng), coord(rng)};
    const std::vector<double> e{1.0, 1.0};
    truth[k] = Truth{MakeMbr(2, c, e), MakePayload(k, 9)};
  }

  {
    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    dopt.num_pages = 1 << 13;
    DiskManager disk(dopt);
    WalOptions wopt;
    WalWriter wal(wopt, &disk);
    BufferPoolOptions popt;
    popt.pool_frames = 6;  // 故意极小：必须靠淘汰/回读保证正确性
    BufferPool pool(popt, &disk, &wal, nullptr, nullptr, nullptr);
    RTreePageOps ops(2, RSplitPolicy::kQuadratic, 12);
    RTree tree;
    TreeBuildOptions topt;
    topt.order_or_capacity = 12;
    topt.mbr_dim = 2;
    topt.split_policy = SplitPolicyName(RSplitPolicy::kQuadratic);
    ASSERT_TRUE(tree.Open(&pool, &ops, topt, 1).ok());
    for (uint64_t k = 0; k < kN; ++k) {
      ASSERT_TRUE(tree.Insert(MakeEntry(k, truth[k].mbr, truth[k].payload)).ok());
    }
    ASSERT_TRUE(tree.Validate().ok());
    ASSERT_TRUE(pool.FlushAll().ok());
    std::printf("  [info] small pool: hit rate %.3f, evictions %llu, dirty evictions %llu\n", pool.hit_rate(),
                static_cast<unsigned long long>(pool.stats().evictions),
                static_cast<unsigned long long>(pool.stats().dirty_evictions));
  }

  // 重开：元数据必须从元数据页读回（缺陷 #26 的回归钉），数据必须完整
  {
    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    dopt.num_pages = 1 << 13;
    DiskManager disk(dopt);
    WalOptions wopt;
    WalWriter wal(wopt, &disk);
    BufferPoolOptions popt;
    popt.pool_frames = 6;
    BufferPool pool(popt, &disk, &wal, nullptr, nullptr, nullptr);
    RTreePageOps ops(2, RSplitPolicy::kQuadratic, 12);
    RTree tree;
    TreeBuildOptions topt;
    topt.order_or_capacity = 12;
    topt.mbr_dim = 2;
    topt.split_policy = SplitPolicyName(RSplitPolicy::kQuadratic);
    ASSERT_TRUE(tree.Open(&pool, &ops, topt, 1).ok());
    ASSERT_TRUE(tree.Validate().ok());
    EXPECT_EQ(DiffScanAll(tree, truth), "");
    EXPECT_EQ(DiffDescent(tree, 2, truth), "");
    std::printf("  [info] after reopen: height %u, pages %llu\n", tree.stats().height,
                static_cast<unsigned long long>(tree.stats().num_pages));
  }
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());
}

// ---------------------------------------------------------------------------
// 回归钉
// ---------------------------------------------------------------------------

// 缺陷 A：同主键、不同 MBR 的插入会在**另一个叶页**里再放一份（R 树的插入去向
// 由 MBR 决定、与 key 无关），于是 Search 命中一份、ScanAll 看到两份，
// 而随后的 Erase 只删掉一份 —— 另一份永久残留。
// 症状：不变量全绿，只有"树内条目数"比真值表大（实测 934 vs 844）。
TEST(RTreeRegression, SameKeyDifferentMbrIsUpsert) {
  Fixture f("upsert", /*dim=*/2, /*frames=*/8, /*capacity=*/8);

  // 关键在于"新 MBR 会被路由到**另一个叶页**"：只有两个条目时它们同处一个叶页，
  // 叶内去重就够用了，缺陷根本露不出来（这条回归钉第一版就是这么写的，白写）。
  // 所以先把 40 条挤在左下角撑出多个叶页，再用右下角的 MBR 逐键覆盖 ——
  // 按最小面积增量路由，新条目必然落到另一侧的叶页里。
  const uint32_t kN = 40;
  auto corner_mbr = [](double base, double jitter) {
    const std::vector<double> c{base + jitter, base + jitter};
    return MakeMbr(2, c, std::vector<double>(2, 1.0));
  };

  std::map<uint64_t, Truth> truth;
  for (uint64_t k = 0; k < kN; ++k) {
    const Mbr m = corner_mbr(1.0, static_cast<double>(k % 5));
    const std::string p = "old-" + std::to_string(k);
    truth[k] = Truth{m, p};
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, m, p)).ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());
  // num_pages 由 Validate 的遍历顺带算出（stats() 自己不做遍历），故上面必须先 Validate
  ASSERT_TRUE(f.tree.stats().num_pages >= 2u);  // 至少两个页，否则测不到跨页路由

  // 用对角方向的 MBR 覆盖同一批键
  for (uint64_t k = 0; k < kN; ++k) {
    const Mbr m = corner_mbr(900.0, static_cast<double>(k % 5));
    const std::string p = "new-" + std::to_string(k);
    truth[k] = Truth{m, p};
    const Status s = f.tree.Insert(MakeEntry(k, m, p));
    ASSERT_TRUE(s.ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());

  // 每个键都必须只剩一份，且是新的 MBR / 新的载荷
  EXPECT_EQ(DiffScanAll(f.tree, truth), "");
  EXPECT_EQ(DiffDescent(f.tree, 2, truth), "");
  uint64_t count = 0;
  ASSERT_TRUE(f.tree.ScanAll([&count](const RecordRef&) {
    ++count;
    return true;
  }).ok());
  EXPECT_EQ(count, static_cast<uint64_t>(kN));  // 覆盖而不是新增（缺陷下会是 2*kN）

  // 旧 MBR 必须彻底消失：拿它精确删除应当报"不存在"
  const Status stale = f.tree.Erase(MakeEntry(3, corner_mbr(1.0, 3.0), ""));
  EXPECT_TRUE(!stale.ok());
}

// 缺陷 B：合并时的摘链照搬了"父页相邻槽即叶链相邻"，但多维树不成立 ——
// 内部页借键会把一个子页搬到另一棵父页的槽位里，它的叶链位置却不动。
// 于是 `left->next = right->next` 会把两者之间的整段叶页从链上摘掉，
// 而那段页的末端仍指向已释放的 right。症状：ScanAll（叶链）突然少掉几百条，
// 但从根下钻一切正常、Validate 也全绿（I9 与链序无关）。
// 这条回归钉用"必须发生大量合并"的负载，然后同时走两条通路比对。
TEST(RTreeRegression, MergeKeepsLeafChainConsistent) {
  Fixture f("chain", /*dim=*/2, /*frames=*/16, /*capacity=*/8);
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(4242);
  std::uniform_real_distribution<double> coord(0.0, 1000.0);

  // 先把树撑到多层
  for (uint64_t k = 0; k < 400; ++k) {
    const std::vector<double> c{coord(rng), coord(rng)};
    const Mbr m = MakeMbr(2, c, {1.0, 1.0});
    const std::string p = MakePayload(k, 6);
    truth[k] = Truth{m, p};
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, m, p)).ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());
  EXPECT_EQ(DiffDescent(f.tree, 2, truth), "");

  // 集中删除约 80%：小容量下必然触发合并与根降级
  for (uint64_t k = 0; k < 320; ++k) {
    const Mbr m = truth[k].mbr;
    truth.erase(k);
    ASSERT_TRUE(f.tree.Erase(MakeEntry(k, m, "")).ok());
    if (k % 20 == 0) {
      const std::string d1 = DiffScanAll(f.tree, truth);
      const std::string d2 = DiffDescent(f.tree, 2, truth);
      if (d1 != "" || d2 != "") {
        std::printf("  [err] at erase %llu the two paths diverge: leaf chain [%s] vs descent [%s]\n",
                    static_cast<unsigned long long>(k), d1.c_str(), d2.c_str());
      }
      ASSERT_EQ(d1, "");
      ASSERT_EQ(d2, "");
    }
  }
  ASSERT_TRUE(f.tree.Validate().ok());
  EXPECT_EQ(DiffScanAll(f.tree, truth), "");
  EXPECT_EQ(DiffDescent(f.tree, 2, truth), "");

  const TreeStats st = f.tree.stats();
  EXPECT_GE(st.num_merges, 1u);  // 没发生合并就等于没测到这条路径
  std::printf("  [info] merge path: splits %llu, merges %llu, borrows %llu, final %zu entries\n",
              static_cast<unsigned long long>(st.num_splits),
              static_cast<unsigned long long>(st.num_merges),
              static_cast<unsigned long long>(st.num_borrows), truth.size());
}

// 缺陷 C：删除在内部页遇到"死胡同"候选（MBR 重叠产生的假阳性）时返回 kNotFound，
// 调用方用 LTM_RETURN_IF_ERROR 直接把它当成终局错误上报 —— 于是先探到的那条路
// 走到底没找到，整次删除就被误报成"记录不存在"，真正装着记录的兄弟候选根本没被访问。
// 症状：Validate 全绿、ScanAll 与真值表逐条一致、计数也对，只有 Erase 说不存在。
TEST(RTreeRegression, EraseTriesAllOverlappingCandidates) {
  Fixture f("cand", /*dim=*/2, /*frames=*/16, /*capacity=*/8);
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(777);
  // 大 MBR 互相重叠 → 制造大量假阳性候选子树
  std::uniform_real_distribution<double> coord(0.0, 100.0);
  std::uniform_real_distribution<double> half(8.0, 20.0);

  for (uint64_t k = 0; k < 400; ++k) {
    const std::vector<double> c{coord(rng), coord(rng)};
    const std::vector<double> e{half(rng), half(rng)};
    const Mbr m = MakeMbr(2, c, e);
    const std::string p = MakePayload(k, 6);
    truth[k] = Truth{m, p};
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, m, p)).ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());

  // 逐条删除：每一条都必须能删掉，且删完的两条通路都要与真值表一致
  for (uint64_t k = 0; k < 400; ++k) {
    const Mbr m = truth[k].mbr;
    truth.erase(k);
    const Status s = f.tree.Erase(MakeEntry(k, m, ""));
    if (!s.ok()) {
      std::printf("  [err] erase key=%llu failed: %s\n", static_cast<unsigned long long>(k),
                  s.ToString().c_str());
    }
    ASSERT_TRUE(s.ok());
    if (k % 50 == 0) {
      ASSERT_TRUE(f.tree.Validate().ok());
      EXPECT_EQ(DiffScanAll(f.tree, truth), "");
      EXPECT_EQ(DiffDescent(f.tree, 2, truth), "");
    }
  }
  ASSERT_TRUE(f.tree.Validate().ok());
  EXPECT_EQ(DiffScanAll(f.tree, truth), "");
}

TEST(RTreeDiff, KnnStatsSnapshotsAreStableWithParallelBruteForceAndReaders) {
  Fixture f("knn_stats_parallel", 2, 12, 8);
  constexpr KeyId count = 96;
  for (KeyId key = 0; key < count; ++key) {
    const double pos = static_cast<double>(key);
    ASSERT_TRUE(f.tree.Insert(MakeEntry(key, MakeMbr(2, {pos, pos}, {0, 0}),
                                      MakePayload(key, 6))).ok());
  }
  Query q; q.kind = QueryKind::kKnn; q.k = 1; q.probe = Point(2);
  q.probe[0] = q.probe[1] = 7;
  ASSERT_TRUE(f.tree.Knn(q, [](const RecordRef&, double) { return true; }).ok());
  const auto& snapshot = f.tree.knn_stats();
  const auto saved = snapshot;
  ASSERT_TRUE(saved.pages_visited > 0u);
  q.k = 200;
  ASSERT_TRUE(f.tree.Knn(q, [](const RecordRef&, double) { return true; }).ok());
  EXPECT_EQ(snapshot.pages_visited, saved.pages_visited);
  EXPECT_EQ(snapshot.entries_scanned, saved.entries_scanned);
  EXPECT_EQ(f.tree.knn_stats().entries_scanned, count);

  // Snapshot pairs must be whole published results, never a partial traversal.
  std::set<std::pair<uint64_t, uint64_t>> valid;
  for (uint32_t k : {1u, 200u}) {
    q.k = k;
    ASSERT_TRUE(f.tree.Knn(q, [](const RecordRef&, double) { return true; }).ok());
    const auto s = f.tree.knn_stats();
    valid.emplace(s.pages_visited, s.entries_scanned);
  }
  std::atomic<bool> go{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int role = 0; role < 3; ++role) threads.emplace_back([&, role] {
    while (!go.load()) std::this_thread::yield();
    for (int round = 0; round < 32; ++round) {
      Query query = q; query.k = role == 0 ? 1u : 200u;
      std::vector<KeyId> keys;
      const auto emit = [&](const RecordRef& ref, double) {
        keys.push_back(ref.key);
        if (std::string(ref.payload, ref.payload_len) != MakePayload(ref.key, 6)) ++errors;
        return true;
      };
      const auto status = role == 2 ? f.tree.KnnBruteForce(query, emit) : f.tree.Knn(query, emit);
      if (!status.ok() || keys.size() != (role == 0 ? 1u : static_cast<size_t>(count)) ||
          keys.empty() || keys[0] != 7) ++errors;
    }
  });
  threads.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 4000; ++i) {
      const auto s = f.tree.knn_stats();
      if (valid.count({s.pages_visited, s.entries_scanned}) == 0) ++errors;
    }
  });
  go.store(true);
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(snapshot.pages_visited, saved.pages_visited);
  EXPECT_EQ(snapshot.entries_scanned, saved.entries_scanned);
  EXPECT_TRUE(f.tree.Validate().ok());
}

int main(int argc, char** argv) {
  Logger::Instance().SetLevel(LogLevel::kError);
  return ltm::test::RunAll(argc, argv);
}

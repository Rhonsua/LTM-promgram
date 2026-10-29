#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "index/hybrid_index.h"
#include "migration/hot_cold_tracker.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {
IndexEntry Make(KeyId key, int version = 0) {
  IndexEntry e;
  e.key = key;
  e.mbr = Mbr::Invalid(2);
  const double pos = static_cast<double>(key) + (key >= 10000 ? 20000.0 + version * 0.01 : 0.0);
  for (uint16_t d = 0; d < 2; ++d) e.mbr.lo[d] = e.mbr.hi[d] = pos;
  const std::string text = std::to_string(key) + ":" + std::to_string(version);
  e.payload.assign(text.begin(), text.end());
  return e;
}

bool Same(const IndexEntry& a, const IndexEntry& b) {
  return a.key == b.key && a.payload == b.payload &&
         a.mbr.lo.coords() == b.mbr.lo.coords() && a.mbr.hi.coords() == b.mbr.hi.coords();
}

bool CommittedShape(const IndexEntry& e, KeyId key, int rounds) {
  try {
    const std::string text(e.payload.begin(), e.payload.end());
    const auto colon = text.find(':');
    if (colon == std::string::npos) return false;
    const int version = std::stoi(text.substr(colon + 1));
    if (e.key != key || version < 0 || version > rounds) return false;
    if (version > 0 && ((version - 1) % 64 != static_cast<int>((key - 10000) % 64) ||
                        (version - 1) % 5 == 0)) return false;
    return Same(e, Make(key, version));
  } catch (...) { return false; }
}

struct Env {
  std::string data, wal;
  std::unique_ptr<HybridIndex> owned;
  HybridIndex& index;
  HybridOptions options;
  explicit Env(const std::string& name)
      : data("log/_hybrid_concurrent_" + name + ".data"),
        wal("log/_hybrid_concurrent_" + name + ".wal"),
        owned(new HybridIndex()), index(*owned) {
    std::filesystem::create_directories("log");
    std::remove(data.c_str()); std::remove(wal.c_str());
    options.config.disk.page_file = data;
    options.config.disk.wal_file = wal;
    options.config.disk.num_pages = 2048;
    options.config.pool.pool_frames = 12;
    options.bplus.order_or_capacity = 8;
    options.rtree.order_or_capacity = 8;
    options.rtree.mbr_dim = 2;
    ASSERT_TRUE(index.Open(options).ok());
  }
  ~Env() { index.Close(); owned.reset(); std::remove(data.c_str()); std::remove(wal.c_str()); }
};
}  // namespace

TEST(HeatConcurrency, TouchCountsAreExactAcrossRehashAndDecay) {
  HotColdTracker heat(MigrationPolicyOptions{});
  constexpr int kThreads = 6, kRounds = 2000;
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) threads.emplace_back([&, t] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < kRounds; ++i) {
      const PageId p = static_cast<PageId>(t * kRounds + i);
      heat.Touch(p, i % 2 == 0);
      heat.Touch(7, false);  // all threads contend on the same heat counter
      heat.SetLevel(p, static_cast<uint8_t>(i % 3));
      heat.SetInPool(p, true);
      if (i % 3 == 0) heat.MarkOutOfPool(p);
      if (i % 5 == 0 && p != 7) heat.ForgetPage(p);
      LTM_UNUSED(heat.Heat(p));
      LTM_UNUSED(heat.ref_bit(p));
      LTM_UNUSED(heat.level(p));
      if (i % 100 == 0) { LTM_UNUSED(heat.stats()); LTM_UNUSED(heat.ColdestInPool(8)); }
    }
  });
  threads.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 80; ++i) heat.DecayAll();
  });
  go.store(true);
  for (auto& thread : threads) thread.join();
  const auto before = heat.stats();
  EXPECT_EQ(before.touches, static_cast<uint64_t>(kThreads * kRounds * 2));
  EXPECT_EQ(before.prefetch_touches, static_cast<uint64_t>(kThreads * kRounds / 2));
  EXPECT_EQ(before.decays, 80u);
  EXPECT_EQ(before.tracked_pages, heat.tracked_pages());
  EXPECT_LE(before.cold_pages, before.tracked_pages);
  heat.Touch(7, false);
  EXPECT_EQ(before.touches + 1, heat.stats().touches); // the old snapshot stays unchanged
  heat.ResetStats();
  EXPECT_EQ(heat.stats().touches, 0u);
}

TEST(HybridConcurrency, ReadersOverlapAndCallbacksCanReadButCannotUpgrade) {
  Env env("callbacks");
  for (KeyId key = 0; key < 96; ++key) ASSERT_TRUE(env.index.Put(Make(key)).ok());
  ASSERT_TRUE(env.index.Validate().ok());
  std::mutex mutex;
  std::condition_variable cv;
  int active = 0, peak = 0;
  bool release = false;
  std::atomic<int> errors{0};
  Query point; point.kind = QueryKind::kPoint; point.key_lo = 7;
  auto reader = [&] {
    const auto status = env.index.Query(point, [&](const IndexEntry&) {
      auto nested = env.index.Get(8);
      if (!nested.ok() || !Same(nested.value(), Make(8))) ++errors;
      if (!env.index.is_open() || env.index.stats().heat.touches == 0) ++errors;
      if (env.index.Put(Make(8, 1)).code() != StatusCode::kInvalidArgument) ++errors;
      if (env.index.Checkpoint().code() != StatusCode::kInvalidArgument) ++errors;
      if (env.index.Validate().code() != StatusCode::kInvalidArgument) ++errors;
      if (env.index.Close().code() != StatusCode::kInvalidArgument) ++errors;
      std::unique_lock<std::mutex> lock(mutex);
      ++active; peak = std::max(peak, active); cv.notify_all();
      if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return release; })) ++errors;
      --active;
      return true;
    });
    if (!status.ok()) ++errors;
  };
  std::thread a(reader), b(reader);
  {
    std::unique_lock<std::mutex> lock(mutex);
    const bool overlapped = cv.wait_for(lock, std::chrono::seconds(5), [&] { return active == 2; });
    EXPECT_TRUE(overlapped);
    release = true; cv.notify_all();
  }
  a.join(); b.join();
  EXPECT_EQ(peak, 2);
  EXPECT_EQ(errors.load(), 0);

  // Scan callbacks reenter Get/stats after page guards are released, even with
  // internal roots. Early stop must still deliver exactly one result.
  for (QueryKind kind : {QueryKind::kRange1D, QueryKind::kIntersect, QueryKind::kKnn}) {
    Query q; q.kind = kind; q.key_lo = 0; q.key_hi = 95; q.k = 5;
    q.mbr = Mbr::Invalid(2); q.probe = Point(2);
    for (uint16_t d = 0; d < 2; ++d) { q.mbr.lo[d] = 0; q.mbr.hi[d] = 100; q.probe[d] = 7; }
    int seen = 0;
    const auto s = env.index.Query(q, [&](const IndexEntry& entry) {
      auto nested = env.index.Get(entry.key);
      if (!nested.ok() || !Same(nested.value(), entry)) ++errors;
      LTM_UNUSED(env.index.stats());
      ++seen;
      return false;
    });
    EXPECT_TRUE(s.ok()); EXPECT_EQ(seen, 1);
  }
  EXPECT_EQ(errors.load(), 0);
  EXPECT_TRUE(env.index.Validate().ok());
  EXPECT_EQ(env.index.pool()->latch_stats().order_violations.load(), 0u);
}

TEST(HybridConcurrency, HeatEnabledMixedApisMatchCommittedRecords) {
  Logger::Instance().SetLevel(LogLevel::kError);
  Env env("mixed");
  constexpr KeyId kImmutable = 192;
  constexpr int kRounds = 96;
  std::map<KeyId, IndexEntry> expected;
  for (KeyId key = 0; key < kImmutable; ++key) expected[key] = Make(key);
  for (KeyId key = 10000; key < 10128; ++key) expected[key] = Make(key);
  for (const auto& kv : expected) ASSERT_TRUE(env.index.Put(kv.second).ok());
  ASSERT_TRUE(env.index.Validate().ok());
  const auto baseline = env.index.stats();
  ASSERT_TRUE(baseline.bplus.num_pages + baseline.rtree.num_pages > 12u);
  for (int t = 0; t < 2; ++t)
    for (int i = 0; i < kRounds; ++i) {
      const KeyId key = 10000 + static_cast<KeyId>(t) * 64 + static_cast<KeyId>(i % 64);
      if (i % 5 == 0) expected.erase(key); else expected[key] = Make(key, i + 1);
    }
  std::atomic<bool> go{false};
  std::atomic<uint64_t> errors{0}, checkpoints{0}, validations{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 2; ++t) threads.emplace_back([&, t] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < kRounds; ++i) {
      const KeyId key = 10000 + static_cast<KeyId>(t) * 64 + static_cast<KeyId>(i % 64);
      const auto s = i % 5 == 0 ? env.index.Erase(key) : env.index.Put(Make(key, i + 1));
      if (!s.ok() && !(i % 5 == 0 && s.code() == StatusCode::kNotFound)) ++errors;
    }
  });
  for (int role = 0; role < 4; ++role) threads.emplace_back([&, role] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 80; ++i) {
      if (role == 0) {
        const KeyId key = static_cast<KeyId>(i) % kImmutable;
        auto r = env.index.Get(key);
        if (!r.ok() || !Same(r.value(), Make(key))) ++errors;
        const KeyId mutable_key = 10000 + static_cast<KeyId>(i) % 64;
        auto mutable_row = env.index.Get(mutable_key);
        if (mutable_row.ok()) {
          if (!CommittedShape(mutable_row.value(), mutable_key, kRounds)) ++errors;
        }
        else if (mutable_row.status().code() != StatusCode::kNotFound) ++errors;
      } else if (role == 1 || role == 2) {
        Query q; q.kind = role == 1 ? QueryKind::kRange1D : QueryKind::kIntersect;
        q.key_lo = 0; q.key_hi = kImmutable - 1; q.mbr = Mbr::Invalid(2);
        for (uint16_t d = 0; d < 2; ++d) { q.mbr.lo[d] = 0; q.mbr.hi[d] = kImmutable; }
        std::map<KeyId, IndexEntry> rows;
        const auto s = env.index.Query(q, [&](const IndexEntry& row) {
          if (row.key >= kImmutable || !Same(row, Make(row.key)) ||
              !rows.emplace(row.key, row).second) ++errors;
          return true;
        });
        if (!s.ok() || rows.size() != kImmutable) ++errors;
      } else {
        Query q; q.kind = QueryKind::kKnn; q.probe = Point(2); q.probe[0] = q.probe[1] = 42;
        std::vector<IndexEntry> rows;
        if (!env.index.Knn(q, 5, &rows).ok() || rows.size() != 5) { ++errors; continue; }
        const KeyId keys[] = {42, 41, 43, 40, 44};
        for (size_t j = 0; j < 5; ++j) if (!Same(rows[j], Make(keys[j]))) ++errors;
      }
      const auto s = env.index.stats();
      if (s.pool.i6_violations != 0 || s.heat.touches < baseline.heat.touches) ++errors;
    }
  });
  // A second KNN reader exercises the mutable R-tree per-call counters.
  threads.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 80; ++i) {
      Query q; q.probe = Point(2); q.probe[0] = q.probe[1] = 42;
      std::vector<IndexEntry> rows;
      if (!env.index.Knn(q, 1, &rows).ok() || rows.size() != 1 || !Same(rows[0], Make(42))) ++errors;
    }
  });
  threads.emplace_back([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 24; ++i) {
      if (!env.index.DecayHeat().ok()) ++errors;
      if (i % 6 == 0) {
        if (!env.index.Checkpoint().ok()) ++errors; else ++checkpoints;
        if (!env.index.Validate().ok()) ++errors; else ++validations;
      }
    }
  });
  go.store(true);
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(errors.load(), 0u);
  EXPECT_EQ(checkpoints.load(), 4u); EXPECT_EQ(validations.load(), 4u);
  ASSERT_TRUE(env.index.Validate().ok());
  const auto after = env.index.stats();
  EXPECT_TRUE(after.heat.touches > baseline.heat.touches);
  EXPECT_EQ(after.heat.decays, 24u);
  EXPECT_TRUE(after.pool.misses.load() > baseline.pool.misses.load());
  EXPECT_EQ(env.index.pool()->latch_stats().order_violations.load(), 0u);
  Query all; all.kind = QueryKind::kRange1D; all.key_hi = ~static_cast<KeyId>(0);
  std::map<KeyId, IndexEntry> actual;
  ASSERT_TRUE(env.index.Scan(all, [&](const IndexEntry& row) {
    if (!actual.emplace(row.key, row).second) ++errors;
    return true;
  }).ok());
  ASSERT_EQ(actual.size(), expected.size());
  for (const auto& kv : expected) {
    const auto it = actual.find(kv.first);
    ASSERT_TRUE(it != actual.end()); EXPECT_TRUE(Same(it->second, kv.second));
  }
  EXPECT_EQ(errors.load(), 0u);
  ASSERT_TRUE(env.index.Close().ok());
  EXPECT_TRUE(env.index.Get(0).status().code() == StatusCode::kCorruption);
  EXPECT_EQ(env.index.stats().heat.touches, 0u);
  std::printf("  HYBRID_HEAT readers=5 writers=2 frames=12 errors=%llu checkpoints=%llu "
              "validations=%llu heat_touches=%llu decays=%llu misses=%llu entries=%zu\n",
              static_cast<unsigned long long>(errors.load()),
              static_cast<unsigned long long>(checkpoints.load()),
              static_cast<unsigned long long>(validations.load()),
              static_cast<unsigned long long>(after.heat.touches),
              static_cast<unsigned long long>(after.heat.decays),
              static_cast<unsigned long long>(after.pool.misses.load() - baseline.pool.misses.load()),
              actual.size());
}

TEST(HybridConcurrency, SameObjectReopenAndFailedOpenRetry) {
  Env env("reopen");
  for (KeyId key = 0; key < 32; ++key) ASSERT_TRUE(env.index.Put(Make(key)).ok());
  for (int round = 0; round < 3; ++round) {
    ASSERT_TRUE(env.index.Close().ok());
    EXPECT_FALSE(env.index.is_open());
    ASSERT_TRUE(env.index.Open(env.options).ok());
    EXPECT_EQ(env.index.stats().writes, 0u);
    for (KeyId key = 0; key < 32; ++key) {
      auto row = env.index.Get(key);
      ASSERT_TRUE(row.ok()); EXPECT_TRUE(Same(row.value(), Make(key)));
    }
    ASSERT_TRUE(env.index.Validate().ok());
  }
  ASSERT_TRUE(env.index.Close().ok());
  auto invalid = env.options;
  invalid.rtree_slot = kMaxTreeSlots; // fails after disk/pool and B+ have opened
  EXPECT_FALSE(env.index.Open(invalid).ok());
  EXPECT_FALSE(env.index.is_open());
  ASSERT_TRUE(env.index.Open(env.options).ok());
  auto row = env.index.Get(7);
  ASSERT_TRUE(row.ok()); EXPECT_TRUE(Same(row.value(), Make(7)));
  ASSERT_TRUE(env.index.Validate().ok());
}

TEST(HybridConcurrency, KnnReadersOverlapAndNestedQueriesRemainIndependent) {
  Env env("knn_overlap");
  for (KeyId key = 0; key < 96; ++key) ASSERT_TRUE(env.index.Put(Make(key)).ok());
  std::mutex mutex;
  std::condition_variable cv;
  int active = 0, peak = 0;
  bool release = false;
  std::atomic<int> errors{0};
  auto reader = [&](KeyId probe) {
    Query q; q.kind = QueryKind::kKnn; q.k = 3; q.probe = Point(2);
    q.probe[0] = q.probe[1] = static_cast<double>(probe);
    int seen = 0;
    const auto status = env.index.Query(q, [&](const IndexEntry& row) {
      ++seen;
      if (!Same(row, Make(probe))) ++errors;
      Query nested = q; nested.probe[0] = nested.probe[1] = static_cast<double>(probe + 2);
      std::vector<IndexEntry> rows;
      if (!env.index.Knn(nested, 1, &rows).ok() || rows.size() != 1 ||
          !Same(rows[0], Make(probe + 2))) ++errors;
      if (env.index.Put(Make(probe, 1)).code() != StatusCode::kInvalidArgument) ++errors;
      std::unique_lock<std::mutex> lock(mutex);
      ++active; peak = std::max(peak, active); cv.notify_all();
      if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return release; })) ++errors;
      --active;
      return false;
    });
    if (!status.ok() || seen != 1) ++errors;
  };
  std::thread a(reader, 7), b(reader, 80);
  {
    std::unique_lock<std::mutex> lock(mutex);
    const bool overlapped = cv.wait_for(lock, std::chrono::seconds(5), [&] { return active == 2; });
    EXPECT_TRUE(overlapped);
    release = true; cv.notify_all();
  }
  a.join(); b.join();
  EXPECT_EQ(peak, 2);
  EXPECT_EQ(errors.load(), 0);
  EXPECT_TRUE(env.index.Validate().ok());
  EXPECT_EQ(env.index.pool()->latch_stats().order_violations.load(), 0u);
}

TEST(HybridConcurrency, ParallelKnnMatchesIndependentTopKAfterReopen) {
  Env env("knn_parallel");
  constexpr KeyId count = 96;
  for (KeyId key = 0; key < count; ++key) ASSERT_TRUE(env.index.Put(Make(key)).ok());
  ASSERT_TRUE(env.index.Validate().ok()); // refresh the structural page-count snapshot
  const auto before = env.index.stats();
  ASSERT_TRUE(before.bplus.num_pages + before.rtree.num_pages > 12u);
  ASSERT_TRUE(env.index.Close().ok());
  ASSERT_TRUE(env.index.Open(env.options).ok());
  std::atomic<bool> go{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> readers;
  for (int role = 0; role < 4; ++role) readers.emplace_back([&, role] {
    while (!go.load()) std::this_thread::yield();
    for (int round = 0; round < 48; ++round) {
      const double pos = static_cast<double>((round * 7 + role * 11) % 96);
      Query q; q.probe = Point(2); q.probe[0] = pos + 0.13; q.probe[1] = pos + 0.71;
      const uint32_t sizes[] = {1, 5, 17, 200};
      const uint32_t k = sizes[round % 4];
      std::vector<std::pair<double, KeyId>> expected;
      for (KeyId key = 0; key < count; ++key) {
        const double x = static_cast<double>(key) - q.probe[0];
        const double y = static_cast<double>(key) - q.probe[1];
        expected.emplace_back(x * x + y * y, key);
      }
      std::sort(expected.begin(), expected.end());
      std::vector<IndexEntry> actual;
      if (!env.index.Knn(q, k, &actual).ok() || actual.size() != std::min<size_t>(k, count)) {
        ++errors; continue;
      }
      for (size_t i = 0; i < actual.size(); ++i)
        if (!Same(actual[i], Make(expected[i].second))) ++errors;
    }
  });
  go.store(true);
  for (auto& reader : readers) reader.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_TRUE(env.index.stats().pool.misses.load() > 0u);
  EXPECT_TRUE(env.index.Validate().ok());
  EXPECT_EQ(env.index.pool()->latch_stats().order_violations.load(), 0u);
}

TEST(HybridConcurrency, KnnTopKTiesRemainOrderedAfterReopen) {
  Env env("knn_topk_ties");
  const auto make = [](KeyId key) {
    auto row = Make(key);
    row.mbr.lo[0] = row.mbr.hi[0] = 1.0;
    row.mbr.lo[1] = row.mbr.hi[1] = 0.0;
    return row;
  };
  for (KeyId key = 48; key-- > 0;) ASSERT_TRUE(env.index.Put(make(key)).ok());
  for (int stage = 0; stage < 2; ++stage) {
    for (uint32_t k : {0u, 1u, 7u, 47u, 48u, 100u}) {
      Query q; q.probe = Point(2);
      std::vector<IndexEntry> rows;
      ASSERT_TRUE(env.index.Knn(q, k, &rows).ok());
      // std::min returns a reference; materialize converted arguments before
      // the assertion macro binds its operands across separate statements.
      const size_t expected_count = std::min<size_t>(k == 0 ? 1u : k, 48u);
      ASSERT_EQ(rows.size(), expected_count);
      for (size_t i = 0; i < rows.size(); ++i) EXPECT_TRUE(Same(rows[i], make(static_cast<KeyId>(i))));
      q.kind = QueryKind::kKnn; q.k = k;
      size_t emitted = 0;
      ASSERT_TRUE(env.index.Query(q, [&](const IndexEntry& row) {
        ++emitted; EXPECT_TRUE(Same(row, make(0)));
        return false;
      }).ok());
      EXPECT_EQ(emitted, 1u);
    }
    ASSERT_TRUE(env.index.Validate().ok());
    if (stage == 0) {
      ASSERT_TRUE(env.index.Checkpoint().ok());
      ASSERT_TRUE(env.index.Close().ok());
      ASSERT_TRUE(env.index.Open(env.options).ok());
    }
  }
  EXPECT_EQ(env.index.pool()->latch_stats().order_violations.load(), 0u);
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

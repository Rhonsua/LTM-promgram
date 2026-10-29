// Fixed per-key histories: compare serial replay, concurrent replay and an oracle.
// Readers check immutable keys while writers mutate disjoint key spaces.
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {
constexpr uint64_t kImmutable = 512;
constexpr int kWriters = 3;
constexpr int kRounds = 240;
constexpr uint64_t kWriterKeys = 96;

IndexEntry Entry(KeyId key, const std::string& payload) {
  IndexEntry e;
  e.key = key;
  e.mbr = Mbr::Invalid(0);
  e.payload.assign(payload.begin(), payload.end());
  return e;
}

struct Operation { KeyId key; bool erase; std::string payload; };
using Streams = std::vector<std::vector<Operation>>;
using Snapshot = std::map<KeyId, std::string>;

Streams Generate(uint64_t seed) {
  Streams streams(kWriters);
  for (int t = 0; t < kWriters; ++t) {
    std::mt19937_64 rng(seed + static_cast<uint64_t>(t));
    for (int i = 0; i < kRounds; ++i) {
      const KeyId key = 10000 + static_cast<uint64_t>(t) * kWriterKeys + rng() % kWriterKeys;
      streams[static_cast<size_t>(t)].push_back({key, rng() % 4 == 0,
                           "s" + std::to_string(seed) + ":t" + std::to_string(t) +
                               ":i" + std::to_string(i) + ":k" + std::to_string(key)});
    }
  }
  return streams;
}

Snapshot Initial() {
  Snapshot result;
  for (KeyId k = 0; k < kImmutable; ++k) result[k] = "immutable:" + std::to_string(k);
  for (uint64_t k = 0; k < kWriterKeys * kWriters; ++k)
    result[10000 + k] = "initial:" + std::to_string(k);
  return result;
}

Snapshot Oracle(const Streams& streams) {
  auto result = Initial();
  for (const auto& stream : streams)
    for (const auto& op : stream) {
      if (op.erase) result.erase(op.key);
      else result[op.key] = op.payload;
    }
  return result;
}

struct Fixture {
  std::string data, log;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<WalWriter> wal;
  std::unique_ptr<BufferPool> pool;
  BPlusPageOps ops{16};
  BPlusTree tree;
  explicit Fixture(const std::string& tag)
      : data("log/_c1_replay_" + tag + ".data"), log("log/_c1_replay_" + tag + ".wal") {
    std::filesystem::create_directories("log");
    std::remove(data.c_str());
    std::remove(log.c_str());
    DiskOptions d;
    d.page_file = data; d.wal_file = log; d.num_pages = 2048;
    disk.reset(new DiskManager(d));
    wal.reset(new WalWriter(WalOptions{}, disk.get()));
    BufferPoolOptions p;
    p.pool_frames = 16;
    pool.reset(new BufferPool(p, disk.get(), wal.get(), nullptr, nullptr, nullptr));
    TreeBuildOptions b; b.order_or_capacity = 16;
    ASSERT_TRUE(tree.Open(pool.get(), &ops, b, 1).ok());
    for (const auto& kv : Initial()) ASSERT_TRUE(tree.Insert(Entry(kv.first, kv.second)).ok());
    ASSERT_TRUE(tree.Validate().ok());
    ASSERT_TRUE(tree.stats().num_pages > p.pool_frames);
  }
  ~Fixture() {
    pool.reset(); wal.reset(); disk.reset();
    std::remove(data.c_str()); std::remove(log.c_str());
  }
};

bool Apply(Fixture& f, const Operation& op) {
  auto e = Entry(op.key, op.payload);
  const auto s = op.erase ? f.tree.Erase(e) : f.tree.Insert(e);
  return s.ok() || (op.erase && s.code() == StatusCode::kNotFound);
}

Snapshot Collect(Fixture& f) {
  Snapshot result;
  bool duplicates = false;
  const auto s = f.tree.ScanAll([&](const RecordRef& r) {
    if (!result.emplace(r.key, std::string(r.payload, r.payload_len)).second) duplicates = true;
    EXPECT_EQ(r.mbr.lo.dim(), 0u);
    EXPECT_EQ(r.mbr.hi.dim(), 0u);
    return true;
  });
  EXPECT_TRUE(s.ok());
  EXPECT_FALSE(duplicates);
  EXPECT_TRUE(f.tree.Validate().ok());
  EXPECT_EQ(f.pool->latch_stats().order_violations.load(), 0u);
  EXPECT_EQ(f.pool->stats().i6_violations, 0u);
  return result;
}
}  // namespace

TEST(C1Replay, ConcurrentDisjointHistoriesMatchSerialAndOracle) {
  Logger::Instance().SetLevel(LogLevel::kError);
  for (uint64_t seed : {42u, 2026u, 8675309u}) {
    const auto streams = Generate(seed);
    const auto expected = Oracle(streams);
    Snapshot serial;
    {
      Fixture f("serial");
      for (const auto& stream : streams)
        for (const auto& op : stream) ASSERT_TRUE(Apply(f, op));
      serial = Collect(f);
      ASSERT_TRUE(serial == expected);
    }
    Fixture f("concurrent");
    const auto misses = f.pool->stats().misses.load();
    std::atomic<bool> go{false};
    std::atomic<int> ready{0}, writers_left{kWriters};
    std::atomic<uint64_t> errors{0}, overlapping_reads{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kWriters; ++t) threads.emplace_back([&, t] {
      ++ready;
      while (!go.load()) std::this_thread::yield();
      for (const auto& op : streams[static_cast<size_t>(t)]) if (!Apply(f, op)) ++errors;
      --writers_left;
    });
    for (int t = 0; t < 3; ++t) threads.emplace_back([&, t] {
      ++ready;
      while (!go.load()) std::this_thread::yield();
      for (int i = 0; i < 4000; ++i) {
        const KeyId key = (static_cast<uint64_t>(i) * 31 + static_cast<uint64_t>(t)) % kImmutable;
        const bool during_writes = writers_left.load() > 0;
        auto r = f.tree.Search(Entry(key, ""));
        if (!r.ok() || r.value().key != key ||
            std::string(r.value().payload, r.value().payload_len) != "immutable:" + std::to_string(key))
          ++errors;
        if (during_writes) ++overlapping_reads;
      }
    });
    while (ready.load() != 6) std::this_thread::yield();
    go.store(true);
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(errors.load(), 0u);
    EXPECT_TRUE(overlapping_reads.load() > 0);
    const auto miss_delta = f.pool->stats().misses.load() - misses;
    EXPECT_TRUE(miss_delta > 0);
    const auto concurrent = Collect(f);
    EXPECT_TRUE(concurrent == serial);
    EXPECT_TRUE(concurrent == expected);
    std::printf("  C1_REPLAY seed=%llu writers=3 readers=3 write_ops=720 read_ops=12000 "
                "frames=16 entries=%zu errors=%llu overlapping_reads=%llu misses=%llu\n",
                static_cast<unsigned long long>(seed), concurrent.size(),
                static_cast<unsigned long long>(errors.load()),
                static_cast<unsigned long long>(overlapping_reads.load()),
                static_cast<unsigned long long>(miss_delta));
  }
}

TEST(C1Replay, MissingEraseRollbackIsSynchronizedWithReaders) {
  Logger::Instance().SetLevel(LogLevel::kError);
  Fixture f("missing_erase");
  const auto expected = Initial();
  const auto undo_before = f.pool->stats().abort_participant_undos;
  std::atomic<bool> go{false}, writing{true};
  std::atomic<int> ready{0};
  std::atomic<uint64_t> errors{0}, overlapping_reads{0};
  std::vector<std::thread> threads;
  threads.emplace_back([&] {
    ++ready;
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 128; ++i) {
      const auto s = f.tree.Erase(Entry(1000000, ""));
      if (s.code() != StatusCode::kNotFound) ++errors;
    }
    writing.store(false);
  });
  for (int t = 0; t < 3; ++t) threads.emplace_back([&, t] {
    ++ready;
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < 2048; ++i) {
      const KeyId key = (static_cast<KeyId>(i) * 31 + static_cast<KeyId>(t)) % kImmutable;
      const bool during_write = writing.load();
      auto record = f.tree.Search(Entry(key, ""));
      if (!record.ok() || record.value().key != key ||
          std::string(record.value().payload, record.value().payload_len) != "immutable:" + std::to_string(key))
        ++errors;
      if (during_write) ++overlapping_reads;
    }
  });
  while (ready.load() != 4) std::this_thread::yield();
  go.store(true);
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(errors.load(), 0u);
  EXPECT_TRUE(overlapping_reads.load() > 0);
  EXPECT_EQ(f.pool->stats().abort_participant_undos - undo_before, 128u);
  EXPECT_TRUE(Collect(f) == expected);
  std::printf("  C1_MISSING_ERASE write_ops=128 read_ops=6144 errors=%llu overlapping_reads=%llu\n",
              static_cast<unsigned long long>(errors.load()),
              static_cast<unsigned long long>(overlapping_reads.load()));
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

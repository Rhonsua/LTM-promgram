#include <array>
#include <cstdio>
#include <filesystem>
#include "bench/mixed_reference.h"
#include "index/hybrid_index.h"
#include "tests/harness/test_harness.h"
using namespace ltm;

TEST(MixedReference, SparseKeysMissingRowsAndChangedPayloadAreRejected) {
  bench::MixedTruth truth;
  for (KeyId key : {10ull, 20ull, 90ull}) truth.emplace(key, bench::MixedEntry(key, 0, 128));
  Query q; q.kind = QueryKind::kRange1D; q.key_lo = 0; q.key_hi = 99;
  auto rows = std::vector<IndexEntry>{truth.at(10), truth.at(20), truth.at(90)};
  EXPECT_TRUE(bench::VerifyMixedRows(truth, q, rows));
  rows[1].payload = {'x'}; EXPECT_FALSE(bench::VerifyMixedRows(truth, q, rows));
  rows[1] = truth.at(10); EXPECT_FALSE(bench::VerifyMixedRows(truth, q, rows));
  rows.pop_back(); EXPECT_FALSE(bench::VerifyMixedRows(truth, q, rows));
  q.kind = QueryKind::kKnn; q.probe = truth.at(10).mbr.lo; q.k = 2;
  EXPECT_TRUE(bench::VerifyMixedRows(truth, q, {truth.at(10), truth.at(20)}));
  EXPECT_FALSE(bench::VerifyMixedRows(truth, q, {truth.at(20), truth.at(90)}));
}
TEST(MixedReference, KnnTiesRequireExactKeyOrderAndCompleteRecords) {
  bench::MixedTruth truth;
  const auto box = bench::MixedEntry(0, 0, 32).mbr;
  for (KeyId key : {9ull, 7ull, 1ull}) {
    auto row = bench::MixedEntry(key, 0, 32); row.mbr = box; truth.emplace(key, row);
  }
  Query q; q.kind = QueryKind::kKnn; q.probe = Point(2); q.k = 2;
  EXPECT_TRUE(bench::VerifyMixedRows(truth, q, {truth.at(1), truth.at(7)}));
  EXPECT_FALSE(bench::VerifyMixedRows(truth, q, {truth.at(7), truth.at(9)}));
  EXPECT_FALSE(bench::VerifyMixedRows(truth, q, {truth.at(7), truth.at(1)}));
  auto changed = truth.at(1); changed.payload.push_back('x');
  EXPECT_FALSE(bench::VerifyMixedRows(truth, q, {changed, truth.at(7)}));
  q.k = 0;
  EXPECT_TRUE(bench::VerifyMixedRows(truth, q, {truth.at(1)}));
  EXPECT_FALSE(bench::VerifyMixedRows(truth, q, {truth.at(7)}));
}

TEST(MixedReference, FixedTraceIsBoundedAndCoversMutationSemantics) {
  for (uint64_t seed : {0ull, 42ull, 987654321ull}) {
    const auto trace = bench::MakeMixedTrace(seed, 512, 300, 16, 8, 10);
    const auto repeat = bench::MakeMixedTrace(seed, 512, 300, 16, 8, 10);
    ASSERT_EQ(trace.operations.size(), size_t{300});
    EXPECT_EQ(trace.operation_hash, repeat.operation_hash);
    std::array<uint64_t, 7> counts{};
    auto model = trace.initial;
    for (const auto& op : trace.operations) {
      ++counts[static_cast<size_t>(op.kind)]; bench::ApplyMixed(op, &model);
      EXPECT_TRUE(model.size() <= 640 && model.size() >= 256);
    }
    for (size_t type = 0; type < counts.size(); ++type) EXPECT_EQ(counts[type], bench::kMixedCounts[type] * 3);
    EXPECT_EQ(bench::MixedTruthHash(model), bench::MixedTruthHash(trace.final));
    EXPECT_TRUE(trace.put_new > 0 && trace.put_update > 0 && trace.put_reinsert >= 3);
    EXPECT_TRUE(trace.erase_found >= 3 && trace.erase_missing >= 3);
  }
}
TEST(MixedReference, ActualIndexWrongPayloadFailsIndependentOracle) {
  const std::string data = "log/_mixed_oracle.data", wal = "log/_mixed_oracle.wal";
  std::filesystem::create_directories("log"); std::remove(data.c_str()); std::remove(wal.c_str());
  {
    HybridOptions options; options.config.disk.page_file = data; options.config.disk.wal_file = wal;
    options.config.pool.pool_frames = 16; options.rtree.mbr_dim = 2;
    options.bplus.order_or_capacity = 32; options.rtree.order_or_capacity = 32;
    HybridIndex index; ASSERT_TRUE(index.Open(options).ok());
    auto expected = bench::MixedEntry(42, 0, 64);
    const auto status = index.Put(expected);
    if (!status.ok()) std::fprintf(stderr, "Put failed: %s\n", status.message().c_str());
    ASSERT_TRUE(status.ok());
    auto changed = expected; changed.payload = {'w', 'r', 'o', 'n', 'g'}; ASSERT_TRUE(index.Put(changed).ok());
    auto read = index.Get(42); ASSERT_TRUE(read.ok());
    EXPECT_FALSE(bench::MixedEqual(read.value(), expected));
    EXPECT_TRUE(bench::MixedEqual(read.value(), changed));
    ASSERT_TRUE(index.Close().ok());
  }
  std::remove(data.c_str()); std::remove(wal.c_str());
}
TEST(MixedReference, SmallLongTraceKeepsRoomForDeleteReinsert) {
  for (uint64_t seed : {0ull, 42ull, 987654321ull}) {
    const auto trace = bench::MakeMixedTrace(seed, 32, 100000, 4, 8, 16);
    auto model = trace.initial;
    for (const auto& op : trace.operations) {
      bench::ApplyMixed(op, &model);
      ASSERT_TRUE(model.size() >= 16 && model.size() <= 40);
    }
    EXPECT_EQ(bench::MixedTruthHash(model), bench::MixedTruthHash(trace.final));
  }
}
int main(int argc, char** argv) { return test::RunAll(argc, argv); }

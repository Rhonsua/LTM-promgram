// Functional chain: create/checkpoint/close, then verify in a separate process.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>
#include "bench/spatial_reference.h"
#include "index/hybrid_index.h"
#include "util/log.h"

namespace {
constexpr uint64_t kRecords = 512;
constexpr uint32_t kFrames = 12;
bool Check(const ltm::Status& status) {
  if (status.ok()) return true;
  std::fprintf(stderr, "PIPELINE_ERROR: %s\n", status.ToString().c_str()); return false;
}
bool Require(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "PIPELINE_ERROR: %s\n", message); return false;
}
void Step(const char* step) { std::printf("PIPELINE_STEP %s\n", step); std::fflush(stdout); }
std::vector<ltm::IndexEntry> Dataset() {
  std::vector<ltm::IndexEntry> truth(static_cast<size_t>(kRecords));
  for (uint64_t key = 0; key < kRecords; ++key) {
    auto& e = truth[static_cast<size_t>(key)]; e.key = key;
    e.mbr.lo = ltm::Point(2); e.mbr.hi = ltm::Point(2);
    e.mbr.lo[0] = static_cast<double>(key % 23) * 4 + static_cast<double>(key % 7) / 16;
    e.mbr.lo[1] = static_cast<double>(key / 23) * 4 + static_cast<double>(key % 11) / 32;
    e.mbr.hi[0] = e.mbr.lo[0] + .75; e.mbr.hi[1] = e.mbr.lo[1] + .5;
    const auto payload = std::to_string(key) + ":pipeline-payload";
    e.payload.assign(payload.begin(), payload.end());
  }
  return truth;
}
bool Same(const ltm::IndexEntry& a, const ltm::IndexEntry& b) {
  return a.key == b.key && a.payload == b.payload && a.mbr.lo.coords() == b.mbr.lo.coords() && a.mbr.hi.coords() == b.mbr.hi.coords();
}
void Hash(uint64_t value, uint64_t* hash) {
  for (int i = 0; i < 8; ++i) { *hash ^= value & 255u; *hash *= 1099511628211ull; value >>= 8; }
}
uint64_t Fingerprint(const std::vector<ltm::IndexEntry>& rows) {
  uint64_t hash = 14695981039346656037ull;
  for (const auto& row : rows) {
    Hash(row.key, &hash); Hash(row.dim(), &hash);
    for (uint16_t d = 0; d < row.dim(); ++d) {
      uint64_t lo = 0, hi = 0; const double low = row.mbr.lo[d], high = row.mbr.hi[d];
      std::memcpy(&lo, &low, sizeof(lo)); std::memcpy(&hi, &high, sizeof(hi));
      Hash(lo, &hash); Hash(hi, &hash);
    }
    Hash(row.payload.size(), &hash);
    for (char c : row.payload) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
  }
  return hash;
}
int Run(int argc, char** argv) {
  std::string stage, directory;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help") {
      std::puts("usage: ltm_pipeline --stage=load|verify --run-dir=DATABASE_DIR"); return 0;
    }
    if (a.rfind("--stage=", 0) == 0) stage = a.substr(8);
    else if (a.rfind("--run-dir=", 0) == 0) directory = a.substr(10);
    else return 2;
  }
  if ((stage != "load" && stage != "verify") || directory.empty()) return 2;
  const auto dir = std::filesystem::absolute(std::filesystem::u8path(directory));
  const auto data = dir / "pages.data", wal = dir / "records.wal";
  if (stage == "load") {
    if (!Require(!std::filesystem::exists(data) && !std::filesystem::exists(wal), "refusing to overwrite an existing database")) return 2;
    std::filesystem::create_directories(dir);
  } else {
    if (!Require(std::filesystem::is_regular_file(data) && std::filesystem::is_regular_file(wal), "verify requires an existing database")) return 2;
    if (!Require(std::filesystem::file_size(data) > 0 && std::filesystem::file_size(wal) > 0, "database files are empty")) return 2;
  }
  ltm::HybridOptions options;
  options.config.disk.page_file = data.u8string(); options.config.disk.wal_file = wal.u8string();
  options.config.disk.num_pages = 4096; options.config.pool.pool_frames = kFrames;
  options.config.pool.enable_prefetch = false;
  options.bplus.order_or_capacity = 32; options.rtree.order_or_capacity = 32;
  options.rtree.mbr_dim = 2; options.rtree.split_policy = "quadratic";
  ltm::HybridIndex index;
  if (!Check(index.Open(options))) return 1;
  Step(stage == "load" ? "created_database" : "reopened_database");
  const auto truth = Dataset();
  if (stage == "load") {
    std::vector<uint64_t> order(static_cast<size_t>(kRecords)); std::iota(order.begin(), order.end(), 0ull);
    std::mt19937_64 rng(42);
    for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[rng() % i]);
    for (uint64_t key : order) if (!Check(index.Put(truth[static_cast<size_t>(key)]))) return 1;
    Step("inserted_512_records");
    if (!Check(index.Checkpoint())) return 1;
    Step("checkpoint_saved");
  }
  for (const auto& expected : truth) {
    auto got = index.Get(expected.key);
    if (!Check(got.status()) || !Require(got.ok() && Same(got.value(), expected), "point lookup mismatch")) return 1;
  }
  auto missing = index.Get(kRecords + 100);
  if (!Require(!missing.ok() && missing.status().code() == ltm::StatusCode::kNotFound, "missing key returned a wrong status")) return 1;
  Step("point_and_missing_key_verified");
  std::vector<ltm::IndexEntry> rows;
  ltm::Query q; q.kind = ltm::QueryKind::kRange1D; q.key_lo = 0; q.key_hi = kRecords - 1;
  auto collect = [&]() {
    rows.clear(); return index.Query(q, [&](const ltm::IndexEntry& e) { rows.push_back(e); return true; });
  };
  if (!Check(collect()) || !Require(rows.size() == truth.size(), "full scan count mismatch")) return 1;
  for (size_t i = 0; i < rows.size(); ++i) if (!Require(Same(rows[i], truth[i]), "full scan record/order mismatch")) return 1;
  const auto dataset_hash = Fingerprint(rows);
  q.key_lo = 19; q.key_hi = 83;
  if (!Check(collect()) || !Require(rows.size() == 65, "range count mismatch")) return 1;
  for (size_t i = 0; i < rows.size(); ++i) if (!Require(Same(rows[i], truth[i + 19]), "range record/order mismatch")) return 1;
  Step("full_scan_and_range_verified");
  q.mbr.lo = ltm::Point(2); q.mbr.hi = ltm::Point(2);
  q.mbr.lo[0] = 15.5; q.mbr.hi[0] = 24.5; q.mbr.lo[1] = 7.5; q.mbr.hi[1] = 16.5;
  q.kind = ltm::QueryKind::kIntersect;
  if (!Check(collect()) || !Require(ltm::bench::VerifyReference(truth, q, ltm::bench::PrepareReference(truth, q), rows), "spatial intersection mismatch")) return 1;
  const auto spatial_rows = rows.size();
  q.kind = ltm::QueryKind::kRangeSpatial;
  if (!Check(collect()) || !Require(ltm::bench::VerifyReference(truth, q, ltm::bench::PrepareReference(truth, q), rows), "spatial containment mismatch")) return 1;
  const auto contain_rows = rows.size();
  if (!Require(spatial_rows > contain_rows && contain_rows > 0, "window did not distinguish intersection and containment")) return 1;
  q.mbr.lo[0] = -3; q.mbr.lo[1] = -3; q.mbr.hi[0] = -2; q.mbr.hi[1] = -2;
  if (!Check(collect()) || !Require(rows.empty(), "empty window returned records")) return 1;
  Step("spatial_containment_and_empty_window_verified");
  q.kind = ltm::QueryKind::kKnn; q.k = 10; q.probe = ltm::Point(2);
  q.probe[0] = truth[123].mbr.lo[0] + 1.125; q.probe[1] = truth[123].mbr.lo[1] + 1.375;
  if (!Check(index.Knn(q, q.k, &rows)) || !Require(ltm::bench::VerifyReference(truth, q, ltm::bench::PrepareReference(truth, q), rows), "KNN mismatch")) return 1;
  Step("knn_verified");
  if (!Check(index.Validate())) return 1;
  const auto stats = index.stats(); const auto pages = stats.bplus.num_pages + stats.rtree.num_pages + 1;
  if (!Require(stats.bplus.num_entries == kRecords && stats.rtree.num_entries == kRecords && stats.i8_pairs_checked == kRecords, "dual-tree inventory mismatch") ||
      !Require(stats.pool.i6_violations == 0 && index.pool()->latch_stats().order_violations.load() == 0, "WAL barrier or lock order violation") ||
      !Require(pages > kFrames && stats.pool.evictions > 0 && stats.io.page_reads > 0, "small pool did not exercise disk reads and eviction") ||
      !Require(stats.writes == (stage == "load" ? kRecords : 0), "unexpected logical writes") || !Check(index.Close())) return 1;
  Step("dual_tree_validated_and_closed");
  std::cout << "PIPELINE_RESULT {\"schema_version\":1,\"stage\":\"" << stage << "\",\"correct\":true"
    << ",\"records\":512,\"pool_frames\":12,\"seed\":42,\"point_checked\":512,\"missing_checked\":1"
    << ",\"full_scan_rows\":512,\"range_rows\":65,\"spatial_rows\":" << spatial_rows << ",\"contain_rows\":" << contain_rows
    << ",\"empty_rows\":0,\"knn_rows\":10,\"i8_pairs_checked\":" << stats.i8_pairs_checked
    << ",\"working_set_pages\":" << pages << ",\"logical_writes\":" << stats.writes
    << ",\"page_reads\":" << stats.io.page_reads << ",\"page_writes\":" << stats.io.page_writes
    << ",\"wal_bytes\":" << stats.io.wal_bytes << ",\"evictions\":" << stats.pool.evictions
    << ",\"i6_violations\":0,\"lock_violations\":0,\"dataset_hash\":\"" << dataset_hash << "\"}\n";
  return 0;
}
}
int main(int argc, char** argv) {
  ltm::Logger::Instance().SetLevel(ltm::LogLevel::kError);
  try { return Run(argc, argv); }
  catch (const std::exception& e) { std::fprintf(stderr, "PIPELINE_ERROR: %s\n", e.what()); return 1; }
}

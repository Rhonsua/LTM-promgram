// Hybrid spatial benchmark; every timed result is verified outside its query interval.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "bench/spatial_reference.h"
#include "index/hybrid_index.h"

namespace {
using Clock = std::chrono::steady_clock;
double Seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
bool Number(const std::string& text, uint64_t* out) {
  const auto r = std::from_chars(text.data(), text.data() + text.size(), *out);
  return r.ec == std::errc() && r.ptr == text.data() + text.size();
}
std::string Json(const std::string& text) {
  std::string result = "\"";
  for (char c : text) {
    if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else result += static_cast<char>(c);
  }
  return result + '"';
}
void Hash(uint64_t value, uint64_t* hash) {
  for (int i = 0; i < 8; ++i) { *hash ^= value & 255u; *hash *= 1099511628211ull; value >>= 8; }
}
void HashDouble(double value, uint64_t* hash) {
  uint64_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); Hash(bits, hash);
}
bool Check(const ltm::Status& status) {
  if (status.ok()) return true;
  std::fprintf(stderr, "BENCH_ERROR: %s\n", status.ToString().c_str()); return false;
}
}

int RunHybridBenchmark(int argc, char** argv) {
  std::string workload, directory;
  uint64_t seed = 42, records = 2048, span = 16, width = 8, k = 10, warmup = 0;
  bool warmup_set = false;
  std::vector<std::string> config_args{argv[0]};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i]; const auto eq = a.find('=');
    if (a.rfind("--", 0) != 0 || eq == std::string::npos) return 2;
    const auto key = a.substr(2, eq - 2), value = a.substr(eq + 1);
    if (key == "workload") workload = value;
    else if (key == "run-dir") directory = value;
    else if (key == "seed" || key == "records" || key == "range-width" || key == "spatial-width" || key == "knn-k" || key == "warmup-ops") {
      uint64_t n = 0; if (!Number(value, &n)) return 2;
      if (key == "seed") seed = n;
      if (key == "records") records = n;
      if (key == "range-width") span = n;
      if (key == "spatial-width") width = n;
      if (key == "knn-k") k = n;
      if (key == "warmup-ops") { warmup = n; warmup_set = true; }
    } else if (key == "bench.ops" || key == "pool.pool_frames" || key == "disk.num_pages") {
      uint64_t n = 0; if (!Number(value, &n) || n == 0 || (key == "pool.pool_frames" && n > 65536)) return 2;
      config_args.push_back(a);
    } else return 2;
  }
  if ((workload != "spatial" && workload != "contain" && workload != "knn") || directory.empty() ||
      records == 0 || records > 200000 || span == 0 || span > records || width == 0 || width > 1000000 || k == 0 || k > records) return 2;
  std::vector<char*> config_argv; for (auto& a : config_args) config_argv.push_back(a.data());
  auto config = ltm::Config::FromCommandLine(static_cast<int>(config_argv.size()), config_argv.data());
  if (!config.ok()) { Check(config.status()); return 2; }
  auto cfg = config.value(); cfg.pool.enable_prefetch = false;
  if (cfg.disk.page_size != ltm::kPageSize || cfg.bench_ops == 0 || cfg.bench_ops > 1000000) return 2;
  if (!warmup_set) warmup = std::max<uint64_t>(1, cfg.bench_ops / 10);
  if (warmup > 1000000) return 2;
  const auto dir = std::filesystem::absolute(std::filesystem::u8path(directory));
  std::filesystem::create_directories(dir);
  const auto data = dir / "pages.data", wal = dir / "records.wal";
  if (std::filesystem::exists(data) || std::filesystem::exists(wal)) {
    std::fprintf(stderr, "BENCH_ERROR: run directory already contains a database\n"); return 2;
  }
  cfg.disk.page_file = data.u8string(); cfg.disk.wal_file = wal.u8string();
  cfg.disk.num_pages = std::max<uint64_t>(cfg.disk.num_pages, records * 4 + 64);
  ltm::HybridOptions options; options.config = cfg;
  options.bplus.order_or_capacity = 32; options.rtree.order_or_capacity = 32;
  options.rtree.mbr_dim = 2; options.rtree.split_policy = "quadratic";
  ltm::HybridIndex index; if (!Check(index.Open(options))) return 1;
  std::vector<ltm::IndexEntry> truth(static_cast<size_t>(records));
  std::vector<uint64_t> order(static_cast<size_t>(records));
  uint64_t dataset_hash = 14695981039346656037ull, query_hash = dataset_hash;
  const auto side = static_cast<uint64_t>(std::ceil(std::sqrt(static_cast<double>(records))));
  for (uint64_t key = 0; key < records; ++key) {
    auto& e = truth[static_cast<size_t>(key)]; e.key = key;
    e.mbr.lo = ltm::Point(2); e.mbr.hi = ltm::Point(2);
    e.mbr.lo[0] = static_cast<double>(key % side) * 4 + static_cast<double>(key % 7) / 16;
    e.mbr.lo[1] = static_cast<double>(key / side) * 4 + static_cast<double>(key % 11) / 32;
    e.mbr.hi[0] = e.mbr.lo[0] + .75; e.mbr.hi[1] = e.mbr.lo[1] + .5;
    const auto text = std::to_string(key) + ":benchmark-payload"; e.payload.assign(text.begin(), text.end());
    Hash(key, &dataset_hash); Hash(2, &dataset_hash);
    for (uint16_t d = 0; d < 2; ++d) { HashDouble(e.mbr.lo[d], &dataset_hash); HashDouble(e.mbr.hi[d], &dataset_hash); }
    for (char c : text) { dataset_hash ^= static_cast<unsigned char>(c); dataset_hash *= 1099511628211ull; }
  }
  std::iota(order.begin(), order.end(), 0ull);
  std::mt19937_64 load_rng(seed), query_rng(seed ^ 0xa1b2c3d4ull);
  for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[load_rng() % i]);
  const auto load_start = Clock::now();
  for (uint64_t key : order) if (!Check(index.Put(truth[static_cast<size_t>(key)]))) return 1;
  const double load_seconds = Seconds(load_start);
  if (!Check(index.Validate()) || !Check(index.Checkpoint())) return 1;
  const auto structure = index.stats();
  const uint64_t full_pages = structure.bplus.num_pages + structure.rtree.num_pages + 1;
  const uint64_t active_pages = structure.rtree.num_pages + 1;
  auto make_query = [&](uint64_t key) {
    ltm::Query q; q.kind = workload == "knn" ? ltm::QueryKind::kKnn :
                          workload == "contain" ? ltm::QueryKind::kRangeSpatial : ltm::QueryKind::kIntersect;
    q.k = static_cast<uint32_t>(k); q.probe = ltm::Point(2); q.mbr.lo = ltm::Point(2); q.mbr.hi = ltm::Point(2);
    for (uint16_t d = 0; d < 2; ++d) {
      const double offset = workload == "knn" ? (d == 0 ? 1.125 : 1.375) : (d == 0 ? .5 : .375);
      q.probe[d] = truth[static_cast<size_t>(key)].mbr.lo[d] + offset;
      q.mbr.lo[d] = q.probe[d] - static_cast<double>(width) / 2;
      q.mbr.hi[d] = q.probe[d] + static_cast<double>(width) / 2;
    }
    return q;
  };
  std::vector<uint64_t> inputs(static_cast<size_t>(warmup + cfg.bench_ops));
  std::unordered_map<uint64_t, ltm::bench::SpatialReference> references;
  const auto oracle_start = Clock::now();
  for (auto& key : inputs) {
    key = query_rng() % records; Hash(key, &query_hash); const auto q = make_query(key);
    for (uint16_t d = 0; d < 2; ++d) {
      HashDouble(q.probe[d], &query_hash); HashDouble(q.mbr.lo[d], &query_hash); HashDouble(q.mbr.hi[d], &query_hash);
    }
    if (references.find(key) == references.end()) references.emplace(key, ltm::bench::PrepareReference(truth, q));
  }
  Hash(workload == "knn" ? 4 : workload == "contain" ? 2 : 3, &query_hash);
  Hash(width, &query_hash); Hash(k, &query_hash);
  const double oracle_seconds = Seconds(oracle_start);
  // Visit the entire R tree before warmup; Validate visited the B+ pages as well.
  ltm::Query all; all.kind = ltm::QueryKind::kIntersect; all.mbr.lo = ltm::Point(2); all.mbr.hi = ltm::Point(2);
  for (uint16_t d = 0; d < 2; ++d) { all.mbr.lo[d] = -1; all.mbr.hi[d] = static_cast<double>(side) * 4 + 2; }
  std::vector<ltm::IndexEntry> rows;
  if (!Check(index.Query(all, [&](const ltm::IndexEntry& e) { rows.push_back(e); return true; })) ||
      !ltm::bench::VerifyReference(truth, all, ltm::bench::PrepareReference(truth, all), rows) ||
      structure.bplus.num_entries != records || structure.rtree.num_entries != records || structure.i8_pairs_checked != records) return 1;
  auto query = [&](uint64_t key) {
    rows.clear(); const auto q = make_query(key);
    if (workload == "knn") return index.Knn(q, static_cast<uint32_t>(k), &rows);
    return index.Query(q, [&](const ltm::IndexEntry& e) { rows.push_back(e); return true; });
  };
  auto verify = [&](uint64_t key) { return ltm::bench::VerifyReference(truth, make_query(key), references.at(key), rows); };
  for (uint64_t i = 0; i < warmup; ++i) {
    const auto key = inputs[static_cast<size_t>(i)]; if (!Check(query(key)) || !verify(key)) return 1;
  }
  const auto before = index.stats();
  std::vector<double> latency; latency.reserve(static_cast<size_t>(cfg.bench_ops));
  double timed_seconds = 0; uint64_t result_rows = 0;
  const auto wall_start = Clock::now();
  for (uint64_t i = warmup; i < warmup + cfg.bench_ops; ++i) {
    const auto key = inputs[static_cast<size_t>(i)]; const auto start = Clock::now();
    const auto status = query(key); const double elapsed = Seconds(start);
    timed_seconds += elapsed; latency.push_back(elapsed * 1e6);
    if (!Check(status) || !verify(key)) { std::fprintf(stderr, "BENCH_ERROR: query reference mismatch\n"); return 1; }
    result_rows += rows.size();
  }
  const double measured_wall_seconds = Seconds(wall_start); const auto after = index.stats();
  if (timed_seconds <= 0 || !Check(index.Validate()) || after.pool.i6_violations != 0 ||
      index.pool()->latch_stats().order_violations.load() != 0 || !Check(index.Close())) return 1;
  std::sort(latency.begin(), latency.end());
  auto percentile = [&](double p) { return latency[static_cast<size_t>(std::ceil(p * static_cast<double>(latency.size()))) - 1]; };
  const uint64_t hits = after.pool.hits.load() - before.pool.hits.load(), misses = after.pool.misses.load() - before.pool.misses.load();
  const auto regime = cfg.pool.pool_frames >= full_pages ? "resident" : cfg.pool.pool_frames < active_pages ? "evicting" : "indeterminate";
  std::cout << std::setprecision(17) << "BENCH_RESULT {\"schema_version\":2,\"engine\":\"hybrid\",\"threads\":1"
    << ",\"workload\":" << Json(workload) << ",\"seed\":" << seed << ",\"records\":" << records
    << ",\"ops\":" << cfg.bench_ops << ",\"warmup_ops\":" << warmup << ",\"range_width\":" << span
    << ",\"spatial_width\":" << width << ",\"knn_k\":" << k << ",\"mbr_dim\":2,\"split_policy\":\"quadratic\""
    << ",\"pool_frames\":" << cfg.pool.pool_frames << ",\"page_size\":" << cfg.disk.page_size
    << ",\"working_set_pages\":" << full_pages << ",\"active_pages\":" << active_pages
    << ",\"bplus_pages\":" << structure.bplus.num_pages << ",\"rtree_pages\":" << structure.rtree.num_pages
    << ",\"i8_pairs_checked\":" << structure.i8_pairs_checked << ",\"regime\":" << Json(regime)
    << ",\"dataset_hash\":" << Json(std::to_string(dataset_hash)) << ",\"query_hash\":" << Json(std::to_string(query_hash))
    << ",\"load_seconds\":" << load_seconds << ",\"oracle_seconds\":" << oracle_seconds << ",\"measurement_seconds\":" << timed_seconds
    << ",\"measured_wall_seconds\":" << measured_wall_seconds << ",\"throughput_ops_s\":" << static_cast<double>(cfg.bench_ops) / timed_seconds
    << ",\"p50_us\":" << percentile(.50) << ",\"p95_us\":" << percentile(.95) << ",\"p99_us\":" << percentile(.99) << ",\"p999_us\":" << percentile(.999)
    << ",\"hits\":" << hits << ",\"misses\":" << misses << ",\"hit_rate\":" << (hits + misses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses))
    << ",\"evictions\":" << after.pool.evictions - before.pool.evictions << ",\"dirty_evictions\":" << after.pool.dirty_evictions - before.pool.dirty_evictions
    << ",\"page_reads\":" << after.io.page_reads - before.io.page_reads << ",\"page_writes\":" << after.io.page_writes - before.io.page_writes
    << ",\"wal_bytes\":" << after.io.wal_bytes - before.io.wal_bytes << ",\"result_rows\":" << result_rows
    << ",\"correct\":true,\"i6_violations\":0,\"lock_violations\":0,\"height\":" << structure.rtree.height
    << ",\"order_capacity\":32,\"heat\":\"enabled\",\"policy\":\"migration\",\"config_dump\":" << Json(cfg.Dump())
    << ",\"run_dir\":" << Json(dir.u8string()) << "}\n";
  return 0;
}

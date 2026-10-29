// B+ benchmark: fixed input, checked records, query intervals exclude verification.
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
#include <vector>
#include "index/bplus_tree.h"
#include "storage/recovery.h"
#include "util/config.h"
#include "util/log.h"

int RunHybridBenchmark(int argc, char** argv);
int RunMixedBenchmark(int argc, char** argv);

namespace {
using Clock = std::chrono::steady_clock;
double Seconds(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}
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
bool Check(const ltm::Status& status) {
  if (status.ok()) return true;
  std::fprintf(stderr, "BENCH_ERROR: %s\n", status.ToString().c_str()); return false;
}
int Run(int argc, char** argv) {
  std::string workload = "point", directory;
  uint64_t seed = 42, records = 2048, span = 16, warmup = 0;
  bool warmup_set = false;
  std::vector<std::string> config_args{argv[0]};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help") {
      std::puts("usage: ltm_bench --run-dir=EMPTY_DIR [--workload=point|range] [--seed=N]");
      std::puts("       [--records=N] [--range-width=N] [--warmup-ops=N] [--bench.ops=N]");
      std::puts("       [--pool.pool_frames=N] [--disk.num_pages=N]");
      std::puts("       Hybrid: --workload=spatial|contain|knn [--spatial-width=N] [--knn-k=N]");
      std::puts("       Mixed: --workload=mixed [--phase=run|verify] (ops must be a multiple of 100)");
      std::puts("              [--trajectory=checked|measure] (measure omits write-after-read probes)");
      std::puts("output: one BENCH_RESULT JSON record; B+ uses CLOCK, Hybrid uses migration policy"); return 0;
    }
    const auto eq = a.find('=');
    if (a.rfind("--", 0) != 0 || eq == std::string::npos) return 2;
    const std::string key = a.substr(2, eq - 2), value = a.substr(eq + 1);
    if (key == "workload") workload = value;
    else if (key == "run-dir") directory = value;
    else if (key == "seed" || key == "records" || key == "range-width" || key == "warmup-ops") {
      uint64_t n = 0; if (!Number(value, &n)) return 2;
      if (key == "seed") seed = n;
      if (key == "records") records = n;
      if (key == "range-width") span = n;
      if (key == "warmup-ops") { warmup = n; warmup_set = true; }
    } else if (key == "bench.ops" || key == "pool.pool_frames" || key == "disk.num_pages") {
      uint64_t n = 0;
      if (!Number(value, &n) || n == 0 || (key == "pool.pool_frames" && n > 65536)) return 2;
      config_args.push_back(a);
    } else { std::fprintf(stderr, "unsupported benchmark option: %s\n", key.c_str()); return 2; }
  }
  if ((workload != "point" && workload != "range") || directory.empty() ||
      records == 0 || records > 200000 || span == 0 || span > records) return 2;
  std::vector<char*> config_argv;
  for (auto& a : config_args) config_argv.push_back(a.data());
  auto config = ltm::Config::FromCommandLine(static_cast<int>(config_argv.size()), config_argv.data());
  if (!config.ok()) { Check(config.status()); return 2; }
  auto cfg = config.value();
  cfg.pool.enable_prefetch = false;
  if (cfg.disk.page_size != ltm::kPageSize) return 2;
  if (cfg.bench_ops == 0 || cfg.bench_ops > 1000000) return 2;
  if (!warmup_set) warmup = std::max<uint64_t>(1, cfg.bench_ops / 10);
  if (warmup > 1000000) return 2;
  const auto dir = std::filesystem::absolute(std::filesystem::u8path(directory));
  std::filesystem::create_directories(dir);
  const auto data = dir / "pages.data", wal = dir / "records.wal";
  if (std::filesystem::exists(data) || std::filesystem::exists(wal)) {
    std::fprintf(stderr, "BENCH_ERROR: run directory already contains a database\n"); return 2;
  }
  cfg.disk.page_file = data.u8string(); cfg.disk.wal_file = wal.u8string();
  cfg.disk.num_pages = std::max<uint64_t>(cfg.disk.num_pages, records * 2 + 64);
  ltm::DiskManager disk(cfg.disk);
  ltm::RecoveryManager recovery(cfg.wal, &disk);
  ltm::BufferPool pool(cfg.pool, &disk, recovery.wal(), &recovery);
  if (!Check(recovery.Recover(&pool))) return 1;
  ltm::BPlusPageOps ops(32); ltm::BPlusTree tree;
  ltm::TreeBuildOptions options; options.order_or_capacity = 32; options.caller_tree_slot = 0;
  if (!Check(tree.Open(&pool, &ops, options, 1))) return 1;
  std::vector<ltm::IndexEntry> truth(static_cast<size_t>(records));
  std::vector<uint64_t> order(static_cast<size_t>(records));
  uint64_t dataset_hash = 14695981039346656037ull, query_hash = dataset_hash;
  for (uint64_t key = 0; key < records; ++key) {
    auto& e = truth[static_cast<size_t>(key)]; e.key = key;
    const std::string text = std::to_string(key) + ":benchmark-payload";
    e.payload.assign(text.begin(), text.end()); Hash(key, &dataset_hash);
    for (char c : text) { dataset_hash ^= static_cast<unsigned char>(c); dataset_hash *= 1099511628211ull; }
  }
  std::iota(order.begin(), order.end(), 0ull);
  std::mt19937_64 load_rng(seed), query_rng(seed ^ 0xa1b2c3d4ull);
  for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[load_rng() % i]);
  const auto load_start = Clock::now();
  for (uint64_t key : order) if (!Check(tree.Insert(truth[static_cast<size_t>(key)]))) return 1;
  const double load_seconds = Seconds(load_start);
  if (!Check(tree.Validate())) return 1;
  ltm::CheckpointManager checkpoint(cfg.wal, &disk, &recovery, &pool);
  if (!Check(checkpoint.RunCheckpoint())) return 1;
  const auto structure = tree.stats();
  const uint64_t working_pages = structure.num_pages + 1;
  auto match = [&](const ltm::RecordRef& ref, uint64_t key) {
    const auto& expected = truth[static_cast<size_t>(key)];
    return ref.key == key && ref.mbr.lo.dim() == 0 && ref.payload_len == expected.payload.size() &&
           (ref.payload_len == 0 || (ref.payload != nullptr &&
             std::memcmp(ref.payload, expected.payload.data(), ref.payload_len) == 0));
  };
  uint64_t checked = 0; bool correct = true;
  if (!Check(tree.ScanAll([&](const ltm::RecordRef& ref) {
    if (checked >= records || !match(ref, checked)) correct = false;
    ++checked; return true;
  })) || !correct || checked != records || structure.num_entries != records) return 1;
  std::vector<uint64_t> inputs(static_cast<size_t>(warmup + cfg.bench_ops));
  for (auto& key : inputs) { key = query_rng() % records; Hash(key, &query_hash); }
  Hash(workload == "point" ? 0 : span, &query_hash);
  auto query = [&](uint64_t key, std::vector<ltm::RecordRef>* rows) {
    rows->clear();
    if (workload == "point") {
      ltm::IndexEntry probe; probe.key = key; auto row = tree.Search(probe);
      if (!row.ok()) return row.status();
      rows->push_back(row.value()); return ltm::Status::Ok();
    }
    ltm::Query q; q.kind = ltm::QueryKind::kRange1D;
    q.key_lo = key; q.key_hi = std::min(records - 1, key + span - 1);
    return tree.RangeScan(q, [&](const ltm::RecordRef& ref) { rows->push_back(ref); return true; });
  };
  auto verify = [&](uint64_t key, const std::vector<ltm::RecordRef>& rows) {
    const uint64_t count = workload == "point" ? 1 : std::min(span, records - key);
    if (rows.size() != count) return false;
    for (size_t i = 0; i < rows.size(); ++i) if (!match(rows[i], key + i)) return false;
    return true;
  };
  std::vector<ltm::RecordRef> rows;
  for (uint64_t i = 0; i < warmup; ++i)
    if (!Check(query(inputs[static_cast<size_t>(i)], &rows)) || !verify(inputs[static_cast<size_t>(i)], rows)) return 1;
  ltm::DiskManager::IoStats io_before, io_after;
  const auto before = pool.SnapshotStats(&io_before);
  std::vector<double> latency; latency.reserve(static_cast<size_t>(cfg.bench_ops));
  double timed_seconds = 0; uint64_t result_rows = 0;
  const auto wall_start = Clock::now();
  for (uint64_t i = warmup; i < warmup + cfg.bench_ops; ++i) {
    const uint64_t key = inputs[static_cast<size_t>(i)];
    const auto start = Clock::now(); const auto status = query(key, &rows);
    const double elapsed = Seconds(start);
    timed_seconds += elapsed; latency.push_back(elapsed * 1e6);
    if (!Check(status) || !verify(key, rows)) return 1;
    result_rows += rows.size();
  }
  const double measured_wall_seconds = Seconds(wall_start);
  const auto after = pool.SnapshotStats(&io_after);
  if (timed_seconds <= 0 || !Check(tree.Validate()) || !Check(pool.FlushAll()) ||
      !Check(recovery.MarkCleanShutdown()) || after.i6_violations != 0 ||
      pool.latch_stats().order_violations.load() != 0) return 1;
  std::sort(latency.begin(), latency.end());
  auto percentile = [&](double p) { return latency[static_cast<size_t>(std::ceil(p * static_cast<double>(latency.size()))) - 1]; };
  const uint64_t hits = after.hits.load() - before.hits.load(), misses = after.misses.load() - before.misses.load();
  std::cout << std::setprecision(17) << "BENCH_RESULT {\"schema_version\":1,\"engine\":\"bplus\",\"threads\":1"
    << ",\"workload\":" << Json(workload) << ",\"seed\":" << seed << ",\"records\":" << records
    << ",\"ops\":" << cfg.bench_ops << ",\"warmup_ops\":" << warmup << ",\"range_width\":" << span
    << ",\"pool_frames\":" << cfg.pool.pool_frames << ",\"page_size\":" << cfg.disk.page_size
    << ",\"working_set_pages\":" << working_pages << ",\"regime\":" << Json(cfg.pool.pool_frames >= working_pages ? "resident" : "evicting")
    << ",\"dataset_hash\":" << Json(std::to_string(dataset_hash)) << ",\"query_hash\":" << Json(std::to_string(query_hash))
    << ",\"load_seconds\":" << load_seconds << ",\"measurement_seconds\":" << timed_seconds
    << ",\"measured_wall_seconds\":" << measured_wall_seconds << ",\"throughput_ops_s\":" << static_cast<double>(cfg.bench_ops) / timed_seconds
    << ",\"p50_us\":" << percentile(.50) << ",\"p95_us\":" << percentile(.95) << ",\"p99_us\":" << percentile(.99) << ",\"p999_us\":" << percentile(.999)
    << ",\"hits\":" << hits << ",\"misses\":" << misses << ",\"hit_rate\":" << (hits + misses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses))
    << ",\"evictions\":" << after.evictions - before.evictions << ",\"dirty_evictions\":" << after.dirty_evictions - before.dirty_evictions
    << ",\"page_reads\":" << io_after.page_reads - io_before.page_reads << ",\"page_writes\":" << io_after.page_writes - io_before.page_writes
    << ",\"wal_bytes\":" << io_after.wal_bytes - io_before.wal_bytes << ",\"result_rows\":" << result_rows
    << ",\"correct\":true,\"i6_violations\":0,\"lock_violations\":0,\"height\":" << structure.height
    << ",\"order_capacity\":32,\"heat\":\"none\",\"policy\":\"clock\",\"config_dump\":" << Json(cfg.Dump())
    << ",\"run_dir\":" << Json(dir.u8string()) << "}\n";
  return 0;
}
}
int main(int argc, char** argv) {
  ltm::Logger::Instance().SetLevel(ltm::LogLevel::kError);
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--workload=mixed") return RunMixedBenchmark(argc, argv);
      if (a == "--workload=spatial" || a == "--workload=contain" || a == "--workload=knn")
        return RunHybridBenchmark(argc, argv);
    }
    return Run(argc, argv);
  }
  catch (const std::exception& e) { std::fprintf(stderr, "BENCH_ERROR: %s\n", e.what()); return 1; }
}

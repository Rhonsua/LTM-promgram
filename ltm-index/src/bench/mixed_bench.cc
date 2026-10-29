// Mixed workload: per-call API intervals, separate probes, persisted fresh-process verification.
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include "bench/mixed_reference.h"
#include "index/hybrid_index.h"

namespace {
using Clock = std::chrono::steady_clock;
using Counters = std::array<uint64_t, 13>;
constexpr std::array<const char*, 13> kCounters{"hits", "misses", "evictions", "dirty_evictions", "page_reads",
  "page_writes", "wal_bytes", "wal_appends", "fsyncs", "pages_created", "pages_allocated", "pages_freed", "logical_writes"};
double Seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
bool Require(bool value, const char* error) {
  if (!value) std::fprintf(stderr, "MIXED_ERROR: %s\n", error);
  return value;
}
bool Check(const ltm::Status& status) {
  if (!status.ok()) std::fprintf(stderr, "MIXED_ERROR: %s\n", status.ToString().c_str());
  return status.ok();
}
std::string Json(const std::string& text) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (byte < 32) { out += "\\u00"; out += hex[byte >> 4]; out += hex[byte & 15u]; }
    else out += c;
  }
  return out + '"';
}
Counters Snapshot(ltm::HybridIndex& index) {
  const auto s = index.stats();
  return {s.pool.hits.load(), s.pool.misses.load(), s.pool.evictions, s.pool.dirty_evictions,
    s.io.page_reads, s.io.page_writes, s.io.wal_bytes, s.io.wal_appends, s.io.fsyncs,
    s.pool.pages_created, s.pool.pages_allocated, s.pool.pages_freed, s.writes};
}
Counters Delta(const Counters& after, const Counters& before) {
  Counters out{}; for (size_t i = 0; i < out.size(); ++i) out[i] = after[i] - before[i]; return out;
}
void Add(Counters* total, const Counters& n) { for (size_t i = 0; i < total->size(); ++i) (*total)[i] += n[i]; }
void EmitCounters(const Counters& n) {
  std::cout << '{';
  for (size_t i = 0; i < n.size(); ++i) { if (i) std::cout << ','; std::cout << Json(kCounters[i]) << ':' << n[i]; }
  std::cout << '}';
}
struct Sample {
  ltm::bench::MixedKind kind;
  uint64_t key = 0, rows = 0;
  bool missing = false;
  double latency_us = 0, probe_seconds = 0;
  Counters io{}, probe_io{};
};
bool CheckGet(ltm::HybridIndex& index, const ltm::bench::MixedTruth& truth, uint64_t key) {
  const auto result = index.Get(key); const auto expected = truth.find(key);
  if (expected == truth.end()) return !result.ok() && result.status().code() == ltm::StatusCode::kNotFound;
  return result.ok() && ltm::bench::MixedEqual(result.value(), expected->second);
}
bool VerifyFinal(ltm::HybridIndex& index, const ltm::bench::MixedTrace& trace) {
  if (!Check(index.Validate())) return false;
  const auto stats = index.stats();
  if (!Require(stats.i8_pairs_checked == trace.final.size() && stats.bplus.num_entries == trace.final.size() &&
               stats.rtree.num_entries == trace.final.size(), "final dual-tree inventory mismatch")) return false;
  ltm::Query all; all.kind = ltm::QueryKind::kRange1D; all.key_hi = trace.max_keys - 1;
  std::vector<ltm::IndexEntry> rows;
  if (!Check(index.Query(all, [&](const ltm::IndexEntry& e) { rows.push_back(e); return true; })) ||
      !Require(ltm::bench::VerifyMixedRows(trace.final, all, rows), "final scan mismatch")) return false;
  for (uint64_t key = 0; key < trace.max_keys; ++key)
    if (!Require(CheckGet(index, trace.final, key), "final point/payload mismatch")) return false;
  for (auto kind : {ltm::bench::MixedKind::Spatial, ltm::bench::MixedKind::Contain, ltm::bench::MixedKind::Knn}) {
    const auto it = std::find_if(trace.operations.begin(), trace.operations.end(), [kind](const auto& op) { return op.kind == kind; });
    rows.clear();
    const auto status = kind == ltm::bench::MixedKind::Knn ? index.Knn(it->query, it->query.k, &rows) :
      index.Query(it->query, [&](const ltm::IndexEntry& e) { rows.push_back(e); return true; });
    if (!Check(status) || !Require(ltm::bench::VerifyMixedRows(trace.final, it->query, rows), "final spatial/KNN mismatch")) return false;
  }
  return Require(index.stats().pool.i6_violations == 0 && index.pool()->latch_stats().order_violations.load() == 0, "invariant violation");
}
}

int RunMixedBenchmark(int argc, char** argv) {
  std::map<std::string, std::string> args;
  const std::array<const char*, 12> allowed{"workload", "phase", "run-dir", "seed", "records", "bench.ops", "warmup-ops",
    "range-width", "spatial-width", "knn-k", "pool.pool_frames", "trajectory"};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i]; const auto eq = a.find('=');
    if (a.rfind("--", 0) != 0 || eq == std::string::npos) return 2;
    const auto key = a.substr(2, eq - 2);
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end() || !args.emplace(key, a.substr(eq + 1)).second) return 2;
  }
  auto number = [&](const char* key, uint64_t fallback, uint64_t* value) {
    *value = fallback; if (!args.count(key)) return true;
    const auto& text = args.at(key); const auto r = std::from_chars(text.data(), text.data() + text.size(), *value);
    return r.ec == std::errc() && r.ptr == text.data() + text.size();
  };
  uint64_t seed = 0, records = 0, count = 0, warmup = 0, span = 0, width = 0, k = 0, frames = 0;
  if (!number("seed", 42, &seed) || !number("records", 512, &records) || !number("bench.ops", 300, &count) ||
      !number("warmup-ops", 30, &warmup) || !number("range-width", 16, &span) || !number("spatial-width", 8, &width) ||
      !number("knn-k", 10, &k) || !number("pool.pool_frames", 128, &frames)) return 2;
  const auto phase = args.count("phase") ? args.at("phase") : "run";
  const auto trajectory = args.count("trajectory") ? args.at("trajectory") : "checked";
  const bool with_probes = trajectory == "checked";
  if (!Require(args["workload"] == "mixed" && !args["run-dir"].empty() && (phase == "run" || phase == "verify") &&
      (with_probes || trajectory == "measure") &&
      records >= 32 && records <= 20000 && count >= 100 && count <= 100000 && count % 100 == 0 && warmup <= 100000 &&
      span > 0 && span <= records && width > 0 && width <= 1000000 && k > 0 && k <= records / 2 && frames >= 4 && frames <= 65536,
      "invalid mixed parameters; ops must be a positive multiple of 100")) return 2;
  const auto directory = std::filesystem::absolute(std::filesystem::u8path(args.at("run-dir")));
  const auto data = directory / "pages.data", wal = directory / "records.wal";
  if (phase == "run") {
    if (!Require(!std::filesystem::exists(data) && !std::filesystem::exists(wal), "run directory already contains a database")) return 2;
    std::filesystem::create_directories(directory);
  } else if (!Require(std::filesystem::is_regular_file(data) && std::filesystem::is_regular_file(wal), "verify database is missing")) return 2;
  const auto oracle_start = Clock::now();
  auto trace = ltm::bench::MakeMixedTrace(seed, records, count, span, width, static_cast<uint32_t>(k));
  ltm::bench::MixedHash(warmup, &trace.query_hash);
  std::mt19937_64 warm_rng(seed ^ 0xc0ffeeull);
  std::vector<uint64_t> warm_keys;
  for (uint64_t i = 0; i < warmup; ++i) { warm_keys.push_back(warm_rng() % records); ltm::bench::MixedHash(warm_keys.back(), &trace.query_hash); }
  const double generation_seconds = Seconds(oracle_start);
  double oracle_seconds = 0;
  ltm::HybridOptions options; options.config.pool.pool_frames = static_cast<uint32_t>(frames);
  options.config.pool.enable_prefetch = false; options.config.bench_ops = count;
  options.config.disk.page_file = data.u8string(); options.config.disk.wal_file = wal.u8string();
  options.config.disk.num_pages = records * 4 + 64;
  options.bplus.order_or_capacity = 32; options.rtree.order_or_capacity = 32;
  options.rtree.mbr_dim = 2; options.rtree.split_policy = "quadratic"; options.create_if_missing = phase == "run";
  ltm::HybridIndex index; if (!Check(index.Open(options))) return 1;
  uint64_t write_probe_calls = 0;
  auto emit_identity = [&]() {
    std::cout << std::setprecision(17) << "MIXED_RESULT {\"schema_version\":" << (with_probes ? 1 : 2)
      << ",\"workload\":\"mixed\",\"engine\":\"hybrid\",\"threads\":1"
      << ",\"phase\":" << Json(phase) << ",\"seed\":" << seed << ",\"records\":" << records << ",\"ops\":" << count
      << ",\"warmup_ops\":" << warmup << ",\"range_width\":" << span << ",\"spatial_width\":" << width << ",\"knn_k\":" << k
      << ",\"pool_frames\":" << frames << ",\"page_size\":4096,\"mbr_dim\":2,\"order_capacity\":32,\"split_policy\":\"quadratic\""
      << ",\"heat\":\"enabled\",\"policy\":\"migration\",\"measurement_scope\":"
      << Json(with_probes ? "api_calls_with_interleaved_probes" : "api_calls_without_write_probes")
      << ",\"trajectory\":" << Json(trajectory) << ",\"write_probe_calls\":" << write_probe_calls
      << ",\"read_validation\":\"inline_full_record_oracle\",\"stats_sampling\":\"per_api_snapshots\""
      << ",\"query_counter_semantics\":\"successful_gets_and_all_query_calls\""
      << ",\"run_dir\":" << Json(directory.u8string()) << ",\"config_dump\":" << Json(options.config.Dump())
      << ",\"dataset_hash\":" << Json(std::to_string(trace.dataset_hash)) << ",\"operation_hash\":" << Json(std::to_string(trace.operation_hash))
      << ",\"query_hash\":" << Json(std::to_string(trace.query_hash)) << ",\"final_hash\":" << Json(std::to_string(ltm::bench::MixedTruthHash(trace.final)))
      << ",\"final_records\":" << trace.final.size() << ",\"peak_records\":" << trace.peak_records << ",\"record_bound\":" << trace.max_keys
      << ",\"i8_pairs_checked\":" << trace.final.size() << ",\"point_keys_checked\":" << trace.max_keys
      << ",\"final_scan_rows\":" << trace.final.size() << ",\"missing_keys_checked\":" << trace.max_keys - trace.final.size()
      << ",\"spatial_queries_checked\":3,\"generation_seconds\":" << generation_seconds
      << ",\"i6_violations\":0,\"lock_violations\":0,\"correct\":true";
  };
  if (phase == "verify") {
    const auto before = Snapshot(index); const auto start = Clock::now();
    if (!VerifyFinal(index, trace)) return 1;
    const auto duration = Seconds(start); const auto io = Delta(Snapshot(index), before);
    if (!Require(io[5] == 0 && io[6] == 0 && io[12] == 0, "reopen validation performed a logical write") || !Check(index.Close())) return 1;
    emit_identity(); std::cout << ",\"verification_seconds\":" << duration << ",\"readonly_io\":";
    EmitCounters(io); std::cout << "}\n"; return 0;
  }
  const auto load_start = Clock::now();
  for (auto key : trace.load_order) if (!Check(index.Put(trace.initial.at(key)))) return 1;
  if (!Check(index.Validate()) || !Check(index.Checkpoint())) return 1;
  const auto initial = index.stats(); const uint64_t initial_pages = initial.bplus.num_pages + initial.rtree.num_pages + 1;
  const double load_seconds = Seconds(load_start);
  for (auto key : warm_keys) if (!Require(CheckGet(index, trace.initial, key), "warmup mismatch")) return 1;
  if (!Check(index.pool()->EnsureMetaLoaded())) return 1;
  uint64_t peak_physical_pages = index.pool()->meta().next_unallocated;
  auto model = trace.initial;
  std::vector<Sample> samples; samples.reserve(trace.operations.size());
  Counters api_io{}, probe_io{}; double api_seconds = 0, probe_seconds = 0, snapshot_seconds = 0;
  const uint64_t queries_before = index.stats().queries;
  uint64_t uncounted_not_found_gets = 0;
  const auto interval_before = Snapshot(index); const auto wall_start = Clock::now();
  auto snapshot = [&]() { const auto start = Clock::now(); auto result = Snapshot(index); snapshot_seconds += Seconds(start); return result; };
  for (const auto& op : trace.operations) {
    Sample sample; sample.kind = op.kind; sample.key = op.key;
    std::vector<ltm::IndexEntry> rows;
    const auto before = snapshot(); const auto start = Clock::now();
    ltm::Status status = ltm::Status::Ok();
    if (op.kind == ltm::bench::MixedKind::Put) status = index.Put(op.entry);
    else if (op.kind == ltm::bench::MixedKind::Erase) status = index.Erase(op.key);
    else if (op.kind == ltm::bench::MixedKind::Get) {
      const auto found = index.Get(op.key); if (found.ok()) rows.push_back(found.value()); else status = found.status();
    } else if (op.kind == ltm::bench::MixedKind::Knn) status = index.Knn(op.query, op.query.k, &rows);
    else status = index.Query(op.query, [&](const ltm::IndexEntry& e) { rows.push_back(e); return true; });
    sample.latency_us = Seconds(start) * 1e6; const auto after = snapshot(); sample.io = Delta(after, before);
    const auto check_start = Clock::now();
    const bool missing = (op.kind == ltm::bench::MixedKind::Get || op.kind == ltm::bench::MixedKind::Erase) && !model.count(op.key);
    if (missing) { if (!Require(status.code() == ltm::StatusCode::kNotFound && rows.empty(), "missing key returned a wrong status")) return 1; }
    else if (!Check(status)) return 1;
    sample.missing = missing; sample.rows = rows.size();
    if (op.kind == ltm::bench::MixedKind::Get && missing) ++uncounted_not_found_gets;
    if (op.kind == ltm::bench::MixedKind::Get && !missing) {
      if (!Require(rows.size() == 1 && ltm::bench::MixedEqual(rows[0], model.at(op.key)), "Get record mismatch")) return 1;
    } else if (static_cast<size_t>(op.kind) > 0 && static_cast<size_t>(op.kind) < 5) {
      if (!Require(ltm::bench::VerifyMixedRows(model, op.query, rows), "query reference mismatch")) return 1;
    }
    ltm::bench::ApplyMixed(op, &model); oracle_seconds += Seconds(check_start);
    if (with_probes && static_cast<size_t>(op.kind) >= 5) {
      const auto pbefore = snapshot(); const auto pstart = Clock::now();
      if (!Require(CheckGet(index, model, op.key), "write-after-read probe mismatch")) return 1;
      if (!model.count(op.key)) ++uncounted_not_found_gets;
      ++write_probe_calls;
      sample.probe_seconds = Seconds(pstart); sample.probe_io = Delta(snapshot(), pbefore);
    }
    peak_physical_pages = std::max(peak_physical_pages, static_cast<uint64_t>(index.pool()->meta().next_unallocated));
    Add(&api_io, sample.io); Add(&probe_io, sample.probe_io);
    api_seconds += sample.latency_us / 1e6; probe_seconds += sample.probe_seconds; samples.push_back(sample);
  }
  const double measured_wall_seconds = Seconds(wall_start); const auto measurement_io = Delta(Snapshot(index), interval_before);
  const uint64_t measured_query_calls = index.stats().queries - queries_before;
  if (!Require(ltm::bench::MixedTruthHash(model) == ltm::bench::MixedTruthHash(trace.final), "model replay mismatch")) return 1;
  const auto verification_start = Clock::now(); if (!VerifyFinal(index, trace)) return 1;
  const double verification_seconds = Seconds(verification_start);
  const auto checkpoint_before = Snapshot(index); const auto checkpoint_start = Clock::now();
  if (!Check(index.Checkpoint())) return 1;
  const double checkpoint_seconds = Seconds(checkpoint_start); const auto checkpoint_io = Delta(Snapshot(index), checkpoint_before);
  if (!Check(index.Close())) return 1;
  const char* regime = frames >= peak_physical_pages && measurement_io[2] == 0 ? "resident" :
    frames < initial_pages && api_io[2] > 0 && api_io[4] > 0 ? "evicting" : "indeterminate";
  emit_identity();
  std::cout << ",\"regime\":" << Json(regime) << ",\"initial_working_set_pages\":" << initial_pages
    << ",\"peak_physical_pages\":" << peak_physical_pages << ",\"load_seconds\":" << load_seconds
    << ",\"measured_query_calls\":" << measured_query_calls
    << ",\"uncounted_not_found_gets\":" << uncounted_not_found_gets
    << ",\"measured_read_requests\":" << measured_query_calls + uncounted_not_found_gets
    << ",\"oracle_seconds\":" << oracle_seconds << ",\"snapshot_seconds\":" << snapshot_seconds
    << ",\"api_seconds\":" << api_seconds << ",\"probe_seconds\":" << probe_seconds << ",\"measured_wall_seconds\":" << measured_wall_seconds
    << ",\"throughput_api_ops_s\":" << static_cast<double>(count) / api_seconds
    << ",\"throughput_wall_ops_s\":" << static_cast<double>(count) / measured_wall_seconds
    << ",\"verification_seconds\":" << verification_seconds << ",\"checkpoint_seconds\":" << checkpoint_seconds
    << ",\"put_new\":" << trace.put_new << ",\"put_update\":" << trace.put_update << ",\"put_reinsert\":" << trace.put_reinsert
    << ",\"erase_found\":" << trace.erase_found << ",\"erase_missing\":" << trace.erase_missing << ",\"api_io\":"; EmitCounters(api_io);
  std::cout << ",\"probe_io\":"; EmitCounters(probe_io); std::cout << ",\"measurement_io\":"; EmitCounters(measurement_io);
  std::cout << ",\"checkpoint_io\":"; EmitCounters(checkpoint_io);
  std::cout << ",\"by_type\":{";
  for (size_t type = 0; type < 7; ++type) {
    std::vector<double> latency; Counters io{}; uint64_t missing = 0, result_rows = 0;
    for (const auto& sample : samples) if (static_cast<size_t>(sample.kind) == type) {
      latency.push_back(sample.latency_us); missing += sample.missing ? 1u : 0u; result_rows += sample.rows; Add(&io, sample.io);
    }
    const double seconds = std::accumulate(latency.begin(), latency.end(), 0.0) / 1e6;
    std::sort(latency.begin(), latency.end());
    auto percentile = [&](double p) { return latency[static_cast<size_t>(std::ceil(p * static_cast<double>(latency.size()))) - 1]; };
    if (type) std::cout << ',';
    std::cout << Json(ltm::bench::kMixedNames[type]) << ":{\"count\":" << latency.size() << ",\"ok\":" << latency.size() - missing
      << ",\"not_found\":" << missing << ",\"rows\":" << result_rows << ",\"seconds\":" << seconds
      << ",\"p50_us\":" << percentile(.5) << ",\"p95_us\":" << percentile(.95) << ",\"p99_us\":" << percentile(.99) << ",\"io\":";
    EmitCounters(io); std::cout << '}';
  }
  std::cout << "},\"samples\":[";
  for (size_t i = 0; i < samples.size(); ++i) {
    const auto& sample = samples[i]; if (i) std::cout << ',';
    std::cout << "{\"type\":" << Json(ltm::bench::kMixedNames[static_cast<size_t>(sample.kind)]) << ",\"key\":" << sample.key
      << ",\"status\":" << Json(sample.missing ? "not_found" : "ok") << ",\"rows\":" << sample.rows
      << ",\"latency_us\":" << sample.latency_us << ",\"probe_seconds\":" << sample.probe_seconds << ",\"io\":";
    EmitCounters(sample.io); std::cout << ",\"probe_io\":"; EmitCounters(sample.probe_io); std::cout << '}';
  }
  std::cout << "]}\n"; return 0;
}

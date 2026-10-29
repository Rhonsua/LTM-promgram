// Fixed total work across readers. Store all answers and validate after timing.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "index/hybrid_index.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t kBasis = 14695981039346656037ull;
struct Args {
  std::string stage, directory, regime;
  uint64_t records = 1024, ops = 768, warmup = 128, frames = 16, readers = 1, k = 16, seed = 42;
};
void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
void Check(const ltm::Status& status) { Require(status.ok(), status.ToString()); }
bool Parse(int argc, char** argv, Args* a) {
  std::vector<std::string> seen;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i]; const auto eq = arg.find('=');
    if (arg.rfind("--", 0) != 0 || eq == std::string::npos) return false;
    const auto key = arg.substr(2, eq - 2), value = arg.substr(eq + 1);
    if (value.empty() || std::find(seen.begin(), seen.end(), key) != seen.end()) return false;
    seen.push_back(key);
    if (key == "stage") a->stage = value;
    else if (key == "run-dir") a->directory = value;
    else if (key == "regime") a->regime = value;
    else {
      uint64_t number = 0;
      for (char c : value) {
        if (c < '0' || c > '9' || number > (1000000000ull - static_cast<unsigned>(c - '0')) / 10) return false;
        number = number * 10 + static_cast<unsigned>(c - '0');
      }
      if (key == "records") a->records = number;
      else if (key == "ops") a->ops = number;
      else if (key == "warmup") a->warmup = number;
      else if (key == "frames") a->frames = number;
      else if (key == "readers") a->readers = number;
      else if (key == "k") a->k = number;
      else if (key == "seed") a->seed = number;
      else return false;
    }
  }
  return !a->directory.empty() && (a->stage == "prepare" || a->stage == "measure" || a->stage == "verify") &&
    (a->regime == "resident" || a->regime == "evicting") &&
    a->records >= 64 && a->records <= 8192 && a->ops >= 8 && a->ops <= 8192 &&
    a->warmup >= 1 && a->warmup <= a->ops && a->readers >= 1 && a->readers <= 8 &&
    a->readers <= a->ops && a->frames >= 12 && a->frames <= 65536 && a->k >= 1 && a->k <= a->records &&
    a->ops * a->k <= 524288;  // Bound retained answers, independently of thread count.
}
void HashWord(uint64_t word, uint64_t* h) {
  for (int i = 0; i < 8; ++i) { *h ^= word & 255u; *h *= 1099511628211ull; word >>= 8; }
}
void HashDouble(double v, uint64_t* h) {
  uint64_t bits = 0; std::memcpy(&bits, &v, sizeof(bits)); HashWord(bits, h);
}
void HashRow(const ltm::IndexEntry& row, uint64_t* h) {
  HashWord(row.key, h); HashWord(row.dim(), h);
  for (uint16_t d = 0; d < row.dim(); ++d) { HashDouble(row.mbr.lo[d], h); HashDouble(row.mbr.hi[d], h); }
  HashWord(row.payload.size(), h);
  for (char c : row.payload) { *h ^= static_cast<unsigned char>(c); *h *= 1099511628211ull; }
}
bool Same(const ltm::IndexEntry& a, const ltm::IndexEntry& b) {
  return a.key == b.key && a.payload == b.payload && a.mbr.lo.coords() == b.mbr.lo.coords() &&
    a.mbr.hi.coords() == b.mbr.hi.coords();
}
std::vector<ltm::IndexEntry> Dataset(const Args& a) {
  const auto side = static_cast<uint64_t>(std::ceil(std::sqrt(static_cast<double>(a.records))));
  std::vector<ltm::IndexEntry> rows(static_cast<size_t>(a.records));
  for (uint64_t key = 0; key < a.records; ++key) {
    auto& row = rows[static_cast<size_t>(key)]; row.key = key;
    row.mbr.lo = ltm::Point(2); row.mbr.hi = ltm::Point(2);
    row.mbr.lo[0] = static_cast<double>(key % side) * 4;
    row.mbr.lo[1] = static_cast<double>(key / side) * 4;
    row.mbr.hi[0] = row.mbr.lo[0] + 1; row.mbr.hi[1] = row.mbr.lo[1] + 1;
    const auto payload = std::to_string(key) + ":knn-fixed-work";
    row.payload.assign(payload.begin(), payload.end());
  }
  return rows;
}
std::vector<ltm::Query> Queries(const Args& a, const std::vector<ltm::IndexEntry>& rows) {
  std::mt19937_64 rng(a.seed); std::vector<ltm::Query> queries(static_cast<size_t>(a.ops));
  for (auto& q : queries) {
    q.kind = ltm::QueryKind::kKnn; q.k = static_cast<uint32_t>(a.k); q.probe = ltm::Point(2);
    const auto& row = rows[static_cast<size_t>(rng() % a.records)];
    // Repeated exact half-grid probes deliberately exercise equal-distance ties.
    q.probe[0] = row.mbr.lo[0] + 2.5; q.probe[1] = row.mbr.lo[1] + 2.5;
  }
  return queries;
}
std::vector<std::vector<uint64_t>> Oracle(const Args& a, const std::vector<ltm::IndexEntry>& rows,
                                        const std::vector<ltm::Query>& queries) {
  std::vector<std::vector<uint64_t>> expected; expected.reserve(queries.size());
  for (const auto& q : queries) {
    std::vector<std::pair<double, uint64_t>> ranks; ranks.reserve(rows.size());
    for (const auto& row : rows) {
      double squared = 0;
      for (uint16_t d = 0; d < 2; ++d) {
        const double v = q.probe[d];
        const double gap = v < row.mbr.lo[d] ? row.mbr.lo[d] - v : (v > row.mbr.hi[d] ? v - row.mbr.hi[d] : 0);
        squared += gap * gap;
      }
      ranks.emplace_back(std::sqrt(squared), row.key);
    }
    std::sort(ranks.begin(), ranks.end()); std::vector<uint64_t> keys;
    for (uint64_t i = 0; i < a.k; ++i) keys.push_back(ranks[static_cast<size_t>(i)].second);
    expected.push_back(std::move(keys));
  }
  return expected;
}
ltm::HybridOptions Options(const Args& a, const std::filesystem::path& dir) {
  ltm::HybridOptions opt;
  opt.config.disk.page_file = (dir / "pages.data").u8string();
  opt.config.disk.wal_file = (dir / "records.wal").u8string();
  opt.config.disk.num_pages = a.records * 4 + 64;
  opt.config.pool.pool_frames = static_cast<uint32_t>(a.frames); opt.config.pool.enable_prefetch = false;
  opt.bplus.order_or_capacity = 32; opt.rtree.order_or_capacity = 32;
  opt.rtree.mbr_dim = 2; opt.rtree.split_policy = "quadratic";
  opt.create_if_missing = a.stage == "prepare"; return opt;
}
void VerifyRows(ltm::HybridIndex* index, const std::vector<ltm::IndexEntry>& rows) {
  Check(index->Validate());
  std::vector<bool> seen(rows.size(), false); uint64_t count = 0; bool good = true;
  ltm::Query q; q.kind = ltm::QueryKind::kRange1D; q.key_hi = std::numeric_limits<uint64_t>::max();
  Check(index->Query(q, [&](const ltm::IndexEntry& row) {
    if (row.key >= rows.size() || seen[static_cast<size_t>(row.key)] || !Same(row, rows[static_cast<size_t>(row.key)])) good = false;
    else seen[static_cast<size_t>(row.key)] = true;
    ++count; return true;
  }));
  Require(good && count == rows.size(), "full persisted record mismatch");
}
uint64_t Ns(Clock::duration duration) {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}
int Run(const Args& a) {
  const auto dir = std::filesystem::absolute(std::filesystem::u8path(a.directory));
  if (a.stage == "prepare") {
    Require(!std::filesystem::exists(dir / "pages.data") && !std::filesystem::exists(dir / "records.wal"), "refusing existing database");
    std::filesystem::create_directories(dir);
  } else Require(std::filesystem::is_regular_file(dir / "pages.data") && std::filesystem::is_regular_file(dir / "records.wal"), "missing database");
  auto rows = Dataset(a); auto queries = Queries(a, rows);
  uint64_t data_hash = kBasis, query_hash = kBasis, answer_hash = kBasis;
  for (const auto& row : rows) HashRow(row, &data_hash);
  for (const auto& q : queries) { HashDouble(q.probe[0], &query_hash); HashDouble(q.probe[1], &query_hash); }
  ltm::HybridIndex index; Check(index.Open(Options(a, dir)));
  if (a.stage == "prepare") {
    std::vector<uint64_t> order(static_cast<size_t>(a.records)); std::iota(order.begin(), order.end(), 0ull);
    std::mt19937_64 rng(a.seed);
    for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[static_cast<size_t>(rng() % i)]);
    for (uint64_t key : order) Check(index.Put(rows[static_cast<size_t>(key)]));
    VerifyRows(&index, rows); Check(index.Checkpoint());
  }
  // Tree page counts are populated by Validate, not persisted across Open.
  // Also verify the complete disk dataset before entering any measured interval.
  if (a.stage != "prepare") VerifyRows(&index, rows);
  const auto structure = index.stats();
  const auto full_pages = structure.bplus.num_pages + structure.rtree.num_pages + 1;
  const auto active_pages = structure.rtree.num_pages + 1;
  Require(a.regime == "resident" ? full_pages <= a.frames : active_pages > a.frames, "declared regime does not match tree page count");
  uint64_t wall_ns = 0, api_ns = 0, p50 = 0, p95 = 0, p99 = 0, page_reads = 0, evictions = 0;
  uint64_t page_writes = 0, wal_appends = 0, query_calls = 0, validated = 0;
  std::vector<uint64_t> counts(static_cast<size_t>(a.readers), 0);
  if (a.stage != "prepare") {
    const auto expected = Oracle(a, rows, queries);
    const auto validate_answer = [&](size_t id, const std::vector<ltm::IndexEntry>& result) {
      Require(result.size() == a.k, "wrong result size");
      for (size_t j = 0; j < result.size(); ++j)
        Require(Same(result[j], rows[static_cast<size_t>(expected[id][j])]), "exact (distance,key)/MBR/payload mismatch");
    };
    if (a.stage == "verify") {
      VerifyRows(&index, rows);
      for (size_t id = 0; id < queries.size(); ++id) {
        std::vector<ltm::IndexEntry> result; Check(index.Knn(queries[id], static_cast<uint32_t>(a.k), &result));
        validate_answer(id, result); for (const auto& row : result) HashRow(row, &answer_hash); ++validated;
      }
    } else {
      for (uint64_t id = 0; id < a.warmup; ++id) {
        std::vector<ltm::IndexEntry> result; Check(index.Knn(queries[static_cast<size_t>(id)], static_cast<uint32_t>(a.k), &result));
        validate_answer(static_cast<size_t>(id), result);
      }
      std::vector<std::vector<ltm::IndexEntry>> answers(queries.size());
      for (auto& result : answers) result.reserve(static_cast<size_t>(a.k));
      std::vector<uint64_t> durations(queries.size());
      std::vector<Clock::time_point> finished(static_cast<size_t>(a.readers));
      std::vector<std::string> errors(static_cast<size_t>(a.readers));
      std::vector<std::thread> workers; workers.reserve(static_cast<size_t>(a.readers));
      std::mutex mutex; std::condition_variable cv; uint64_t ready = 0; bool go = false;
      try {
        for (size_t worker = 0; worker < a.readers; ++worker) workers.emplace_back([&, worker] {
          { std::unique_lock<std::mutex> lock(mutex); ++ready; cv.notify_all(); cv.wait(lock, [&] { return go; }); }
          try {
            for (size_t id = worker; id < queries.size(); id += static_cast<size_t>(a.readers)) {
              const auto tick = Clock::now();
              const auto status = index.Knn(queries[id], static_cast<uint32_t>(a.k), &answers[id]);
              durations[id] = Ns(Clock::now() - tick); Check(status); ++counts[worker];
            }
          } catch (const std::exception& error) { errors[worker] = error.what(); }
          finished[worker] = Clock::now();
        });
      } catch (...) {
        { std::lock_guard<std::mutex> lock(mutex); go = true; } cv.notify_all();
        for (auto& worker : workers) worker.join(); throw;
      }
      ltm::IndexStats before;
      Clock::time_point start;
      { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return ready == a.readers; });
        before = index.stats(); start = Clock::now(); go = true; }
      cv.notify_all(); for (auto& worker : workers) worker.join();
      const auto after = index.stats();
      for (const auto& error : errors) Require(error.empty(), error);
      wall_ns = Ns(*std::max_element(finished.begin(), finished.end()) - start);
      for (size_t id = 0; id < queries.size(); ++id) {
        validate_answer(id, answers[id]); ++validated;
        for (const auto& row : answers[id]) HashRow(row, &answer_hash);
      }
      api_ns = std::accumulate(durations.begin(), durations.end(), uint64_t{0});
      std::sort(durations.begin(), durations.end());
      const auto percentile = [&](size_t n) { return durations[(durations.size() * n + 99) / 100 - 1]; };
      p50 = percentile(50); p95 = percentile(95); p99 = percentile(99);
      page_reads = after.io.page_reads - before.io.page_reads; evictions = after.pool.evictions - before.pool.evictions;
      page_writes = after.io.page_writes - before.io.page_writes; wal_appends = after.io.wal_appends - before.io.wal_appends;
      query_calls = after.queries - before.queries;
      Require(wall_ns > 0 && api_ns > 0 && query_calls == a.ops && after.writes == before.writes &&
        page_writes == 0 && wal_appends == 0 && after.pool.dirty_evictions == before.pool.dirty_evictions, "read-only interval invariant failed");
      Require(a.regime == "resident" ? page_reads == 0 && evictions == 0 : page_reads > 0 && evictions > 0,
        "measured residency/eviction condition failed");
      VerifyRows(&index, rows);
    }
  }
  Check(index.Close());
  std::cout << "KNN_RESULT {\"schema_version\":1,\"stage\":\"" << a.stage << "\",\"regime\":\"" << a.regime
    << "\",\"measurement_scope\":\"fixed_total_queries_deferred_validation\",\"records\":" << a.records
    << ",\"ops\":" << a.ops << ",\"warmup\":" << a.warmup << ",\"frames\":" << a.frames
    << ",\"readers\":" << a.readers << ",\"k\":" << a.k << ",\"seed\":" << a.seed
    << ",\"full_pages\":" << full_pages << ",\"active_pages\":" << active_pages
    << ",\"dataset_hash\":\"" << data_hash << "\",\"query_hash\":\"" << query_hash
    << "\",\"answer_hash\":\"" << answer_hash << "\",\"validated_queries\":" << validated
    << ",\"wall_ns\":" << wall_ns << ",\"api_sum_ns\":" << api_ns << ",\"p50_ns\":" << p50
    << ",\"p95_ns\":" << p95 << ",\"p99_ns\":" << p99 << ",\"page_reads\":" << page_reads
    << ",\"evictions\":" << evictions << ",\"page_writes\":" << page_writes
    << ",\"wal_appends\":" << wal_appends << ",\"query_calls\":" << query_calls << ",\"worker_counts\":[";
  for (size_t i = 0; i < counts.size(); ++i) std::cout << (i ? "," : "") << counts[i];
  std::cout << "],\"correct\":true}\n"; return 0;
}
}  // namespace
int main(int argc, char** argv) {
  Args args;
  if (!Parse(argc, argv, &args)) { std::cerr << "KNN_ERROR: invalid arguments\n"; return 2; }
  try { return Run(args); }
  catch (const std::exception& error) { std::cerr << "KNN_ERROR: " << error.what() << '\n'; return 1; }
}

// Fixed committed Hybrid workload; checkpoint placement is the only input variable.
// Prepare exits without Close or destructors after all commits are acknowledged.
// Recover times the existing storage recovery API; verification is a fresh process.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include "index/hybrid_index.h"
#include "storage/recovery.h"
#include "util/log.h"

namespace {
constexpr uint64_t kRecords = 512;
constexpr uint64_t kUpdates = 512;
constexpr uint32_t kFrames = 24;
struct Args { std::string stage, directory; uint64_t tail = kUpdates + 1; };
bool Check(const ltm::Status& s) {
  if (s.ok()) return true;
  std::fprintf(stderr, "RECOVERY_ERROR: %s\n", s.ToString().c_str()); return false;
}
bool Require(bool yes, const char* message) {
  if (yes) return true;
  std::fprintf(stderr, "RECOVERY_ERROR: %s\n", message); return false;
}
bool Parse(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s.rfind("--stage=", 0) == 0 && args->stage.empty()) args->stage = s.substr(8);
    else if (s.rfind("--run-dir=", 0) == 0 && args->directory.empty()) args->directory = s.substr(10);
    else if (s.rfind("--tail-updates=", 0) == 0 && args->tail == kUpdates + 1) {
      const auto value = s.substr(15);
      if (value.empty()) return false;
      uint64_t n = 0;
      for (char c : value) {
        if (c < '0' || c > '9' || n > (kUpdates - static_cast<unsigned>(c - '0')) / 10) return false;
        n = n * 10 + static_cast<unsigned>(c - '0');
      }
      if (n > kUpdates) return false;
      args->tail = n;
    } else return false;
  }
  return !args->directory.empty() && args->tail <= kUpdates &&
    (args->stage == "prepare" || args->stage == "recover" || args->stage == "verify");
}
ltm::IndexEntry Row(uint64_t key, uint64_t generation) {
  ltm::IndexEntry row; row.key = key;
  row.mbr.lo = ltm::Point(2); row.mbr.hi = ltm::Point(2);
  row.mbr.lo[0] = static_cast<double>(key % 23) * 4 + static_cast<double>(generation) / 8;
  row.mbr.lo[1] = static_cast<double>(key / 23) * 4 + static_cast<double>(generation) / 16;
  row.mbr.hi[0] = row.mbr.lo[0] + .75; row.mbr.hi[1] = row.mbr.lo[1] + .5;
  const auto payload = std::to_string(key) + ":recovery:" + std::to_string(generation);
  row.payload.assign(payload.begin(), payload.end()); return row;
}
void HashWord(uint64_t word, uint64_t* hash) {
  for (int i = 0; i < 8; ++i) { *hash ^= word & 255u; *hash *= 1099511628211ull; word >>= 8; }
}
void HashRow(const ltm::IndexEntry& row, uint64_t* hash) {
  HashWord(row.key, hash); HashWord(row.dim(), hash);
  for (uint16_t d = 0; d < row.dim(); ++d) {
    uint64_t lo = 0, hi = 0; const double low = row.mbr.lo[d], high = row.mbr.hi[d];
    std::memcpy(&lo, &low, sizeof(lo)); std::memcpy(&hi, &high, sizeof(hi));
    HashWord(lo, hash); HashWord(hi, hash);
  }
  HashWord(row.payload.size(), hash);
  for (char c : row.payload) { *hash ^= static_cast<unsigned char>(c); *hash *= 1099511628211ull; }
}
uint64_t ExpectedHash() {
  uint64_t hash = 14695981039346656037ull;
  for (uint64_t key = 0; key < kRecords; ++key) HashRow(Row(key, 1), &hash);
  return hash;
}
bool Same(const ltm::IndexEntry& a, const ltm::IndexEntry& b) {
  return a.key == b.key && a.payload == b.payload && a.mbr.lo.coords() == b.mbr.lo.coords() && a.mbr.hi.coords() == b.mbr.hi.coords();
}
ltm::HybridOptions Options(const std::filesystem::path& dir) {
  ltm::HybridOptions opt;
  opt.config.disk.page_file = (dir / "pages.data").u8string();
  opt.config.disk.wal_file = (dir / "records.wal").u8string();
  opt.config.disk.num_pages = 4096; opt.config.pool.pool_frames = kFrames;
  opt.config.pool.enable_prefetch = false;
  opt.bplus.order_or_capacity = 32; opt.rtree.order_or_capacity = 32;
  opt.rtree.mbr_dim = 2; opt.rtree.split_policy = "quadratic"; return opt;
}
void Identity(const Args& a) {
  std::cout << "RECOVERY_RESULT {\"schema_version\":1,\"stage\":\"" << a.stage
    << "\",\"engine\":\"hybrid\",\"records\":512,\"updates\":512,\"pool_frames\":24,\"page_size\":4096"
    << ",\"tail_updates\":" << a.tail << ",\"expected_hash\":\"" << ExpectedHash() << "\"";
}
int Run(const Args& a) {
  const auto dir = std::filesystem::absolute(std::filesystem::u8path(a.directory));
  const auto opt = Options(dir);
  if (a.stage == "prepare") {
    if (!Require(!std::filesystem::exists(dir / "pages.data") && !std::filesystem::exists(dir / "records.wal"), "refusing to overwrite an existing database")) return 2;
    std::filesystem::create_directories(dir);
    ltm::HybridIndex index;
    if (!Check(index.Open(opt))) return 1;
    uint64_t input_hash = 14695981039346656037ull;
    for (uint64_t key = 0; key < kRecords; ++key) {
      const auto row = Row(key, 0); HashRow(row, &input_hash);
      if (!Check(index.Put(row))) return 1;
    }
    const auto cut = kUpdates - a.tail;
    uint64_t checkpoint_us = 0, checkpoint_before_bytes = 0, checkpoint_kept_bytes = 0;
    for (uint64_t i = 0; i <= kUpdates; ++i) {
      if (i == cut) {
        checkpoint_before_bytes = index.disk()->WalSize();
        const auto start = std::chrono::steady_clock::now();
        if (!Check(index.Checkpoint())) return 1;
        checkpoint_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        checkpoint_kept_bytes = index.disk()->WalSize();
      }
      if (i < kUpdates) {
        const auto row = Row(i, 1); HashRow(row, &input_hash);
        if (!Check(index.Put(row))) return 1;
      }
    }
    // Commit normally forces WAL sync; this final sync makes the boundary explicit.
    if (!Check(index.disk()->SyncWal())) return 1;
    const auto stats = index.stats();
    if (!Require(stats.writes == kRecords + kUpdates && stats.pool.i6_violations == 0 && index.pool()->latch_stats().order_violations.load() == 0, "write count or invariant mismatch")) return 1;
    Identity(a);
    std::cout << ",\"committed_puts\":1024,\"input_hash\":\"" << input_hash << "\",\"wal_file_bytes\":" << index.disk()->WalSize()
      << ",\"checkpoint_before_bytes\":" << checkpoint_before_bytes << ",\"checkpoint_kept_bytes\":" << checkpoint_kept_bytes
      << ",\"checkpoint_us\":" << checkpoint_us << ",\"abrupt_exit_code\":9,\"i6_violations\":0,\"lock_violations\":0}\n" << std::flush;
    // A real process exit skips Close, FlushAll and all destructors.
    std::_Exit(9);
  }
  if (!Require(std::filesystem::is_regular_file(dir / "pages.data") && std::filesystem::is_regular_file(dir / "records.wal"), "existing database required")) return 2;
  if (a.stage == "recover") {
    const auto start = std::chrono::steady_clock::now();
    ltm::DiskManager disk(opt.config.disk);
    ltm::RecoveryManager recovery(opt.config.wal, &disk);
    ltm::BufferPool pool(opt.config.pool, &disk, recovery.wal(), &recovery);
    if (!Check(recovery.Recover(&pool))) return 1;
    const auto ready_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    const auto& r = recovery.report();
    if (!Require(!r.clean_shutdown && r.txns_undone == 0 && r.losers == 0, "unexpected clean shutdown or uncommitted transaction")) return 1;
    Identity(a);
    std::cout << ",\"previous_clean_shutdown\":false,\"recover_us\":" << r.elapsed_micros << ",\"storage_ready_us\":" << ready_us
      << ",\"wal_bytes\":" << r.wal_bytes << ",\"records_scanned\":" << r.records_scanned << ",\"pages_redone\":" << r.pages_redone
      << ",\"records_redone\":" << r.records_redone << ",\"records_skipped\":" << r.records_skipped
      << ",\"txns_seen\":" << r.txns_seen << ",\"txns_undone\":" << r.txns_undone
      << ",\"checkpoint_lsn\":" << r.checkpoint_lsn << ",\"redo_start_lsn\":" << r.redo_start_lsn
      << ",\"recovery_needed\":" << (r.recovery_needed ? "true" : "false") << ",\"storage_recovered\":true}\n";
    return 0;
  }
  ltm::HybridIndex index;
  if (!Check(index.Open(opt))) return 1;
  for (uint64_t key = 0; key < kRecords; ++key) {
    const auto hit = index.Get(key);
    if (!Check(hit.status()) || !Require(Same(hit.value(), Row(key, 1)), "full point record mismatch")) return 1;
  }
  uint64_t rows = 0, hash = 14695981039346656037ull; bool same = true;
  ltm::Query query; query.kind = ltm::QueryKind::kRange1D; query.key_lo = 0;
  query.key_hi = std::numeric_limits<uint64_t>::max();
  if (!Check(index.Query(query, [&](const ltm::IndexEntry& row) {
    same = same && rows < kRecords && Same(row, Row(rows, 1));
    HashRow(row, &hash); ++rows; return true;
  })) || !Require(same && rows == kRecords && hash == ExpectedHash(), "full scan mismatch") || !Check(index.Validate())) return 1;
  const auto stats = index.stats();
  if (!Require(stats.bplus.num_entries == kRecords && stats.rtree.num_entries == kRecords && stats.i8_pairs_checked == kRecords && stats.pool.i6_violations == 0 && index.pool()->latch_stats().order_violations.load() == 0, "dual-tree invariant mismatch") || !Check(index.Close())) return 1;
  Identity(a);
  std::cout << ",\"correct\":true,\"point_checked\":512,\"full_scan_rows\":" << rows << ",\"i8_pairs_checked\":" << stats.i8_pairs_checked
    << ",\"dataset_hash\":\"" << hash << "\",\"i6_violations\":0,\"lock_violations\":0}\n";
  return 0;
}
}
int main(int argc, char** argv) {
  ltm::Logger::Instance().SetLevel(ltm::LogLevel::kError);
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::puts("usage: ltm_recovery_bench --stage=prepare|recover|verify --run-dir=DIR --tail-updates=0..512"); return 0;
  }
  Args args;
  if (!Parse(argc, argv, &args)) return 2;
  try { return Run(args); }
  catch (const std::exception& e) { std::fprintf(stderr, "RECOVERY_ERROR: %s\n", e.what()); return 1; }
}

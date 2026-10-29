#include "util/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "util/log.h"

namespace ltm {
namespace {

std::string ToUpperSnake(const std::string& key) {
  std::string out;
  out.reserve(key.size() + 8);
  for (char c : key) {
    if (c == '.') {
      out.push_back('_');
    } else if (c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c - 'a' + 'A'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string Trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) {
    return "";
  }
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool ParseU64(const std::string& v, uint64_t* out) {
  if (v.empty()) {
    return false;
  }
  char* end = nullptr;
  unsigned long long r = std::strtoull(v.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *out = static_cast<uint64_t>(r);
  return true;
}

bool ParseBool(const std::string& v, bool* out) {
  std::string s;
  s.reserve(v.size());
  for (char c : v) {
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (s == "1" || s == "true" || s == "yes" || s == "on") {
    *out = true;
    return true;
  }
  if (s == "0" || s == "false" || s == "no" || s == "off") {
    *out = false;
    return true;
  }
  return false;
}

LogLevel ParseLogLevel(const std::string& v) {
  std::string s;
  for (char c : v) {
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (s == "debug") return LogLevel::kDebug;
  if (s == "warn" || s == "warning") return LogLevel::kWarn;
  if (s == "error") return LogLevel::kError;
  if (s == "off" || s == "none") return LogLevel::kOff;
  return LogLevel::kInfo;
}

const char* LogLevelName(LogLevel lv) {
  switch (lv) {
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
    case LogLevel::kOff: return "off";
  }
  return "info";
}

// 已知键数量：用于 Dump() 的完整输出
const char* const kAllKeys[] = {
    "disk.page_size", "disk.num_pages", "disk.page_file", "disk.wal_file",
    "disk.sync_on_write", "pool.pool_frames", "pool.clock_hand_ms",
    "pool.decay_interval_ms", "pool.cold_threshold", "pool.hot_delta",
    "pool.decay_shift", "pool.enable_prefetch", "pool.prefetch_depth",
    "pool.prefetch_weight_pct", "wal.group_commit_ms", "wal.group_commit_records",
    "wal.max_wal_bytes", "migration.clock_hand_ms", "migration.decay_interval_ms",
    "migration.cold_threshold", "migration.hot_delta", "migration.decay_shift",
    "migration.dirty_flush_high_water", "migration.nonleaf_evict_penalty",
    "log.level", "log.file", "bench.ops"};

}  // namespace

Config Config::Default() { return Config{}; }

Status Config::Set(const std::string& key, const std::string& value) {
  uint64_t num = 0;
  bool b = false;
  const bool is_num = ParseU64(value, &num);
  const bool is_bool = ParseBool(value, &b);

  // ---- disk ----
  if (key == "disk.page_size") {
    if (!is_num || num < 512 || num > (1u << 20)) return InvalidArgument("disk.page_size is invalid");
    disk.page_size = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "disk.num_pages") {
    if (!is_num) return InvalidArgument("disk.num_pages is invalid");
    disk.num_pages = num;
    return Status::Ok();
  }
  if (key == "disk.page_file") { disk.page_file = value; return Status::Ok(); }
  if (key == "disk.wal_file") { disk.wal_file = value; return Status::Ok(); }
  if (key == "disk.sync_on_write") {
    if (!is_bool) return InvalidArgument("disk.sync_on_write is invalid");
    disk.sync_on_write = b;
    return Status::Ok();
  }
  // ---- buffer pool ----
  if (key == "pool.pool_frames") {
    if (!is_num || num == 0) return InvalidArgument("pool.pool_frames is invalid");
    pool.pool_frames = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "pool.clock_hand_ms") {
    if (!is_num) return InvalidArgument("pool.clock_hand_ms is invalid");
    pool.clock_hand_ms = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "pool.decay_interval_ms") {
    if (!is_num) return InvalidArgument("pool.decay_interval_ms is invalid");
    pool.decay_interval_ms = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "pool.cold_threshold") {
    if (!is_num || num > 255) return InvalidArgument("pool.cold_threshold is invalid");
    pool.cold_threshold = static_cast<uint8_t>(num);
    return Status::Ok();
  }
  if (key == "pool.hot_delta") {
    if (!is_num || num > 255) return InvalidArgument("pool.hot_delta is invalid");
    pool.hot_delta = static_cast<uint8_t>(num);
    return Status::Ok();
  }
  if (key == "pool.decay_shift") {
    if (!is_num || num > 7) return InvalidArgument("pool.decay_shift is invalid");
    pool.decay_shift = static_cast<uint8_t>(num);
    return Status::Ok();
  }
  if (key == "pool.enable_prefetch") {
    if (!is_bool) return InvalidArgument("pool.enable_prefetch is invalid");
    pool.enable_prefetch = b;
    return Status::Ok();
  }
  if (key == "pool.prefetch_depth") {
    if (!is_num) return InvalidArgument("pool.prefetch_depth is invalid");
    pool.prefetch_depth = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "pool.prefetch_weight_pct") {
    if (!is_num || num > 100) return InvalidArgument("pool.prefetch_weight_pct is invalid");
    pool.prefetch_weight_pct = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  // ---- wal ----
  if (key == "wal.group_commit_ms") {
    if (!is_num) return InvalidArgument("wal.group_commit_ms is invalid");
    wal.group_commit_ms = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "wal.group_commit_records") {
    if (!is_num) return InvalidArgument("wal.group_commit_records is invalid");
    wal.group_commit_records = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "wal.max_wal_bytes") {
    if (!is_num) return InvalidArgument("wal.max_wal_bytes is invalid");
    wal.max_wal_bytes = num;
    return Status::Ok();
  }
  // ---- migration ----
  if (key == "migration.clock_hand_ms") {
    if (!is_num) return InvalidArgument("migration.clock_hand_ms is invalid");
    migration.clock_hand_ms = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "migration.decay_interval_ms") {
    if (!is_num) return InvalidArgument("migration.decay_interval_ms is invalid");
    migration.decay_interval_ms = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "migration.cold_threshold") {
    if (!is_num || num > 255) return InvalidArgument("migration.cold_threshold is invalid");
    migration.cold_threshold = static_cast<uint8_t>(num);
    return Status::Ok();
  }
  if (key == "migration.hot_delta") {
    if (!is_num || num > 255) return InvalidArgument("migration.hot_delta is invalid");
    migration.hot_delta = static_cast<uint8_t>(num);
    return Status::Ok();
  }
  if (key == "migration.decay_shift") {
    if (!is_num || num > 7) return InvalidArgument("migration.decay_shift is invalid");
    migration.decay_shift = static_cast<uint8_t>(num);
    return Status::Ok();
  }
  if (key == "migration.dirty_flush_high_water") {
    if (!is_num) return InvalidArgument("migration.dirty_flush_high_water is invalid");
    migration.dirty_flush_high_water = static_cast<uint32_t>(num);
    return Status::Ok();
  }
  if (key == "migration.nonleaf_evict_penalty") {
    if (!is_num) return InvalidArgument("migration.nonleaf_evict_penalty is invalid");
    migration.nonleaf_evict_penalty = static_cast<int32_t>(num);
    return Status::Ok();
  }
  // ---- log / bench ----
  if (key == "log.level") { log_level = ParseLogLevel(value); return Status::Ok(); }
  if (key == "log.file") { log_file = value; return Status::Ok(); }
  if (key == "bench.ops") {
    if (!is_num || num == 0) return InvalidArgument("bench.ops is invalid");
    bench_ops = num;
    return Status::Ok();
  }
  return InvalidArgument("unknown config key: " + key);
}

std::string Config::Get(const std::string& key) const {
  auto s = [](uint64_t v) { return std::to_string(v); };
  if (key == "disk.page_size") return s(disk.page_size);
  if (key == "disk.num_pages") return s(disk.num_pages);
  if (key == "disk.page_file") return disk.page_file;
  if (key == "disk.wal_file") return disk.wal_file;
  if (key == "disk.sync_on_write") return disk.sync_on_write ? "true" : "false";
  if (key == "pool.pool_frames") return s(pool.pool_frames);
  if (key == "pool.clock_hand_ms") return s(pool.clock_hand_ms);
  if (key == "pool.decay_interval_ms") return s(pool.decay_interval_ms);
  if (key == "pool.cold_threshold") return s(pool.cold_threshold);
  if (key == "pool.hot_delta") return s(pool.hot_delta);
  if (key == "pool.decay_shift") return s(pool.decay_shift);
  if (key == "pool.enable_prefetch") return pool.enable_prefetch ? "true" : "false";
  if (key == "pool.prefetch_depth") return s(pool.prefetch_depth);
  if (key == "pool.prefetch_weight_pct") return s(pool.prefetch_weight_pct);
  if (key == "wal.group_commit_ms") return s(wal.group_commit_ms);
  if (key == "wal.group_commit_records") return s(wal.group_commit_records);
  if (key == "wal.max_wal_bytes") return s(wal.max_wal_bytes);
  if (key == "migration.clock_hand_ms") return s(migration.clock_hand_ms);
  if (key == "migration.decay_interval_ms") return s(migration.decay_interval_ms);
  if (key == "migration.cold_threshold") return s(migration.cold_threshold);
  if (key == "migration.hot_delta") return s(migration.hot_delta);
  if (key == "migration.decay_shift") return s(migration.decay_shift);
  if (key == "migration.dirty_flush_high_water") return s(migration.dirty_flush_high_water);
  if (key == "migration.nonleaf_evict_penalty") return std::to_string(migration.nonleaf_evict_penalty);
  if (key == "log.level") return LogLevelName(log_level);
  if (key == "log.file") return log_file;
  if (key == "bench.ops") return s(bench_ops);
  return "<unknown key>";
}

std::string Config::Dump() const {
  std::ostringstream os;
  for (const char* k : kAllKeys) {
    os << k << " = " << Get(k) << '\n';
  }
  return os.str();
}

Status Config::LoadFile(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    return Status::Error(StatusCode::kIoError, "cannot open the config file: " + path);
  }
  std::string line;
  int lineno = 0;
  while (std::getline(ifs, line)) {
    ++lineno;
    std::string t = Trim(line);
    if (t.empty() || t[0] == '#') {
      continue;
    }
    size_t eq = t.find('=');
    if (eq == std::string::npos) {
      return InvalidArgument("config file line " + std::to_string(lineno) + " is missing '=': " + t);
    }
    std::string k = Trim(t.substr(0, eq));
    std::string v = Trim(t.substr(eq + 1));
    Status s = Set(k, v);
    if (!s.ok()) {
      return Status::Error(s.code(), "config file line " + std::to_string(lineno) + " line: " + s.message());
    }
  }
  return Status::Ok();
}

void Config::ApplyEnvironment() {
  for (const char* k : kAllKeys) {
    std::string env = "LTM_" + ToUpperSnake(k);
    const char* raw = std::getenv(env.c_str());
    if (raw == nullptr) {
      continue;
    }
    Status s = Set(k, std::string(raw));
    if (!s.ok()) {
      LOG_WARN("environment variable ", env, " has an invalid value and was ignored: ", s.message());
    }
  }
}

StatusOr<Config> Config::FromCommandLine(int argc, char** argv) {
  Config cfg = Config::Default();
  cfg.ApplyEnvironment();

  // 第一遍：只找 --config=，因为它的优先级低于显式 --key=value
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--config=", 0) == 0) {
      Status s = cfg.LoadFile(a.substr(9));
      if (!s.ok()) {
        return s;
      }
    }
  }
  // 第二遍：显式键值覆盖
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--", 0) != 0 || a.rfind("--config=", 0) == 0) {
      continue;
    }
    size_t eq = a.find('=');
    if (eq == std::string::npos) {
      continue;  // 形如 --list / --verbose 的开关由调用者自行处理
    }
    std::string k = a.substr(2, eq - 2);
    std::string v = a.substr(eq + 1);
    Status s = cfg.Set(k, v);
    if (!s.ok()) {
      return s;
    }
  }
  return cfg;
}

}  // namespace ltm

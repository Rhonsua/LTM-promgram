// tests/index/test_bplus_validate.cc
// M2 专用验收入口：B+ 树不变量全量自检（由 scripts/validate_tree.py 驱动）。
//
// 用法：
//   ltm_validate_bplus                 跑内建负载后自检
//   ltm_validate_bplus --ops 200000    指定操作数
//   ltm_validate_bplus --capacity 32   指定阶数
//   ltm_validate_bplus --frames 64     指定缓冲池帧数（故意设小可逼出淘汰路径）
//
// 退出码：0 = 通过；1 = 校验失败；2 = 尚未实现（本树已实现，因此不再返回 2）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "util/log.h"

namespace {

struct Args {
  uint64_t ops = 20000;
  uint16_t capacity = 32;
  uint32_t frames = 128;
  uint64_t seed = 42;
  bool quiet = false;
};

Args Parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto num = [&s](const char* prefix) -> uint64_t {
      return std::strtoull(s.c_str() + std::strlen(prefix), nullptr, 10);
    };
    if (s.rfind("--ops=", 0) == 0) {
      a.ops = num("--ops=");
    } else if (s.rfind("--capacity=", 0) == 0) {
      a.capacity = static_cast<uint16_t>(num("--capacity="));
    } else if (s.rfind("--frames=", 0) == 0) {
      a.frames = static_cast<uint32_t>(num("--frames="));
    } else if (s.rfind("--seed=", 0) == 0) {
      a.seed = num("--seed=");
    } else if (s == "--quiet") {
      a.quiet = true;
    }
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = Parse(argc, argv);
  ltm::Logger::Instance().SetLevel(args.quiet ? ltm::LogLevel::kError : ltm::LogLevel::kWarn);

  const std::string page_file = "log/_validate_bplus.data";
  const std::string wal_file = "log/_validate_bplus.wal";
#if defined(_WIN32)
  std::system("if not exist log mkdir log");
#else
  std::system("mkdir -p log");
#endif
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());

  // 在池、WAL 和磁盘析构后才清理文件；失败时保留现场。
  struct SuccessfulFileCleanup {
    const std::string& data;
    const std::string& log;
    bool completed = false;
    ~SuccessfulFileCleanup() {
      if (completed) {
        std::remove(data.c_str());
        std::remove(log.c_str());
      }
    }
  } cleanup{page_file, wal_file};

  ltm::DiskOptions dopt;
  dopt.page_file = page_file;
  dopt.wal_file = wal_file;
  dopt.num_pages = 1u << 18;
  ltm::DiskManager disk(dopt);
  ltm::WalOptions wopt;
  ltm::WalWriter wal(wopt, &disk);
  ltm::BufferPoolOptions popt;
  popt.pool_frames = args.frames;
  ltm::BufferPool pool(popt, &disk, &wal, nullptr, nullptr, nullptr);

  ltm::BPlusPageOps ops(args.capacity);
  ltm::BPlusTree tree;
  ltm::TreeBuildOptions topt;
  topt.order_or_capacity = args.capacity;

  ltm::Status s = tree.Open(&pool, &ops, topt, /*tree_id=*/1);
  if (s.code() == ltm::StatusCode::kNotImplemented) {
    std::printf("the B+ tree is not implemented yet (%s)\n", s.ToString().c_str());
    return 2;
  }
  if (!s.ok()) {
    std::printf("BPlusTree::Open failed: %s\n", s.ToString().c_str());
    return 1;
  }

  // 负载：随机插入/删除，同时维护真值表用于最终比对
  std::map<uint64_t, std::string> truth;
  std::mt19937_64 rng(args.seed);
  const uint64_t key_space = args.ops;  // 键空间与操作数同阶，制造大量重复与删除
  std::uniform_int_distribution<uint64_t> key_dist(0, key_space);
  uint64_t inserts = 0;
  uint64_t erases = 0;

  for (uint64_t i = 0; i < args.ops; ++i) {
    const uint64_t k = key_dist(rng);
    if ((rng() % 100) < 60 || truth.empty()) {
      std::string p(6 + (rng() % 30), 'x');
      truth[k] = p;
      ltm::IndexEntry e;
      e.key = k;
      e.mbr = ltm::Mbr::Invalid(0);
      e.payload.assign(p.begin(), p.end());
      const ltm::Status r = tree.Insert(e);
      if (!r.ok()) {
        std::printf("Insert failed at step %llu: %s\n", static_cast<unsigned long long>(i),
                    r.ToString().c_str());
        return 1;
      }
      ++inserts;
    } else {
      auto it = truth.lower_bound(k);
      if (it == truth.end()) {
        it = truth.begin();
      }
      const uint64_t victim = it->first;
      truth.erase(it);
      ltm::IndexEntry e;
      e.key = victim;
      const ltm::Status r = tree.Erase(e);
      if (!r.ok()) {
        std::printf("Erase failed at step %llu: %s\n", static_cast<unsigned long long>(i),
                    r.ToString().c_str());
        return 1;
      }
      ++erases;
    }
  }

  // 全量比对
  std::vector<std::pair<uint64_t, std::string>> got;
  ltm::Status scan = tree.ScanAll([&got](const ltm::RecordRef& r) {
    got.emplace_back(r.key, std::string(r.payload, r.payload_len));
    return true;
  });
  if (!scan.ok()) {
    std::printf("ScanAll failed: %s\n", scan.ToString().c_str());
    return 1;
  }
  if (got.size() != truth.size()) {
    std::printf("entry counts differ: in tree %zu, truth table %zu\n", got.size(), truth.size());
    return 1;
  }
  size_t i = 0;
  for (const auto& kv : truth) {
    if (got[i].first != kv.first || got[i].second != kv.second) {
      std::printf("entry %zu differs: in tree (key=%llu, len=%zu), truth (key=%llu, len=%zu)\n", i,
                  static_cast<unsigned long long>(got[i].first), got[i].second.size(),
                  static_cast<unsigned long long>(kv.first), kv.second.size());
      return 1;
    }
    ++i;
  }

  // 不变量自检
  s = tree.Validate();
  if (!s.ok()) {
    std::printf("invariant validation failed: %s\n", s.ToString().c_str());
    return 1;
  }

  const ltm::TreeStats st = tree.stats();
  std::printf("B+ tree validation passed\n");
  std::printf("  params   : ops=%llu capacity=%u frames=%u seed=%llu\n",
              static_cast<unsigned long long>(args.ops), args.capacity, args.frames,
              static_cast<unsigned long long>(args.seed));
  std::printf("  size     : height=%u pages=%llu entries=%llu (inserts %llu / erases %llu)\n", st.height,
              static_cast<unsigned long long>(st.num_pages),
              static_cast<unsigned long long>(st.num_entries),
              static_cast<unsigned long long>(inserts),
              static_cast<unsigned long long>(erases));
  std::printf("  structure: splits %llu / merges %llu / borrows %llu, avg fill %.1f%%\n",
              static_cast<unsigned long long>(st.num_splits),
              static_cast<unsigned long long>(st.num_merges),
              static_cast<unsigned long long>(st.num_borrows), st.avg_fill_rate * 100.0);
  std::printf("  buffer   : hit rate %.1f%%, evictions %llu (dirty %llu)\n", pool.hit_rate() * 100.0,
              static_cast<unsigned long long>(pool.stats().evictions),
              static_cast<unsigned long long>(pool.stats().dirty_evictions));
  std::printf("  I/O      : reads %llu pages / writes %llu pages\n",
              static_cast<unsigned long long>(disk.stats().page_reads),
              static_cast<unsigned long long>(disk.stats().page_writes));
  // 采集侧的四条"漏洞"计数必须单独打出来：I6 violations == 0 只说明"被写盘的页
  // 都带了 LSN"，如果某次标脏发生在事务之外、而那个页恰好整轮都没被刷出去，
  // 违例计数就不会响。压测（phase 5）要把这几条也当判据，否则会漏掉整类缺口。
  std::printf("  WAL leak : outside-txn %llu, missing-before %llu, identical-skipped %llu, "
              "unstamped-writes %llu\n",
              static_cast<unsigned long long>(pool.stats().deltas_outside_txn),
              static_cast<unsigned long long>(pool.stats().deltas_missing_before),
              static_cast<unsigned long long>(pool.stats().deltas_skipped_identical),
              static_cast<unsigned long long>(pool.stats().i6_unstamped_writes));
  std::printf("  WAL      : deltas %llu, LSN stamps %llu (missed %llu), sync-on-flush %llu, "
              "I6 violations %llu, page bytes %llu vs changed-byte range %llu (%.1fx)\n",
              static_cast<unsigned long long>(pool.stats().wal_deltas),
              static_cast<unsigned long long>(pool.stats().lsn_stamps),
              static_cast<unsigned long long>(pool.stats().lsn_stamps_missed),
              static_cast<unsigned long long>(pool.stats().wal_sync_on_flush),
              static_cast<unsigned long long>(pool.stats().i6_violations),
              static_cast<unsigned long long>(pool.stats().delta_bytes_full),
              static_cast<unsigned long long>(pool.stats().delta_bytes_changed),
              pool.stats().delta_bytes_changed > 0
                  ? static_cast<double>(pool.stats().delta_bytes_full) /
                        static_cast<double>(pool.stats().delta_bytes_changed)
                  : 0.0);
  std::printf("  checks   : I1 parent key == child min key, I2 leaf chain bidirectional and ascending, I3 fill >= half capacity, I4 equal path lengths\n");

  cleanup.completed = true;
  return 0;
}

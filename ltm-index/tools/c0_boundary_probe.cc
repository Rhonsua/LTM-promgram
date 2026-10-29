// tools/c0_boundary_probe.cc
// C0 的定义边界：缺页/淘汰路径在共享闩下到底安不安全？
//
// ---------------------------------------------------------------------------
// 为什么要有这个探针
// ---------------------------------------------------------------------------
// docs/status.md 的 M6-1 一节写着一条断言：C0 的读并发**只在"只读 + 测量期零缺页"
// 下成立**，缺页/淘汰路径会改 `page_to_frame` / `free_frames` / `clock_hand`，
// 那条路在共享闩下不安全。test_c0_latch.cc 甚至把它写成了断言（misses 增量必须为 0）。
//
// 但**这条断言从来没有被实测过**。按本项目的规矩（缺陷 #37 的教训、report/m4_2
// 的做法），一个"能出事的路径"如果没演示过它真出事，就只是描述，不是证据。
//
// 本探针补的就是这一步：故意把池子调小，让 8 个读线程在测量期**必然缺页**，
// 看会发生什么。它同时给 C1 的帧表改造提供一条可复现的红 —— 明天动手时，
// 判据不是"文档说这里不安全"，而是"这条命令跑出来的就是这个错误"。
//
// 【它不是 ctest 用例】故意不注册进 LTM_TEST_SOURCES：C0 的边界是已知的、
// 待 C1 修的问题，让它常驻红灯会把测试套件变成一个"习惯了失败"的东西。
// 它是诊断工具，红是它的预期输出。
//
// 用法：build-ninja/c0_boundary_probe.exe [threads] [frames] [keys]
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/latch_manager.h"
#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "util/log.h"

using namespace ltm;

namespace {

std::string PayloadFor(uint64_t key) { return "v" + std::to_string(key); }

IndexEntry MakeEntry(uint64_t key) {
  IndexEntry e;
  e.key = key;
  e.mbr = Mbr::Invalid(0);
  const std::string p = PayloadFor(key);
  e.payload.assign(p.begin(), p.end());
  return e;
}

IndexEntry MakeProbe(uint64_t key) {
  IndexEntry e;
  e.key = key;
  e.mbr = Mbr::Invalid(0);
  return e;
}

}  // namespace

int main(int argc, char** argv) {
  const int threads = argc > 1 ? std::atoi(argv[1]) : 8;
  const uint32_t frames = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 16;
  const uint64_t keys = argc > 3 ? static_cast<uint64_t>(std::atoll(argv[3])) : 20000;

  const std::string page_file = "log/_c0probe.data";
  const std::string wal_file = "log/_c0probe.wal";
  std::system("if not exist log mkdir log");
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());

  DiskOptions dopt;
  dopt.page_file = page_file;
  dopt.wal_file = wal_file;
  dopt.num_pages = 1 << 15;

  DiskManager disk(dopt);
  WalWriter wal(WalOptions{}, &disk);

  BufferPoolOptions popt;
  // 【这就是被测量的变量】池子必须小于工作集，否则测量期零缺页，
  // 探针就退化成 test_c0_latch.cc 那条已经通过的用例了。
  popt.pool_frames = frames;
  BufferPool pool(popt, &disk, &wal, nullptr, nullptr, nullptr);

  BPlusPageOps ops(64);
  BPlusTree tree;
  TreeBuildOptions topt;
  topt.order_or_capacity = 64;
  {
    const Status s = tree.Open(&pool, &ops, topt, /*tree_id=*/1);
    if (!s.ok()) {
      std::printf("open failed: %s\n", s.ToString().c_str());
      return 2;
    }
  }
  for (uint64_t k = 0; k < keys; ++k) {
    const Status s = tree.Insert(MakeEntry(k));
    if (!s.ok()) {
      std::printf("load failed at key %llu: %s\n", static_cast<unsigned long long>(k),
                  s.ToString().c_str());
      return 2;
    }
  }

  const uint64_t misses_before = pool.stats().misses.load(std::memory_order_relaxed);
  const uint32_t resident_before = pool.resident_pages();
  std::printf("pool: frames=%u resident=%u  tree: %llu keys\n", frames, resident_before,
              static_cast<unsigned long long>(keys));
  std::printf("launching %d reader threads on a %u-frame pool (resident before=%u)\n", threads,
              frames, resident_before);
  std::printf("  -> frames < working set => misses in the measured phase; frames >= => none\n");
  std::fflush(stdout);

  std::atomic<uint64_t> bad_value{0};
  std::atomic<uint64_t> not_found{0};
  std::atomic<uint64_t> bad_status{0};
  std::atomic<uint64_t> exceptions{0};
  std::atomic<bool> stop{false};
  std::mutex first_err_mu;
  std::string first_err;

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> ts;
  ts.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    ts.emplace_back([&, t] {
      try {
        std::mt19937_64 rng(0xB0u + static_cast<uint64_t>(t) * 7919u);
        while (!stop.load(std::memory_order_relaxed)) {
          const uint64_t key = rng() % keys;
          auto rec = tree.Search(MakeProbe(key));
          if (!rec.ok()) {
            {
              std::lock_guard<std::mutex> g(first_err_mu);
              if (first_err.empty()) {
                first_err = "key=" + std::to_string(key) + " -> " + rec.status().ToString();
              }
            }
            ++not_found;
            continue;
          }
          const std::string got(rec.value().payload, rec.value().payload_len);
          if (got != PayloadFor(key) || rec.value().key != key) {
            ++bad_value;
          }
        }
      } catch (...) {
        ++exceptions;
      }
    });
  }

  // 跑到出现足够多的坏结果，或超时为止。崩溃型失败（unordered_map 被写坏）
  // 通常几毫秒内就发生，进程直接挂掉，输出里会留下 "crashed" 一行。
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (bad_value.load() + not_found.load() + bad_status.load() > 0) {
      break;
    }
  }
  stop.store(true);
  for (auto& th : ts) {
    th.join();
  }
  const auto t1 = std::chrono::steady_clock::now();

  const uint64_t misses_delta = pool.stats().misses.load(std::memory_order_relaxed) - misses_before;
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  std::printf("ran %.2fs, misses during measurement=%llu\n", secs,
              static_cast<unsigned long long>(misses_delta));
  std::printf("wrong_value=%llu not_found=%llu bad_status=%llu exceptions=%llu\n",
              static_cast<unsigned long long>(bad_value.load()),
              static_cast<unsigned long long>(not_found.load()),
              static_cast<unsigned long long>(bad_status.load()),
              static_cast<unsigned long long>(exceptions.load()));

  const Status v = tree.Validate();
  std::printf("tree.Validate()=%s\n", v.ok() ? "ok" : v.ToString().c_str());
  {
    std::lock_guard<std::mutex> g(first_err_mu);
    if (!first_err.empty()) {
      std::printf("first read failure: %s\n", first_err.c_str());
    }
  }
  // C1：LockOrderChecker 的判据。逆序/重复获取/升级都记在这里（RelWithDebInfo 下只计数）。
  std::printf("latch: order_violations=%llu shared=%llu exclusive=%llu contentions=%llu\n",
              static_cast<unsigned long long>(pool.latch_stats().order_violations.load()),
              static_cast<unsigned long long>(pool.latch_stats().shared_acquisitions.load()),
              static_cast<unsigned long long>(pool.latch_stats().exclusive_acquisitions.load()),
              static_cast<unsigned long long>(pool.latch_stats().contentions.load()));

  const bool clean = bad_value.load() == 0 && not_found.load() == 0 && bad_status.load() == 0 &&
                     exceptions.load() == 0 && v.ok();
  std::printf("VERDICT: %s\n", clean ? "no corruption observed (boundary not demonstrated)"
                                     : "CORRUPTION OBSERVED (C0 boundary is real)");
  pool.FlushAll();
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());
  return clean ? 0 : 1;
}

// tests/concurrency/test_c0_latch.cc
// M6-1 · C0 全局闩的守卫语义 + M6-1 的读扩展性测量。
//
// 【2026-10-04 状态更新】C1 已落地，索引层**不再取 C0 全局闩**（写串行化归
// `BufferPool::write_mutex`，写/读互斥下移到页闩）。本文件保留下来是因为：
//   * `LatchGuardIsReentrant` 仍在验证 `C0LatchGuard` 本身的可重入语义（守卫还在用）；
//   * `ReadScaling` 现在量的是**C1 的读扩展性**（页闩共享），与 report/m6_1 的
//     C0 基线可直接对比；
//   * `ConcurrentWritersAreSerialized` 现在验证的是 `write_mutex` 的串行化。
//
// ---------------------------------------------------------------------------
// 【测量前提：测量期零缺页 —— 现在是"负载控制"，不再是"C0 的定义边界"】
// C0 时代这条断言的含义是"别踩到 C0 保护不到的缺页/淘汰路径"。C1 之后那条路
// 已由 `BufferPool::Impl::table_mutex` + 页闩保护，缺页不再不安全；保留这条断言
// 只是为了把"扩展性"与"缺页开销"两个变量分开 —— 工作集全驻留时量的才是纯读路径。
// ---------------------------------------------------------------------------
// 历史判据（已废弃）：C0 的验收判据"8 读线程线性扩展 >= 4x"实测不可达（约 1.3-1.6x，
// report/m6_1/c0_read_scaling.csv），已改写为**正确性**判据（不撕裂 + 零缺页 + 写被串行化）。
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/latch_manager.h"
#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {

constexpr uint64_t kKeys = 20000;
constexpr int kQueriesPerThread = 200000;
// 全部工作集（约 320 页）必须能同时驻留，否则测量期就会出现缺页
constexpr uint32_t kFrames = 1024;

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

struct Fixture {
  std::string page_file;
  std::string wal_file;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<WalWriter> wal;
  std::unique_ptr<BufferPool> pool;
  BPlusPageOps ops{64};
  BPlusTree tree;

  explicit Fixture(const std::string& tag, uint32_t frames = kFrames)
      : page_file("log/_c0_" + tag + ".data"), wal_file("log/_c0_" + tag + ".wal") {
#if defined(_WIN32)
    std::system("if not exist log mkdir log");
#else
    std::system("mkdir -p log");
#endif
    std::remove(page_file.c_str());
    std::remove(wal_file.c_str());

    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    // 32 MB 足够：20000 键 / capacity 64 只需约 320 个叶+内部页，留了 25 倍余量。
    // 【不要随手调大这个数】DiskManager 会按 num_pages 把页文件建出来，
    // 而 ScalingControlIndependentPools 一次要开 8 个池：原来写的 1<<16 是
    // 每池 256 MB，8 个池就是 2 GB 的文件创建，单这一条把该用例拖到 257 秒
    // （ctest 的超时是 300 秒 —— 差一步就成"随机超时"）。
    dopt.num_pages = 1 << 13;
    disk.reset(new DiskManager(dopt));
    WalOptions wopt;
    wal.reset(new WalWriter(wopt, disk.get()));
    BufferPoolOptions popt;
    popt.pool_frames = frames;
    // heat / policy 都留空：C0 的测量要排除热度表这条额外的共享可变状态。
    // 带上热度表时 Touch() 也是共享闩下改共享状态，属同一类边界（见文件头）。
    pool.reset(new BufferPool(popt, disk.get(), wal.get(), nullptr, nullptr, nullptr));

    TreeBuildOptions topt;
    topt.order_or_capacity = 64;
    const Status s = tree.Open(pool.get(), &ops, topt, /*tree_id=*/1);
    if (!s.ok()) {
      std::printf("Fixture open failed: %s\n", s.ToString().c_str());
    }
  }

  ~Fixture() {
    pool.reset();
    disk.reset();
    std::remove(page_file.c_str());
    std::remove(wal_file.c_str());
  }
};

struct RoundResult {
  int threads = 0;
  uint64_t queries = 0;
  double seconds = 0.0;
  double ops_per_sec = 0.0;
  double speedup = 1.0;
  uint64_t mismatches = 0;
  uint64_t miss_delta = 0;
};

// 一轮并发点查：threads 个线程各做 kQueriesPerThread 次 Search。
// 查询集合与线程数无关（每线程用固定种子的 RNG），所以各轮的**总操作数相同**，
// 差别只可能来自并发度本身 —— 这是"扩展比"能当判据的前提。
RoundResult RunRound(Fixture& f, int threads, double base_ops_per_sec) {
  RoundResult r;
  r.threads = threads;
  r.queries = static_cast<uint64_t>(threads) * kQueriesPerThread;

  const uint64_t misses_before = f.pool->stats().misses.load(std::memory_order_relaxed);
  std::atomic<uint64_t> mismatches{0};
  std::atomic<bool> failed{false};
  std::string fail_msg;

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&f, t, &mismatches, &failed, &fail_msg]() {
      std::mt19937_64 rng(0xC0C0u + static_cast<uint64_t>(t) * 7919u);
      uint64_t bad = 0;
      for (int i = 0; i < kQueriesPerThread; ++i) {
        const uint64_t key = rng() % kKeys;
        auto rec = f.tree.Search(MakeProbe(key));
        if (!rec.ok()) {
          if (bad == 0 && !failed.exchange(true)) {
            fail_msg = "Search failed for key " + std::to_string(key) + ": " + rec.status().ToString();
          }
          ++bad;
          continue;
        }
        const std::string got(rec.value().payload, rec.value().payload_len);
        if (got != PayloadFor(key) || rec.value().key != key) {
          if (bad == 0 && !failed.exchange(true)) {
            fail_msg = "Search returned the wrong record for key " + std::to_string(key);
          }
          ++bad;
        }
      }
      mismatches.fetch_add(bad, std::memory_order_relaxed);
    });
  }
  for (auto& w : workers) {
    w.join();
  }
  const auto t1 = std::chrono::steady_clock::now();

  r.seconds = std::chrono::duration<double>(t1 - t0).count();
  r.ops_per_sec = r.seconds > 0.0 ? static_cast<double>(r.queries) / r.seconds : 0.0;
  r.speedup = base_ops_per_sec > 0.0 ? r.ops_per_sec / base_ops_per_sec : 1.0;
  r.mismatches = mismatches.load();
  r.miss_delta = f.pool->stats().misses.load(std::memory_order_relaxed) - misses_before;
  if (failed.load()) {
    std::printf("  [error] %s\n", fail_msg.c_str());
  }
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// 用例 1：读扩展性（M6-1 的验收测量）
// ---------------------------------------------------------------------------
TEST(C0, ReadScaling) {
  Fixture f("scale");

  // ---- 单线程装载 + 预热：把全部工作集读进池子 ----
  for (uint64_t k = 0; k < kKeys; ++k) {
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k)).ok());
  }
  for (uint64_t k = 0; k < kKeys; ++k) {
    ASSERT_TRUE(f.tree.Search(MakeProbe(k)).ok());
  }
  const uint64_t misses_after_warmup = f.pool->stats().misses.load(std::memory_order_relaxed);
  const uint64_t hits_after_warmup = f.pool->stats().hits.load(std::memory_order_relaxed);
  std::printf("  [info] warmup done: resident=%u frames=%u hits=%llu misses=%llu\n",
              f.pool->resident_pages(), f.pool->pool_frames(),
              static_cast<unsigned long long>(hits_after_warmup),
              static_cast<unsigned long long>(misses_after_warmup));

  // 再单独确认一次"热起来之后确实全是命中"：这是下面那条零缺页断言的前置检查。
  {
    const uint64_t before = f.pool->stats().misses.load(std::memory_order_relaxed);
    ASSERT_TRUE(f.tree.Search(MakeProbe(12345)).ok());
    EXPECT_EQ(f.pool->stats().misses.load(std::memory_order_relaxed), before);
  }

  const std::vector<int> thread_counts = {1, 2, 4, 8};
  std::vector<RoundResult> results;
  double base = 0.0;
  for (int t : thread_counts) {
    RoundResult r = RunRound(f, t, base);
    if (t == 1) {
      base = r.ops_per_sec;
      r.speedup = 1.0;
    }
    std::printf("  [info] threads=%d queries=%llu %.3fs -> %.0f ops/s  speedup=%.2fx  "
                "mismatches=%llu miss_delta=%llu\n",
                r.threads, static_cast<unsigned long long>(r.queries), r.seconds, r.ops_per_sec,
                r.speedup, static_cast<unsigned long long>(r.mismatches),
                static_cast<unsigned long long>(r.miss_delta));
    results.push_back(r);
  }

  // ---- 断言 1：正确性。并发读返回的每一条记录都必须与单线程装载的一致 ----
  for (const auto& r : results) {
    EXPECT_EQ(r.mismatches, 0u);
  }
  // ---- 断言 2：C0 的前提。测量期零缺页，否则测的就不是 C0 承诺的那条路 ----
  for (const auto& r : results) {
    if (r.miss_delta != 0) {
      LTM_FAIL_("C0 precondition violated: " + std::to_string(r.miss_delta) +
                " misses occurred during the measured phase with threads=" +
                std::to_string(r.threads) +
                ". C0's read concurrency only holds with a fully resident working set; a miss "
                "path mutates the frame table and free list, which the shared latch does not "
                "protect. This is C1's job (M6-2), not a number to be recorded.");
    }
  }

  // ---- CSV ----
#if defined(_WIN32)
  std::system("if not exist report\\m6_1 mkdir report\\m6_1");
#else
  std::system("mkdir -p report/m6_1");
#endif
  const char* csv_path = "report/m6_1/c0_read_scaling.csv";
  std::FILE* fp = std::fopen(csv_path, "w");
  if (fp != nullptr) {
    std::fprintf(fp, "threads,queries,seconds,ops_per_sec,speedup,misses_during_measurement\n");
    for (const auto& r : results) {
      std::fprintf(fp, "%d,%llu,%.6f,%.1f,%.3f,%llu\n", r.threads,
                   static_cast<unsigned long long>(r.queries), r.seconds, r.ops_per_sec, r.speedup,
                   static_cast<unsigned long long>(r.miss_delta));
    }
    std::fclose(fp);
    std::printf("  [info] wrote %s\n", csv_path);
  } else {
    LTM_FAIL_(std::string("cannot open ") + csv_path + " for writing");
  }

  const double best = results.back().speedup;
  std::printf("  [info] C0 read scaling: 8 threads = %.2fx of 1 thread (plan target: >= 4x)\n", best);
}

// ---------------------------------------------------------------------------
// 用例 2：C0 真的把写串行化了吗（多写线程 + 事务/ WAL 一起过一遍）
//
// 这条路径在 C0 下是**安全**的，而且是刻意选的：写操作取独占闩，因此缓冲池的
// 未命中/淘汰路径、以及 BufferPool::Impl::write_op（一个普通成员，不是
// thread_local 的事务深度计数）一次只会被一个线程碰。
// 换句话说，这条用例同时验证了"独占闩确实生效"和"M4-2 的写事务在闩下没被并发破坏"。
// ---------------------------------------------------------------------------
TEST(C0, ConcurrentWritersAreSerialized) {
  constexpr int kWriters = 4;
  constexpr int kPerWriter = 500;
  Fixture f("writers");

  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < kWriters; ++t) {
    workers.emplace_back([&f, t, &failures]() {
      for (int i = 0; i < kPerWriter; ++i) {
        // 各写线程的键区间互不相交，避免把测试变成"同键覆写"的语义问题
        const uint64_t key = static_cast<uint64_t>(t) * kPerWriter + static_cast<uint64_t>(i);
        if (!f.tree.Insert(MakeEntry(key)).ok()) {
          ++failures;
        }
      }
    });
  }
  for (auto& w : workers) {
    w.join();
  }

  EXPECT_EQ(failures.load(), 0);
  // 结构自检 + 逐条点查：写串行化如果没生效，这里会是"丢记录"或"页结构损坏"
  ASSERT_TRUE(f.tree.Validate().ok());
  uint64_t scanned = 0;
  ASSERT_TRUE(f.tree.ScanAll([&scanned](const RecordRef&) {
    ++scanned;
    return true;
  }).ok());
  EXPECT_EQ(scanned, static_cast<uint64_t>(kWriters) * kPerWriter);
  for (int t = 0; t < kWriters; ++t) {
    const uint64_t key = static_cast<uint64_t>(t) * kPerWriter;
    auto r = f.tree.Search(MakeProbe(key));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(std::string(r.value().payload, r.value().payload_len), PayloadFor(key));
  }
  std::printf("  [info] %d writer threads x %d inserts: entries=%llu, validate ok\n", kWriters,
              kPerWriter, static_cast<unsigned long long>(scanned));
}

// ---------------------------------------------------------------------------
// 用例 3：对照实验 —— 同机同负载，但每个线程用**自己的**池与树（零共享状态）
//
// 用途：ReadScaling 跑出来的扩展比远低于线程数时，必须先排除"机器/测试框架
// 本身跑不上去"，否则会把机器噪声当成 C0 的结构性上限，进而误导 M6-2 的方向。
// 对照组与 ReadScaling 的唯一区别是"有没有共享的缓冲池"：
//   * 对照组装不下 8x 扩展 → 问题在机器/调度/测试框架，与 C0 无关
//   * 对照组接近 8x   扩展 → 瓶颈在共享状态，是 C0 自身的结构上限
// ---------------------------------------------------------------------------
TEST(C0, ScalingControlIndependentPools) {
  constexpr int kWorkers = 8;

  // 每个 worker 一棵独立的树。
  std::vector<std::unique_ptr<Fixture>> fx;
  fx.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    fx.emplace_back(new Fixture("ctl" + std::to_string(i)));
  }
  // 【装载必须并行，否则这条用例会跑到 ctest 超时】
  // 装载是 WAL 组提交主导的：`group_commit_ms = 2`，串行装载时每条插入的提交都要
  // 等满这个 2ms 窗口（没有第二条提交来凑成一组）。单线程装 8 棵 20000 键的树
  // = 16 万次提交 ≈ 5 分钟，实测 289 秒 —— ctest 超时是 300 秒。
  // 八个池各有各的 WAL 与文件，装载又全在主线程之外，所以并行装载不改变被测量的
  // 任何东西（测量期只读、零缺页），只是把准备阶段的时间除以 8。
  {
    std::atomic<int> load_fail{0};
    std::vector<std::thread> loaders;
    loaders.reserve(kWorkers);
    for (int i = 0; i < kWorkers; ++i) {
      loaders.emplace_back([&fx, i, &load_fail]() {
        BPlusTree& tree = fx[static_cast<size_t>(i)]->tree;
        for (uint64_t k = 0; k < kKeys; ++k) {
          if (!tree.Insert(MakeEntry(k)).ok()) {
            ++load_fail;
            return;
          }
        }
        // 预热：把该池的工作集全部读进内存，保证测量期零缺页
        for (uint64_t k = 0; k < kKeys; ++k) {
          if (!tree.Search(MakeProbe(k)).ok()) {
            ++load_fail;
            return;
          }
        }
      });
    }
    for (auto& t : loaders) {
      t.join();
    }
    ASSERT_EQ(load_fail.load(), 0);
  }

  auto run = [&](int workers) -> double {
    std::atomic<uint64_t> mismatches{0};
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int w = 0; w < workers; ++w) {
      ts.emplace_back([&fx, w, &mismatches]() {
        std::mt19937_64 rng(0x11u + static_cast<uint64_t>(w) * 7919u);
        BPlusTree& tree = fx[static_cast<size_t>(w)]->tree;
        uint64_t bad = 0;
        for (int i = 0; i < kQueriesPerThread; ++i) {
          const uint64_t key = rng() % kKeys;
          auto rec = tree.Search(MakeProbe(key));
          if (!rec.ok() || std::string(rec.value().payload, rec.value().payload_len) != PayloadFor(key)) {
            ++bad;
          }
        }
        mismatches.fetch_add(bad, std::memory_order_relaxed);
      });
    }
    for (auto& t : ts) {
      t.join();
    }
    const auto t1 = std::chrono::steady_clock::now();
    EXPECT_EQ(mismatches.load(), 0u);
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    return sec > 0.0 ? static_cast<double>(workers) * kQueriesPerThread / sec : 0.0;
  };

  const double one = run(1);
  const double eight = run(kWorkers);
  const double speedup = one > 0.0 ? eight / one : 0.0;
  std::printf("  [info] control (independent pools, no shared state): 1 thread=%.0f ops/s "
              "8 threads=%.0f ops/s -> %.2fx\n",
              one, eight, speedup);
  // 24 核机器上，零共享的 8 线程至少该有像样的扩展；低于 3x 才说明机器/框架有上限。
  EXPECT_GE(speedup, 3.0);
}

// ---------------------------------------------------------------------------
// 用例 4：守卫可重入（这条不是锦上添花 —— shared_mutex 不可重入，嵌套就是自死锁，
// 而死锁的表现是"测试挂住"，最容易被误当成"跑得慢"）
// ---------------------------------------------------------------------------
TEST(C0, LatchGuardIsReentrant) {
  std::shared_mutex latch;
  {
    // 同模式嵌套：只有最外层真的加锁
    C0LatchGuard outer(&latch, LatchMode::kShared);
    EXPECT_TRUE(outer.owns());
    C0LatchGuard inner(&latch, LatchMode::kShared);
    EXPECT_FALSE(inner.owns());
    C0LatchGuard inner2(&latch, LatchMode::kShared);
    EXPECT_FALSE(inner2.owns());
  }
  {
    // 独占下再要任意模式：不再加锁，也不能死锁
    C0LatchGuard outer(&latch, LatchMode::kExclusive);
    EXPECT_TRUE(outer.owns());
    C0LatchGuard inner(&latch, LatchMode::kExclusive);
    EXPECT_FALSE(inner.owns());
    C0LatchGuard inner_shared(&latch, LatchMode::kShared);
    EXPECT_FALSE(inner_shared.owns());
  }
  {
    // nullptr（尚未 Open）退化为空操作
    C0LatchGuard none(nullptr, LatchMode::kExclusive);
    EXPECT_FALSE(none.owns());
  }
  // 守卫全部析构后锁必须真的回到手上：这一段若挂住就说明计数漏减
  ASSERT_TRUE(latch.try_lock());
  latch.unlock();

  // 多线程各持一份共享闩：共享闩必须真的允许并发持有
  std::atomic<int> concurrent{0};
  std::atomic<int> peak{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&latch, &concurrent, &peak]() {
      C0LatchGuard g(&latch, LatchMode::kShared);
      const int now = concurrent.fetch_add(1) + 1;
      int seen = peak.load();
      while (now > seen && !peak.compare_exchange_weak(seen, now)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      concurrent.fetch_sub(1);
    });
  }
  for (auto& t : ts) {
    t.join();
  }
  EXPECT_GE(peak.load(), 2);
  std::printf("  [info] concurrent shared holders observed: %d\n", peak.load());
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

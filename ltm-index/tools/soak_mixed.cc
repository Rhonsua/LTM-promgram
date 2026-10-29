// tools/soak_mixed.cc
// C1 验收的第三件：混合读写压测驱动（方案 §6「30 分钟无死锁无挂起」）。
//
// ---------------------------------------------------------------------------
// 【先读这一段，避免把它当成 C1 的通过证据，也避免把它当成"今天就是绿的"】
//
// 它的结果**取决于池子相对于工作集的大小**，两种 regime 的含义完全不同：
//
//   resident regime（frames >= 树的页数）
//       -> 测量期几乎零缺页，走不到帧表的加载/淘汰路径。
//          今天绿 = 只证明【驱动本身】工作 + C0 的命中路径串行化成立。
//          **它没有测到 C1 要改的东西**，不许写成"C1 压测通过"。
//
//   evicting regime（frames << 树的页数）
//       -> 必然反复缺页/淘汰，踩到缺陷 #46（帧表在共享闩下不安全）。
//          **今天就是红的**，实测两种症状：
//            the fetched page claims page_id=68, but the requested page is 67
//            Unpin count exceeds the pin count: 515
//          且单线程同池子是干净的（949580 次淘汰，0 失败）—— 说明红来自并发，
//          不是"池子太小所以什么都不能跑"。
//
// 所以这个驱动一个命令两个身份：
//   * 今天：C1 帧表改造的**第二条现成判据**（第一条是 c0_boundary_probe，
//     那条只做点查；这条覆盖区间扫 + 写路径）。
//   * C1 之后：30 分钟验收的压测主体。
//
// 驱动跑完会打印 REGIME 行，把这一次到底属于哪种明确写出来 —— 就是防止
// 有人拿 resident 的绿去交 evicting 的差。
// ---------------------------------------------------------------------------
//
// 【"无挂起"怎么判：心跳，不是超时】
// 只加一个总超时是不够的：死锁的典型表现是**进程还活着、但一行都不往前走了**，
// 它会在超时到点前一直"看起来正常"。所以这里用进度计数器 + 监视线程：
// 连续 `--stall` 秒（进度零增长）就判定挂起，打印每个线程的最后位置，然后 abort。
// 这才是"无挂起"的判据，而不是"跑完了"。
// ---------------------------------------------------------------------------
//
// 【为什么必须周期性做检查点】不做的话 30 分钟跑不下去：实测这份负载写 WAL 约
// 40 MB/s，12 秒就是 512 MB，1800 秒会写出几十 GB 把盘写满。检查点会截断日志。
// 但检查点有两条既定前提（见 .workbuddy-ai 的已定死口径）：
//   ① 不许在写事务在途时做（截断会丢该事务的 UNDO）；
//   ② 写操作与检查点互斥。C1 落地后取的是 `BufferPool::write_mutex`（D4 单写者），
//      不再是 C0 全局闩（C0 已从索引层撤掉）。
// 所以这里用**排空屏障**：置 pause -> 等所有线程退出当前操作 -> 取写锁做检查点
// -> 放 pause。等 `active == 0` 这条判据本身就挡住了"先读到 pause==false 再自增"
// 的竞态：晚自增的那个线程也必须减回去，等待才会结束。
//
// 【C1 下的锁语义（已核对，不是假设）】
//   写（Insert/Erase）      -> 单写者（BufferPool::write_mutex）+ 被改页的独占页闩
//   读（Search/RangeScan/
//       ScanAll/Validate）   -> 只取共享页闩，不取任何全局闩
//   ScanAll / Validate        -> 共享（ScanAll 转调 RangeScan）
//
// 用法：build-ninja/soak_mixed.exe [seconds] [readers] [writers] [keys] [frames]
//                                   [stall_s] [checkpoint_s]
//       build-ninja/soak_mixed.exe 30  4 0 20000 64      # 今天应当变红（evicting）
//       build-ninja/soak_mixed.exe 30  4 0 20000 4096    # 今天应当变绿（resident）
//       build-ninja/soak_mixed.exe 1800 6 2 20000 512 10 10   # C1 之后的 30 分钟验收
//
// 【注意】挂起或崩溃时会走 abort/death，log/_soak.* 会留在盘上（铁律 6：跑完清 log/）。
// ---------------------------------------------------------------------------
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/recovery.h"
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

Query RangeQuery(uint64_t lo, uint64_t hi) {
  Query q;
  q.kind = QueryKind::kRange1D;
  q.key_lo = lo;
  q.key_hi = hi;
  return q;
}

// 每个线程的进度：监视线程读它判挂起，出事时打印它定位到线程
struct ThreadProgress {
  std::atomic<uint64_t> ops{0};
  std::atomic<uint64_t> phase{0};  // 0=空闲 1=点查 2=区间扫 3=插入 4=删除
};

const char* PhaseName(uint64_t p) {
  switch (p) {
    case 1: return "point-search";
    case 2: return "range-scan";
    case 3: return "insert";
    case 4: return "erase";
    default: return "idle";
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 30;
  const int readers = argc > 2 ? std::atoi(argv[2]) : 4;
  const int writers = argc > 3 ? std::atoi(argv[3]) : 2;
  const uint64_t keys = argc > 4 ? static_cast<uint64_t>(std::atoll(argv[4])) : 20000;
  const uint32_t frames = argc > 5 ? static_cast<uint32_t>(std::atoi(argv[5])) : 512;
  const double stall_limit = argc > 6 ? std::atof(argv[6]) : 10.0;
  const double ckpt_every = argc > 7 ? std::atof(argv[7]) : 10.0;

  const uint64_t churn = keys / 8 == 0 ? 1 : keys / 8;
  const uint64_t base_end = keys > churn ? keys - churn : 0;

  const std::string page_file = "log/_soak.data";
  const std::string wal_file = "log/_soak.wal";
  std::system("if not exist log mkdir log");
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());

  // 构造顺序按项目既定口径：DiskManager -> RecoveryManager(持有 WalWriter)
  // -> BufferPool(借用)；销毁顺序相反。
  DiskOptions dopt;
  dopt.page_file = page_file;
  dopt.wal_file = wal_file;
  dopt.num_pages = 1 << 16;
  DiskManager disk(dopt);
  WalOptions wopt;
  RecoveryManager rec(wopt, &disk);

  BufferPoolOptions popt;
  popt.pool_frames = frames;
  BufferPool pool(popt, &disk, rec.wal(), &rec, nullptr, nullptr);
  CheckpointManager ckpt(wopt, &disk, &rec, &pool);

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

  std::printf("== soak_mixed ==\n");
  std::printf("duration=%ds readers=%d writers=%d keys=%llu frames=%u stall=%.1fs ckpt=%.1fs\n",
              seconds, readers, writers, static_cast<unsigned long long>(keys), frames,
              stall_limit, ckpt_every);

  for (uint64_t k = 0; k < base_end; ++k) {
    const Status s = tree.Insert(MakeEntry(k));
    if (!s.ok()) {
      std::printf("base load failed at %llu: %s\n", static_cast<unsigned long long>(k),
                  s.ToString().c_str());
      return 2;
    }
  }

  // ---- 先自检一次，再定 REGIME ----
  // 两个作用：① 载入后就必须是完好的，否则后面变红是"载入坏了"还是"并发坏了"
  // 分不清；② num_pages 只在 Validate 的遍历里统计（tree_ops.cc:1442），
  // 不先 Validate 一次它就是 0，REGIME 会永远判成 resident —— 一个永远说
  // "没测到"的判据等于没有判据。
  {
    const Status v0 = tree.Validate();
    if (!v0.ok()) {
      std::printf("pre-soak Validate failed (base load is already inconsistent): %s\n",
                  v0.ToString().c_str());
      return 2;
    }
  }
  const uint64_t tree_pages = tree.stats().num_pages;
  const uint64_t wal_after_base = disk.WalSize();
  const bool evicting = tree_pages > frames;
  std::printf("base loaded + validated: %llu keys, tree holds %llu pages, pool holds %u frames\n",
              static_cast<unsigned long long>(base_end),
              static_cast<unsigned long long>(tree_pages), frames);
  std::printf("WAL after base load: %llu bytes (one-time; the checkpoint loop truncates it)\n",
              static_cast<unsigned long long>(wal_after_base));
  std::printf("REGIME: %s\n", evicting ? "evicting (frames < tree pages -> miss/evict path is exercised)"
                                       : "resident (frames >= tree pages -> the load/evict path is NOT exercised)");
  if (evicting) {
    std::printf("  evicting is the VALID configuration: it exercises the miss/evict path and,\n"
                "  under C1, real read/write page-latch contention. This used to be red because of\n"
                "  defect #46; a green run here now is the C1 result.\n");
  } else {
    std::printf("  CAVEAT: resident regime -- the load/evict path is NOT exercised, so this run\n"
                "  cannot see the code C1 changes. Re-run with frames < tree pages.\n");
  }
  std::printf("  (base keys [0,%llu) are never touched by writers; churn window [%llu,%llu))\n",
              static_cast<unsigned long long>(base_end),
              static_cast<unsigned long long>(base_end), static_cast<unsigned long long>(keys));

  // 载入后立刻截断一次日志：载入 17500 条会攒下几百 MB 的 WAL，不截的话它会被
  // 算进 peak_wal，把"稳态每个检查点周期占多少盘"这个真正要看的数淹没掉。
  // 此刻还没有工作线程，独占闩即可，不需要排空。
  {
    // C1：写串行化闩（D4 单写者），检查点必须与写操作互斥。
    std::lock_guard<std::recursive_mutex> lk(pool.write_mutex());
    const Status s = ckpt.RunCheckpoint();
    if (!s.ok()) {
      std::printf("post-load checkpoint failed: %s\n", s.ToString().c_str());
      return 2;
    }
  }
  std::printf("post-load WAL truncated to %llu bytes\n",
              static_cast<unsigned long long>(disk.WalSize()));
  std::fflush(stdout);

  std::atomic<bool> stop{false};
  std::atomic<bool> pause{false};
  std::atomic<int> active{0};
  std::atomic<uint64_t> total_ops{0};
  std::atomic<uint64_t> fails{0};
  std::atomic<uint64_t> wrong{0};
  std::atomic<uint64_t> inserts{0};
  std::atomic<uint64_t> erases{0};
  std::atomic<uint64_t> scanned_rows{0};
  std::atomic<uint64_t> peak_wal{0};
  std::atomic<uint64_t> ckpts{0};
  const int nthreads = readers + writers;
  std::unique_ptr<ThreadProgress[]> prog = std::make_unique<ThreadProgress[]>(static_cast<size_t>(nthreads));

  // 每个操作前后进出 active，检查点靠"等到 0"排空
  auto enter = [&] {
    while (pause.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    active.fetch_add(1, std::memory_order_acq_rel);
  };
  auto leave = [&] { active.fetch_sub(1, std::memory_order_acq_rel); };

  std::vector<std::thread> ts;
  ts.reserve(static_cast<size_t>(nthreads));

  for (int t = 0; t < readers; ++t) {
    ts.emplace_back([&, t] {
      ThreadProgress& p = prog[static_cast<size_t>(t)];
      std::mt19937_64 rng(0x5A0Au + static_cast<uint64_t>(t) * 7919u);
      uint64_t n = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        if (pause.load(std::memory_order_acquire)) {
          std::this_thread::yield();
          continue;
        }
        enter();
        ++n;
        // 1/8 走区间扫：它会把叶链整串读进来，是"只测点查"的压测最容易漏掉的路径
        if ((n & 0x7) == 0) {
          p.phase.store(2, std::memory_order_relaxed);
          const uint64_t from = rng() % keys;
          const uint64_t to = from + 200 > keys ? keys - 1 : from + 200;
          uint64_t seen = 0;
          const Status s = tree.RangeScan(RangeQuery(from, to), [&seen](const RecordRef&) {
            ++seen;
            return true;
          });
          if (!s.ok()) {
            ++fails;
          } else {
            scanned_rows.fetch_add(seen, std::memory_order_relaxed);
          }
        } else {
          p.phase.store(1, std::memory_order_relaxed);
          const uint64_t key = rng() % keys;
          auto rec = tree.Search(MakeProbe(key));
          if (rec.ok()) {
            // 【这才是判据】读回来必须是它自己的载荷。读到别人的载荷 = 撕裂。
            // 查不到是允许的（别的线程可能刚删掉窗口里的键）。
            const std::string got(rec.value().payload, rec.value().payload_len);
            if (got != PayloadFor(key) || rec.value().key != key) {
              ++wrong;
            }
          }
        }
        leave();
        p.ops.fetch_add(1, std::memory_order_relaxed);
        total_ops.fetch_add(1, std::memory_order_relaxed);
      }
      p.phase.store(0, std::memory_order_relaxed);
    });
  }

  for (int t = 0; t < writers; ++t) {
    const size_t idx = static_cast<size_t>(readers + t);
    ts.emplace_back([&, idx] {
      ThreadProgress& p = prog[idx];
      std::mt19937_64 rng(0xA17u ^ (static_cast<uint64_t>(idx) * 104729u));
      while (!stop.load(std::memory_order_relaxed)) {
        if (pause.load(std::memory_order_acquire)) {
          std::this_thread::yield();
          continue;
        }
        enter();
        const uint64_t key = base_end + (rng() % churn);
        // 一半插入一半擦除：让窗口里的条数在一个稳态附近抖动，而不是单调把树撑大
        // ——30 分钟持续增长会把工作集推爆，那测的就变成淘汰路径了。
        if ((rng() & 1) != 0) {
          p.phase.store(3, std::memory_order_relaxed);
          if (!tree.Insert(MakeEntry(key)).ok()) {
            ++fails;
          } else {
            inserts.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          p.phase.store(4, std::memory_order_relaxed);
          // 擦一个不存在的键是**允许**的（别的写线程刚删了），不记 fail
          if (tree.Erase(MakeProbe(key)).ok()) {
            erases.fetch_add(1, std::memory_order_relaxed);
          }
        }
        leave();
        p.ops.fetch_add(1, std::memory_order_relaxed);
        total_ops.fetch_add(1, std::memory_order_relaxed);
      }
      p.phase.store(0, std::memory_order_relaxed);
    });
  }

  // ---- 挂起监视：判据是"进度零增长"，不是"总超时" ----
  std::atomic<bool> hung{false};
  std::thread watchdog([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    uint64_t last = total_ops.load();
    auto last_change = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline && !stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      if (pause.load()) {  // 排空/检查点期间进度本来就该停，不算挂起
        last = total_ops.load();
        last_change = std::chrono::steady_clock::now();
        continue;
      }
      const uint64_t now = total_ops.load();
      if (now != last) {
        last = now;
        last_change = std::chrono::steady_clock::now();
        continue;
      }
      const double stalled =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - last_change).count();
      if (stalled >= stall_limit) {
        hung.store(true);
        std::printf("!!!! HANG: no progress for %.1fs (limit %.1fs), total_ops=%llu active=%d\n",
                    stalled, stall_limit, static_cast<unsigned long long>(now), active.load());
        for (int i = 0; i < nthreads; ++i) {
          std::printf("     thread %d: ops=%llu phase=%s\n", i,
                      static_cast<unsigned long long>(
                          prog[static_cast<size_t>(i)].ops.load(std::memory_order_relaxed)),
                      PhaseName(prog[static_cast<size_t>(i)].phase.load()));
        }
        std::printf("     (a deadlock looks exactly like this: process alive, nothing moving)\n");
        std::fflush(stdout);
        std::abort();
      }
    }
    stop.store(true);
  });

  // ---- 主线程：周期性检查点（排空屏障），直到监视线程叫停 ----
  auto next_ckpt = std::chrono::steady_clock::now() + std::chrono::duration<double>(ckpt_every);
  while (!stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (ckpt_every <= 0 || std::chrono::steady_clock::now() < next_ckpt) {
      continue;
    }
    next_ckpt = std::chrono::steady_clock::now() + std::chrono::duration<double>(ckpt_every);

    pause.store(true, std::memory_order_release);
    // 等排空。等不到就是有线程卡在某个调用里出不来了 —— 这也是一种挂起，
    // 而且上面的进度心跳抓不到它（进度可能还在涨，只是这个线程再也回不来）。
    const auto drain_start = std::chrono::steady_clock::now();
    while (active.load(std::memory_order_acquire) != 0) {
      if (std::chrono::duration<double>(std::chrono::steady_clock::now() - drain_start).count() >
          30.0) {
        std::printf("!!!! HANG: could not drain for checkpoint, active=%d after 30s\n",
                    active.load());
        std::fflush(stdout);
        std::abort();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
      // 正式量的是"截断前一刻的日志大小" = 稳态下每个检查点周期真正占的盘。
      // 在截断之前采，而不是在循环里乱采：后者采到的头一次样本可能是**载入期**
      // 攒下的日志（载入 17500 条的日志就有几百 MB），会把稳态值淹没掉。
      const uint64_t before = disk.WalSize();
      if (before > peak_wal.load()) {
        peak_wal.store(before);
      }
      // C1：与写操作取同一把写串行化闩（D4 单写者）。
      std::lock_guard<std::recursive_mutex> lk(pool.write_mutex());
      const Status s = ckpt.RunCheckpoint();
      if (s.ok()) {
        ckpts.fetch_add(1, std::memory_order_relaxed);
      } else {
        ++fails;
        std::printf("[ckpt] RunCheckpoint failed: %s\n", s.ToString().c_str());
      }
    }
    pause.store(false, std::memory_order_release);
  }

  stop.store(true);
  pause.store(false, std::memory_order_release);
  for (auto& th : ts) {
    th.join();
  }
  watchdog.join();

  // ---- 收尾核对（压测已停，单线程）：结构必须完好，载荷必须对得上 ----
  const Status v = tree.Validate();
  uint64_t live = 0;
  const Status scan = tree.ScanAll([&live](const RecordRef&) {
    ++live;
    return true;
  });
  uint64_t reread_bad = 0;
  uint64_t reread_n = 0;
  tree.ScanAll([&](const RecordRef& r) {
    ++reread_n;
    const std::string got(r.payload, r.payload_len);
    if (got != PayloadFor(r.key)) {
      ++reread_bad;
    }
    return true;
  });

  std::printf("\n================ soak summary ================\n");
  std::printf("total_ops=%llu  (inserts=%llu erases=%llu range_rows=%llu)\n",
              static_cast<unsigned long long>(total_ops.load()),
              static_cast<unsigned long long>(inserts.load()),
              static_cast<unsigned long long>(erases.load()),
              static_cast<unsigned long long>(scanned_rows.load()));
  std::printf("read_wrong_payload=%llu   (non-zero => a read returned another record)\n",
              static_cast<unsigned long long>(wrong.load()));
  std::printf("call_failures=%llu\n", static_cast<unsigned long long>(fails.load()));
  std::printf("final: live_entries=%llu validate=%s scan=%s\n",
              static_cast<unsigned long long>(live), v.ok() ? "ok" : v.ToString().c_str(),
              scan.ok() ? "ok" : scan.ToString().c_str());
  std::printf("re-read %llu keys, %llu wrong payload\n", static_cast<unsigned long long>(reread_n),
              static_cast<unsigned long long>(reread_bad));
  std::printf("checkpoints=%llu peak_wal_bytes=%llu (this is what makes 30 min fit on disk)\n",
              static_cast<unsigned long long>(ckpts.load()),
              static_cast<unsigned long long>(peak_wal.load()));
  std::printf("pool: hits=%llu misses=%llu evictions=%llu i6_violations=%llu\n",
              static_cast<unsigned long long>(pool.stats().hits.load()),
              static_cast<unsigned long long>(pool.stats().misses.load()),
              static_cast<unsigned long long>(pool.stats().evictions),
              static_cast<unsigned long long>(pool.stats().i6_violations));
  // C1 判据：页闩的 LockOrderChecker 必须零逆序（逆序/重复获取/升级都记在这一项）。
  const uint64_t order_violations = pool.latch_stats().order_violations.load();
  std::printf("latch: order_violations=%llu shared=%llu exclusive=%llu contentions=%llu\n",
              static_cast<unsigned long long>(order_violations),
              static_cast<unsigned long long>(pool.latch_stats().shared_acquisitions.load()),
              static_cast<unsigned long long>(pool.latch_stats().exclusive_acquisitions.load()),
              static_cast<unsigned long long>(pool.latch_stats().contentions.load()));

  const bool clean = !hung.load() && wrong.load() == 0 && fails.load() == 0 && v.ok() && scan.ok() &&
                     reread_bad == 0 && live >= base_end;
  std::printf("SOAK_VERDICT: %s\n", clean ? "clean" : "FAILED");
  std::printf("REGIME: %s\n", evicting ? "evicting" : "resident");
  if (!evicting) {
    std::printf("CAVEAT: resident regime -- this run did not exercise the frame-table path, so it is\n"
                "        NOT evidence about C1. Re-run with frames < tree pages.\n");
  }
  std::printf("REMAINDER: this run was %ds. C1's acceptance needs >= 1800s in the evicting regime.\n",
              seconds);
  pool.FlushAll();
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());
  return clean ? 0 : 1;
}

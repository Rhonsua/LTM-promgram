// demo/demo_main.cc
// 结题演示脚本（方案 M7）：三条可复现的演示
//   1. 崩溃恢复：写入 N 条 → 强制崩溃 → 重启 → 全量遍历结果与崩溃前已提交状态一致
//   2. 冷热迁移：内存受限下的混合负载，打印命中率 / 驱逐次数 / I/O 放大
//   3. 混合索引：一维 + 多维混合负载，分别走 B+ 树与 R 树路由
//
// M0 阶段：仅打印演示计划与当前可用状态；M4/M5 完成后逐条接通。
//
// M3 起新增「已实现能力」一节：**真的把 B+ 树与 R 树跑一遍**并打印实测规模。
// 在此之前整个 demo 只有 printf —— 退出码 0 但什么都没验证，"三棵树都能跑通"
// 这条端到端检查于是形同虚设。骨架照旧保留（它记录的是还没接通的演示），
// 但已实现的里程碑必须拿出真数字。
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "index/bplus_tree.h"
#include "index/hybrid_index.h"
#include "index/rtree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "util/config.h"
#include "util/crc32c.h"
#include "util/log.h"

namespace {

void Banner(const char* title) {
  std::printf("\n=====================================================================\n");
  std::printf(" %s\n", title);
  std::printf("=====================================================================\n");
}

void Step(const char* text) { std::printf("  - %s\n", text); }

// Mbr::lo / hi 是 Point（带维度校验的包装），构造时逐维写入
ltm::Mbr MakeMbr(uint16_t dim, const std::vector<double>& lo, const std::vector<double>& hi) {
  ltm::Mbr m = ltm::Mbr::Invalid(dim);
  m.lo.resize(dim);
  m.hi.resize(dim);
  for (uint16_t d = 0; d < dim; ++d) {
    m.lo[d] = lo[d];
    m.hi[d] = hi[d];
  }
  return m;
}

// ---------------------------------------------------------------------------
// 已实现能力：B+ 树 / R 树各跑一轮真实负载
//
// 与 ltm_validate_* 的分工：那两个是**闸门**（断言语义，失败即退出码 1），
// 这里只打印实测规模供人眼看 —— 演示要的是"看得见"，不是"卡得住"。
// 用独立的页文件，跑完即删，不污染其他证据。
// ---------------------------------------------------------------------------
struct DemoEnv {
  ltm::DiskManager disk;
  ltm::WalWriter wal;
  ltm::BufferPool pool;

  DemoEnv(const std::string& tag, uint32_t frames)
      : disk(MakeDiskOptions(tag)),
        wal(MakeWalOptions(), &disk),
        pool(MakePoolOptions(frames), &disk, &wal, nullptr, nullptr, nullptr) {}

  static ltm::DiskOptions MakeDiskOptions(const std::string& tag) {
    ltm::DiskOptions d;
    d.page_file = "log/_demo_" + tag + ".data";
    d.wal_file = "log/_demo_" + tag + ".wal";
    d.num_pages = 1u << 17;
    return d;
  }
  static ltm::WalOptions MakeWalOptions() { return ltm::WalOptions(); }
  static ltm::BufferPoolOptions MakePoolOptions(uint32_t frames) {
    ltm::BufferPoolOptions p;
    p.pool_frames = frames;  // 故意小于树的页数，让淘汰/回写路径真的走到
    return p;
  }
};

// M4-2 写屏障的**可见证据**。
//
// 为什么这几行必须打印：写屏障最容易的失败形态不是"报错"，而是**静默空转** ——
// 一个从不被调用的屏障、一条永远为真的断言，看起来和正确实现完全一样（本项目的
// 缺陷 #37 就是这么来的）。所以要让"delta 真的在流"这件事每次都留下数字：
//   deltas > 0            采集器确实在把改动写进 WAL
//   outside-txn == 0      没有落下任何"事务外被改"的页
//   missing-before == 0   没有"事务内改过却没被钉住"的采集漏洞
//   missed == 0           每条 LSN 都真的落进了页头
//   unstamped == 0        没有"被写过却没有任何 REDO 覆盖"的页落盘（这才是 I6 缺口）
//   violations == 0       strict 模式下一次都没触发
//   sync-on-flush > 0     写前日志规则真的执行过（事务中途淘汰脏页必然发生）
void PrintWalEvidence(const ltm::BufferPool& pool) {
  const ltm::BufferPool::PoolStats& ps = pool.stats();
  std::printf("  WAL deltas: appended %llu, identical-skipped %llu, missing-before %llu, outside-txn %llu\n",
              static_cast<unsigned long long>(ps.wal_deltas),
              static_cast<unsigned long long>(ps.deltas_skipped_identical),
              static_cast<unsigned long long>(ps.deltas_missing_before),
              static_cast<unsigned long long>(ps.deltas_outside_txn));
  std::printf("  I6 barrier: lsn-stamps %llu (missed %llu), sync-on-flush %llu, unstamped-writes %llu, violations %llu, last_lsn %llu\n",
              static_cast<unsigned long long>(ps.lsn_stamps),
              static_cast<unsigned long long>(ps.lsn_stamps_missed),
              static_cast<unsigned long long>(ps.wal_sync_on_flush),
              static_cast<unsigned long long>(ps.i6_unstamped_writes),
              static_cast<unsigned long long>(ps.i6_violations),
              static_cast<unsigned long long>(pool.last_lsn()));
  if (ps.delta_bytes_full > 0) {
    std::printf("  WAL volume: %llu bytes of page image recorded; the changed byte range is %llu bytes (%.1fx smaller)\n",
                static_cast<unsigned long long>(ps.delta_bytes_full),
                static_cast<unsigned long long>(ps.delta_bytes_changed),
                ps.delta_bytes_changed > 0
                    ? static_cast<double>(ps.delta_bytes_full) /
                          static_cast<double>(ps.delta_bytes_changed)
                    : 0.0);
  }
}

void DemoBPlus(uint32_t frames) {
  const uint64_t kOps = 4000;
  const uint16_t kCapacity = 32;
  std::remove("log/_demo_bplus.data");
  std::remove("log/_demo_bplus.wal");
  DemoEnv env("bplus", frames);

  ltm::BPlusPageOps ops(kCapacity);
  ltm::BPlusTree tree;
  ltm::TreeBuildOptions topt;
  topt.order_or_capacity = kCapacity;
  const ltm::Status s = tree.Open(&env.pool, &ops, topt, /*tree_id=*/0);
  if (!s.ok()) {
    std::printf("  B+ tree Open failed: %s\n", s.ToString().c_str());
    return;
  }
  // 键空间与操作数同阶、且乘数是奇数且与键空间互素 —— 4000 条键**互不相同**。
  // 这一点很重要：唯一点查与"全扫条数 == 插入条数"这两条断言都依赖它；
  // 键空间写成 kOps*2 却按 [0,kOps) 扫描的那一版，会把一半记录扫不到，
  // 看上去像"树丢了条目"。
  const uint64_t kKeySpace = kOps;
  uint64_t sample_key = 0;
  for (uint64_t i = 0; i < kOps; ++i) {
    ltm::IndexEntry e;
    e.key = (i * 2654435761u) % kKeySpace;  // 打散，避免顺序插入只往最右页写
    if (i == kOps / 2) {
      sample_key = e.key;  // 留一个**确定存在**的键给点查用
    }
    const std::string p(8 + (i % 25), 'b');
    e.payload.assign(p.begin(), p.end());
    const ltm::Status r = tree.Insert(e);
    if (!r.ok()) {
      std::printf("  B+ tree Insert failed: %s\n", r.ToString().c_str());
      return;
    }
  }
  const ltm::Status vs = tree.Validate();
  const ltm::TreeStats st = tree.stats();  // 规模由 Validate 的遍历算出，必须在其后取

  // 点查 + 范围扫描各来一次，证明查询路径真的通
  uint64_t scanned = 0;
  ltm::IndexEntry probe;
  probe.key = sample_key;
  const ltm::StatusOr<ltm::RecordRef> hit = tree.Search(probe);
  ltm::Query q;
  q.kind = ltm::QueryKind::kRange1D;
  q.key_lo = 0;
  q.key_hi = kKeySpace;
  const ltm::Status rs = tree.RangeScan(q, [&scanned](const ltm::RecordRef&) {
    ++scanned;
    return true;
  });
  if (!rs.ok()) {
    std::printf("  B+ tree RangeScan failed: %s\n", rs.ToString().c_str());
    return;
  }

  std::printf("  B+ tree: inserted %llu -> height=%u pages=%llu entries=%llu fill %.1f%%\n",
              static_cast<unsigned long long>(kOps), st.height,
              static_cast<unsigned long long>(st.num_pages),
              static_cast<unsigned long long>(st.num_entries), st.avg_fill_rate * 100.0);
  std::printf("  B+ tree: splits %llu / merges %llu / borrows %llu, hit rate %.1f%%\n",
              static_cast<unsigned long long>(st.num_splits),
              static_cast<unsigned long long>(st.num_merges),
              static_cast<unsigned long long>(st.num_borrows), env.pool.hit_rate() * 100.0);
  std::printf("  B+ tree: point lookup key=%llu %s, range scan hit %llu entries, invariant self-check %s\n",
              static_cast<unsigned long long>(sample_key), hit.ok() ? "hit" : "miss",
              static_cast<unsigned long long>(scanned),
              vs.ok() ? "all green" : vs.ToString().c_str());
  PrintWalEvidence(env.pool);
  std::remove("log/_demo_bplus.data");
  std::remove("log/_demo_bplus.wal");
}

void DemoRTree(uint32_t frames) {
  const uint64_t kOps = 4000;
  const uint16_t kCapacity = 32;
  const uint16_t kDim = 2;
  std::remove("log/_demo_rtree.data");
  std::remove("log/_demo_rtree.wal");
  DemoEnv env("rtree", frames);

  ltm::RTreePageOps ops(kDim, ltm::RSplitPolicy::kQuadratic, kCapacity);
  ltm::RTree tree;
  ltm::TreeBuildOptions topt;
  topt.order_or_capacity = kCapacity;
  topt.mbr_dim = kDim;
  topt.split_policy = "quadratic";
  const ltm::Status s = tree.Open(&env.pool, &ops, topt, /*tree_id=*/2);
  if (!s.ok()) {
    std::printf("  R tree Open failed: %s\n", s.ToString().c_str());
    return;
  }
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> coord(0.0, 1000.0);
  std::uniform_real_distribution<double> extent(0.0, 5.0);
  for (uint64_t i = 0; i < kOps; ++i) {
    std::vector<double> lo(kDim), hi(kDim);
    for (uint16_t d = 0; d < kDim; ++d) {
      const double c = coord(rng);
      const double e = extent(rng);
      lo[d] = c - e;
      hi[d] = c + e;
    }
    ltm::IndexEntry e;
    e.key = i + 1;
    e.mbr = MakeMbr(kDim, lo, hi);
    const std::string p(8 + (i % 25), 'r');
    e.payload.assign(p.begin(), p.end());
    const ltm::Status r = tree.Insert(e);
    if (!r.ok()) {
      std::printf("  R tree Insert failed: %s\n", r.ToString().c_str());
      return;
    }
  }
  const ltm::Status vs = tree.Validate();
  const ltm::TreeStats st = tree.stats();

  // 空间相交 + KNN 各来一次
  uint64_t window_hits = 0;
  ltm::Query sq;
  sq.kind = ltm::QueryKind::kIntersect;
  sq.mbr = MakeMbr(kDim, std::vector<double>(kDim, 400.0), std::vector<double>(kDim, 600.0));
  const ltm::Status ss = tree.SpatialQuery(sq, [&window_hits](const ltm::RecordRef&) {
    ++window_hits;
    return true;
  });
  uint64_t knn_hits = 0;
  uint64_t knn_brute = 0;
  ltm::Query kq;
  kq.kind = ltm::QueryKind::kKnn;
  kq.probe.resize(kDim);
  for (uint16_t d = 0; d < kDim; ++d) {
    kq.probe[d] = 500.0;
  }
  kq.k = 10;
  const ltm::Status ks = tree.Knn(kq, [&knn_hits](const ltm::RecordRef&, double) {
    ++knn_hits;
    return true;
  });
  const uint64_t knn_pages = tree.knn_stats().pages_visited;
  const ltm::Status bs = tree.KnnBruteForce(kq, [&knn_brute](const ltm::RecordRef&, double) {
    ++knn_brute;
    return true;
  });
  const uint64_t brute_pages = tree.knn_stats().brute_pages;

  std::printf("  R tree: inserted %llu -> height=%u pages=%llu entries=%llu fill %.1f%%\n",
              static_cast<unsigned long long>(kOps), st.height,
              static_cast<unsigned long long>(st.num_pages),
              static_cast<unsigned long long>(st.num_entries), st.avg_fill_rate * 100.0);
  std::printf("  R tree: splits %llu / merges %llu / borrows %llu, avg MBR overlap area %.4f\n",
              static_cast<unsigned long long>(st.num_splits),
              static_cast<unsigned long long>(st.num_merges),
              static_cast<unsigned long long>(st.num_borrows), st.avg_overlap_area);
  std::printf("  R tree: spatial intersect [400,600]^2 hit %llu entries, KNN(k=10) returned %llu"
              " (brute force %llu entries)\n",
              static_cast<unsigned long long>(window_hits),
              static_cast<unsigned long long>(knn_hits),
              static_cast<unsigned long long>(knn_brute));
  std::printf("  R tree: KNN visited %llu pages / brute force %llu pages, invariant self-check %s\n",
              static_cast<unsigned long long>(knn_pages),
              static_cast<unsigned long long>(brute_pages),
              vs.ok() && ss.ok() && ks.ok() && bs.ok() ? "all green" : "failed");
  PrintWalEvidence(env.pool);
  std::remove("log/_demo_rtree.data");
  std::remove("log/_demo_rtree.wal");
}

// ---------------------------------------------------------------------------
// 混合索引：一维 + 多维混合负载，四条查询路径各走一遍
//
// 【它证明的是"两棵树一份记录"这件事】B+ 树的叶条目里直接存 R 树的叶页号
// （方案 §4.9），所以按键定位空间记录不需要独立的映射表；而 Put/Erase 用
// 一个写事务罩住两棵树，崩溃时要么都生效要么都回滚。
// ---------------------------------------------------------------------------
void DemoHybrid(uint32_t frames) {
  const uint64_t kOps = 2000;
  const uint16_t kDim = 2;
  std::remove("log/_demo_hybrid.data");
  std::remove("log/_demo_hybrid.wal");

  ltm::HybridOptions opt;
  opt.config.disk.page_file = "log/_demo_hybrid.data";
  opt.config.disk.wal_file = "log/_demo_hybrid.wal";
  opt.config.disk.num_pages = 1u << 16;
  opt.config.pool.pool_frames = frames;
  opt.bplus.order_or_capacity = 32;
  opt.rtree.order_or_capacity = 32;
  opt.rtree.mbr_dim = kDim;
  opt.rtree.split_policy = "quadratic";

  ltm::HybridIndex idx;
  const ltm::Status opened = idx.Open(opt);
  if (!opened.ok()) {
    std::printf("  HybridIndex::Open failed: %s\n", opened.ToString().c_str());
    return;
  }
  std::mt19937_64 rng(41);
  std::uniform_real_distribution<double> coord(0.0, 1000.0);
  std::uniform_real_distribution<double> extent(0.0, 6.0);
  for (uint64_t i = 1; i <= kOps; ++i) {
    std::vector<double> lo(kDim), hi(kDim);
    for (uint16_t d = 0; d < kDim; ++d) {
      const double c = coord(rng);
      const double e = extent(rng);
      lo[d] = c - e;
      hi[d] = c + e;
    }
    ltm::IndexEntry e;
    e.key = i;
    e.mbr = MakeMbr(kDim, lo, hi);
    const std::string p(10 + (i % 20), 'h');
    e.payload.assign(p.begin(), p.end());
    const ltm::Status r = idx.Put(e);
    if (!r.ok()) {
      std::printf("  HybridIndex::Put failed: %s\n", r.ToString().c_str());
      return;
    }
  }

  // 四条查询路径：点查 / 一维范围 / 空间相交 / KNN
  uint64_t point_hit = 0;
  auto one = idx.Get(kOps / 2);
  if (one.ok()) {
    point_hit = 1;
  }
  uint64_t range_hits = 0;
  ltm::Query rq;
  rq.kind = ltm::QueryKind::kRange1D;
  rq.key_lo = 1;
  rq.key_hi = 100;
  ltm::Status rs = idx.Query(rq, [&range_hits](const ltm::IndexEntry&) {
    ++range_hits;
    return true;
  });
  uint64_t spatial_hits = 0;
  ltm::Query sq;
  sq.kind = ltm::QueryKind::kIntersect;
  sq.mbr = MakeMbr(kDim, std::vector<double>(kDim, 400.0), std::vector<double>(kDim, 600.0));
  ltm::Status ss = idx.Query(sq, [&spatial_hits](const ltm::IndexEntry&) {
    ++spatial_hits;
    return true;
  });
  std::vector<ltm::IndexEntry> knn;
  ltm::Query kq;
  kq.kind = ltm::QueryKind::kKnn;
  kq.probe.resize(kDim);
  for (uint16_t d = 0; d < kDim; ++d) {
    kq.probe[d] = 500.0;
  }
  ltm::Status ks = idx.Knn(kq, 10, &knn);
  const ltm::Status vs = idx.Validate();
  const ltm::IndexStats st = idx.stats();

  std::printf("  hybrid: %llu records -> bplus pages=%llu entries=%llu, rtree pages=%llu entries=%llu\n",
              static_cast<unsigned long long>(kOps),
              static_cast<unsigned long long>(st.bplus.num_pages),
              static_cast<unsigned long long>(st.bplus.num_entries),
              static_cast<unsigned long long>(st.rtree.num_pages),
              static_cast<unsigned long long>(st.rtree.num_entries));
  std::printf("  hybrid: point %s, 1-D range [1,100] -> %llu, spatial [400,600]^2 -> %llu, KNN(k=10) -> %zu\n",
              point_hit ? "hit" : "miss", static_cast<unsigned long long>(range_hits),
              static_cast<unsigned long long>(spatial_hits), knn.size());
  std::printf("  hybrid: Get/Erase hit the B+ side only (the MBR travels inside the B+ payload);\n");
  std::printf("          the I8 cross-check used %llu hint hits / %llu stale hints (chain scans %llu)\n",
              static_cast<unsigned long long>(st.locate_hint_hits),
              static_cast<unsigned long long>(st.locate_hint_misses),
              static_cast<unsigned long long>(st.locate_chain_scans));
  std::printf("  hybrid: I8 cross-check %s (%llu record pairs verified, both directions)\n",
              (vs.ok() && rs.ok() && ss.ok() && ks.ok()) ? "all green" : "FAILED",
              static_cast<unsigned long long>(st.i8_pairs_checked));
  PrintWalEvidence(*idx.pool());
  std::remove("log/_demo_hybrid.data");
  std::remove("log/_demo_hybrid.wal");
}

}  // namespace

int main(int argc, char** argv) {
  ltm::Logger::Instance().SetLevel(ltm::LogLevel::kInfo);
  ltm::Logger::Instance().SetLogFile("log/demo.log");

  auto cfg_or = ltm::Config::FromCommandLine(argc, argv);
  if (!cfg_or.ok()) {
    std::printf("config parse failed: %s\n", cfg_or.status().ToString().c_str());
    return 2;
  }
  const ltm::Config& cfg = cfg_or.value();

  std::printf("generic larger-than-memory tree index -- demo\n");
  std::printf("params: pool_frames=%u page_size=%u prefetch_depth=%u\n", cfg.pool.pool_frames,
              cfg.disk.page_size, cfg.pool.prefetch_depth);
  std::printf("checksum: CRC32C (hardware acceleration: %s)\n",
              ltm::Crc32cUsingHardware() ? "yes" : "no (software table)");

  Banner("Demo 1: crash recovery (needs M4)");
  Step("write N records -> record the committed set S");
  Step("force a crash at a chosen fault point (FaultInjector::SetCrashAfterWalAppend)");
  Step("restart the process -> RecoveryManager::Recover() -> traverse the whole set S'");
  Step("assert S' == S (nothing committed is lost) and Validate() is all green (invariants I1..I10)");
  Step("evidence: report/recovery_matrix.md");

  Banner("Demo 2: hot/cold migration under limited memory (needs M1/M6)");
  Step("dataset size = 3 x pool_frames x page_size");
  Step("run YCSB-style A/B/C mixed workloads; print hit rate / evictions / I/O amplification");
  Step("compare against a pure in-memory index: throughput and p99 latency cost curves");
  Step("evidence: report/bench_*.csv, report/figures/*.png");

  Banner("Demo 3: hybrid 1-D + multi-dimensional index (M5 done)");
  Step("HybridIndex::Put writes the B+ tree and the R tree in ONE transaction (same txn_id)");
  Step("a key locates its R-tree record straight from the B+ leaf entry, with no separate mapping table");
  Step("the locate hint is validated and falls back to a leaf-chain scan when the R tree moved the record");
  Step("demonstrate all four query routes: point / 1-D range / spatial intersect / KNN");
  Step("evidence: report/m5/ and tests/index/test_hybrid_diff.cc");

  Banner("Implemented: B+ tree, R tree and the hybrid index all running");
  Step("real buffer pool + real page file + random workload; sizes and invariant self-check results are measured");
  DemoBPlus(cfg.pool.pool_frames);
  DemoRTree(cfg.pool.pool_frames);
  DemoHybrid(cfg.pool.pool_frames);

  Banner("current status");
  std::printf("  Done: M0 skeleton / M1 storage core / M2 B+ tree / M3 R tree / M4 WAL + crash recovery\n");
  std::printf("        M5 hybrid index (single-transaction dual-tree, I8 cross-checked)\n");
  std::printf("  To do: M6 concurrency + benchmark / M7 wrap-up\n");

  std::printf("\nDemo finished. See docs/status.md for progress.\n");
  return 0;
}

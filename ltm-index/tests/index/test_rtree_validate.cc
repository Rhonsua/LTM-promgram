// tests/index/test_rtree_validate.cc
// M3 专用验收入口：R 树不变量全量自检 + 空间查询 / KNN 与暴力扫描等价性。
//
// 与 M2 的 ltm_validate_bplus 对称：**真缓冲池 + 真页算子 + 随机增删负载 + 真值表比对**。
// 本程序刻意不接受"空树通过" —— I9（MBR 精确包含）、I3（填充率）、KNN 等价在空树上
// 都是平凡成立的，因此每一条断言都要求树非空且发生过分裂。
//
// 用法：
//   ltm_validate_rtree                    跑内建负载后自检
//   ltm_validate_rtree --ops=8000         指定操作数
//   ltm_validate_rtree --capacity=32      指定每页最大条目数
//   ltm_validate_rtree --frames=64        指定缓冲池帧数（故意设小可逼出淘汰路径）
//   ltm_validate_rtree --dim=2            指定维度
//   ltm_validate_rtree --policy=quadratic 分裂策略（even / linear / quadratic）
//   ltm_validate_rtree --seed=42          随机种子
//   ltm_validate_rtree --quiet            只输出结论
//   ltm_validate_rtree --policy-csv=report/rtree_split_policy.csv
//                                         三策略对照，结果直接写成 CSV
//
// 退出码：0 = 通过；1 = 校验失败；2 = 尚未实现
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "index/rtree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "util/log.h"

namespace {

constexpr double kEps = 1e-9;

struct Args {
  // 默认值就是**闸门**的参数（scripts/validate_tree.py 不带参数直接跑本程序）：
  //   ops=8000 让装载后的树长到三层（height=2），覆盖多级内部页的分裂传播；
  //   frames=32 小于树的页数，故意逼出淘汰/回写路径 —— 缓冲池 100% 命中时
  //   "脏页淘汰 + 重读"这条路径一次都没走过，验收会退化成"只在内存里对"。
  uint64_t ops = 8000;
  uint16_t capacity = 32;
  uint32_t frames = 32;
  uint16_t dim = 2;
  uint64_t seed = 42;
  std::string policy = "quadratic";
  bool quiet = false;
  bool trace = false;  // 每步比对树内条目数与真值表，定位"静默丢条目"的首次出现
  std::string policy_csv;  // 非空时改跑三策略对照并写该路径，不做不变量自检
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
    } else if (s.rfind("--dim=", 0) == 0) {
      a.dim = static_cast<uint16_t>(num("--dim="));
    } else if (s.rfind("--seed=", 0) == 0) {
      a.seed = num("--seed=");
    } else if (s.rfind("--policy=", 0) == 0) {
      a.policy = s.substr(std::strlen("--policy="));
    } else if (s == "--quiet") {
      a.quiet = true;
    } else if (s == "--trace") {
      a.trace = true;
    } else if (s.rfind("--policy-csv=", 0) == 0) {
      a.policy_csv = s.substr(std::strlen("--policy-csv="));
    }
  }
  return a;
}

// 真值表里的一条：MBR + 载荷。比对时载荷必须逐字节相同。
struct Truth {
  ltm::Mbr mbr;
  std::string payload;
};

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

bool SameMbr(const ltm::Mbr& a, const ltm::Mbr& b) {
  if (a.lo.dim() != b.lo.dim()) return false;
  for (uint16_t d = 0; d < a.lo.dim(); ++d) {
    if (std::fabs(a.lo[d] - b.lo[d]) > kEps) return false;
    if (std::fabs(a.hi[d] - b.hi[d]) > kEps) return false;
  }
  return true;
}

ltm::Point MakePoint(uint16_t dim, const std::vector<double>& c) {
  ltm::Point p;
  p.resize(dim);
  for (uint16_t d = 0; d < dim; ++d) {
    p[d] = c[d];
  }
  return p;
}

int Fail(const char* what, const std::string& detail) {
  std::printf("%s: %s\n", what, detail.c_str());
  return 1;
}

// ---------------------------------------------------------------------------
// 三策略对照（--policy-csv=<路径>）：M3 的量化证据
//
// 为什么放在这里、而不是等 M6 的 ltm_bench：分裂策略是 R 树的**结构性参数**
// （它决定树形、重叠面积与查询代价），属于 M3 的验收面。等 bench（M6-3）落地
// 再测，"三策略对照"这一条在 M3 就没有证据可交。数字由本程序直接写进 CSV ——
// 手工誊抄的表格既无法复现，也违背方案 §9.2「禁止手工录入数字」。
//
// 公平性：三棵树跑**同一份**条目序列与**同一组**查询点，只换策略。条目键取
// 1..ops 且互不相同，于是三棵树的条目数必然相同，"页数/填充率/重叠面积"才
// 是同口径可比量。
// ---------------------------------------------------------------------------
struct PolicyRow {
  std::string policy;
  double build_ms = 0.0;
  double query_ms = 0.0;
  uint32_t height = 0;
  uint64_t pages = 0;
  uint64_t entries = 0;
  double fill = 0.0;
  double overlap = 0.0;
  uint64_t splits = 0;
  uint64_t knn_pages = 0;
  uint64_t brute_pages = 0;
};

// 跑一棵树（指定策略），把量化指标填进 out。
// 失败时返回 1，并把原因写进 *err（调用方已经打印不了更具体的上下文了）。
int RunOnePolicy(const Args& args, const char* policy, const std::vector<ltm::IndexEntry>& load,
                 const std::vector<ltm::Query>& knn_queries,
                 const std::vector<ltm::Query>& spatial_queries, PolicyRow* out, std::string* err) {
  const std::string page_file = std::string("log/_policy_") + policy + ".data";
  const std::string wal_file = std::string("log/_policy_") + policy + ".wal";
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());

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

  ltm::RTreePageOps ops(args.dim, ltm::ParseSplitPolicy(policy), args.capacity);
  ltm::RTree tree;
  ltm::TreeBuildOptions topt;
  topt.order_or_capacity = args.capacity;
  topt.mbr_dim = args.dim;
  topt.split_policy = policy;
  const ltm::Status s = tree.Open(&pool, &ops, topt, /*tree_id=*/2);
  if (!s.ok()) {
    *err = std::string("Open(") + policy + ") failed: " + s.ToString();
    return 1;
  }

  const std::chrono::steady_clock::time_point t_build = std::chrono::steady_clock::now();
  for (const ltm::IndexEntry& e : load) {
    const ltm::Status r = tree.Insert(e);
    if (!r.ok()) {
      *err = std::string("Insert(") + policy + ") failed: " + r.ToString();
      return 1;
    }
  }
  out->build_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t_build)
                      .count();

  const ltm::Status vs = tree.Validate();
  if (!vs.ok()) {
    *err = std::string("Validate(") + policy + ") failed: " + vs.ToString();
    return 1;
  }
  const ltm::TreeStats st = tree.stats();  // 规模字段由 Validate 的遍历算出，必须在其后取
  out->height = st.height;
  out->pages = st.num_pages;
  out->entries = st.num_entries;
  out->fill = st.avg_fill_rate;
  out->overlap = st.avg_overlap_area;
  out->splits = st.num_splits;

  if (st.num_entries != load.size()) {
    *err = std::string("entry count mismatch (") + policy + "): in tree " + std::to_string(st.num_entries) +
           ", load " + std::to_string(load.size());
    return 1;
  }

  uint64_t emitted = 0;
  const std::chrono::steady_clock::time_point t_query = std::chrono::steady_clock::now();
  for (const ltm::Query& q : knn_queries) {
    const ltm::Status r = tree.Knn(q, [&emitted](const ltm::RecordRef&, double) {
      ++emitted;
      return true;
    });
    if (!r.ok()) {
      *err = std::string("Knn(") + policy + ") failed: " + r.ToString();
      return 1;
    }
    // Knn 每次调用都会重置探针计数，必须**逐次累加**；KnnBruteForce 只重置
    // brute_pages，所以两列要各读各的、不能都放到最后读。
    out->knn_pages += tree.knn_stats().pages_visited;
    const ltm::Status rb = tree.KnnBruteForce(q, [](const ltm::RecordRef&, double) {
      return true;
    });
    if (!rb.ok()) {
      *err = std::string("KnnBruteForce(") + policy + ") failed: " + rb.ToString();
      return 1;
    }
    out->brute_pages += tree.knn_stats().brute_pages;
  }
  for (const ltm::Query& q : spatial_queries) {
    const ltm::Status r = tree.SpatialQuery(q, [](const ltm::RecordRef&) { return true; });
    if (!r.ok()) {
      *err = std::string("SpatialQuery(") + policy + ") failed: " + r.ToString();
      return 1;
    }
  }
  out->query_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t_query)
                      .count();
  // 反空转护栏：查询集一条都没命中时，剪枝率与页访问数都是无意义的数字。
  if (emitted == 0) {
    *err = std::string("every query came back empty (") + policy + "), so the comparison is meaningless";
    return 1;
  }
  out->policy = policy;

  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());
  return 0;
}

int RunPolicyCsv(const Args& args, const std::string& path) {
  // 负载与查询点先生成一次，三棵树共用（见本节顶部注释）。
  std::vector<ltm::IndexEntry> load;
  load.reserve(static_cast<size_t>(args.ops));
  std::vector<ltm::Query> knn_queries;
  std::vector<ltm::Query> spatial_queries;
  {
    std::mt19937_64 rng(args.seed);
    std::uniform_real_distribution<double> coord(0.0, 1000.0);
    std::uniform_real_distribution<double> extent(0.0, 5.0);
    for (uint64_t i = 0; i < args.ops; ++i) {
      std::vector<double> lo(args.dim), hi(args.dim);
      for (uint16_t d = 0; d < args.dim; ++d) {
        const double c = coord(rng);
        const double e = extent(rng);
        lo[d] = c - e;
        hi[d] = c + e;
      }
      ltm::IndexEntry e;
      e.key = i + 1;  // 键唯一，三棵树条目数一致
      e.mbr = MakeMbr(args.dim, lo, hi);
      const std::string p(8 + (i % 25), 'x');
      e.payload.assign(p.begin(), p.end());
      load.push_back(e);
    }
    for (int q = 0; q < 200; ++q) {
      std::vector<double> c(args.dim);
      for (uint16_t d = 0; d < args.dim; ++d) {
        c[d] = coord(rng);
      }
      ltm::Query kq;
      kq.kind = ltm::QueryKind::kKnn;
      kq.probe = MakePoint(args.dim, c);
      kq.k = 10;
      knn_queries.push_back(kq);
    }
    for (int q = 0; q < 100; ++q) {
      std::vector<double> lo(args.dim), hi(args.dim);
      for (uint16_t d = 0; d < args.dim; ++d) {
        const double a = coord(rng);
        lo[d] = a;
        hi[d] = a + 40.0;
      }
      ltm::Query sq;
      sq.kind = ltm::QueryKind::kIntersect;
      sq.mbr = MakeMbr(args.dim, lo, hi);
      spatial_queries.push_back(sq);
    }
  }

  const char* kPolicies[] = {"even", "linear", "quadratic"};
  std::vector<PolicyRow> rows;
  for (const char* pol : kPolicies) {
    PolicyRow row;
    std::string err;
    if (RunOnePolicy(args, pol, load, knn_queries, spatial_queries, &row, &err) != 0) {
      return Fail("the three-policy comparison failed", err);
    }
    rows.push_back(row);
    std::printf("  [%s] build %.1f ms / pages %llu / overlap %.4f / splits %llu / KNN pages %llu\n", pol,
                row.build_ms, static_cast<unsigned long long>(row.pages), row.overlap,
                static_cast<unsigned long long>(row.splits),
                static_cast<unsigned long long>(row.knn_pages));
  }

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return Fail("cannot write the CSV", path);
  }
  std::fprintf(f, "# generated by ltm_validate_rtree --policy-csv; do not edit by hand\n");
  std::fprintf(f, "# params: ops=%llu capacity=%u frames=%u dim=%u seed=%llu\n",
               static_cast<unsigned long long>(args.ops), args.capacity, args.frames, args.dim,
               static_cast<unsigned long long>(args.seed));
  std::fprintf(f, "# all three trees run the same entry sequence (keys 1..ops) and the same query points (200 KNN with k=10,\n");
  std::fprintf(f, "# and 100 spatial-intersect queries), varying only the split policy; entry counts are identical by construction, so page counts, fill rates and overlap areas are directly comparable.\n");
  std::fprintf(f, "# build_ms includes the R-tree upsert leaf-chain prescan (O(leaf pages), identical across policies),\n");
  std::fprintf(f, "# so compare the policies mainly on the avg_overlap_area / splits / knn_pages columns.\n");
  std::fprintf(f, "policy,build_ms,query_ms,height,pages,entries,avg_fill_rate,");
  std::fprintf(f, "avg_overlap_area,splits,knn_pages,brute_pages,prune_rate\n");
  for (const PolicyRow& r : rows) {
    const double prune = r.brute_pages == 0
                             ? 0.0
                             : 1.0 - static_cast<double>(r.knn_pages) /
                                         static_cast<double>(r.brute_pages);
    std::fprintf(f, "%s,%.3f,%.3f,%u,%llu,%llu,%.4f,%.4f,%llu,%llu,%llu,%.4f\n", r.policy.c_str(),
                 r.build_ms, r.query_ms, r.height, static_cast<unsigned long long>(r.pages),
                 static_cast<unsigned long long>(r.entries), r.fill, r.overlap,
                 static_cast<unsigned long long>(r.splits),
                 static_cast<unsigned long long>(r.knn_pages),
                 static_cast<unsigned long long>(r.brute_pages), prune);
  }
  std::fclose(f);
  std::printf("three-policy comparison done, %zu rows written to %s\n", rows.size(), path.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = Parse(argc, argv);
  ltm::Logger::Instance().SetLevel(args.quiet ? ltm::LogLevel::kError : ltm::LogLevel::kWarn);

  const std::string page_file = "log/_validate_rtree.data";
  const std::string wal_file = "log/_validate_rtree.wal";
#if defined(_WIN32)
  std::system("if not exist log mkdir log");
#else
  std::system("mkdir -p log");
#endif

  // 三策略对照是一条独立的证据链（不与不变量自检共用树），单独走。
  if (!args.policy_csv.empty()) {
    return RunPolicyCsv(args, args.policy_csv);
  }
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

  const ltm::RSplitPolicy policy = ltm::ParseSplitPolicy(args.policy);

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

  ltm::RTreePageOps ops(args.dim, policy, args.capacity);
  ltm::RTree tree;
  ltm::TreeBuildOptions topt;
  topt.order_or_capacity = args.capacity;
  topt.mbr_dim = args.dim;
  topt.split_policy = args.policy;

  ltm::Status s = tree.Open(&pool, &ops, topt, /*tree_id=*/2);
  if (s.code() == ltm::StatusCode::kNotImplemented) {
    std::printf("the R tree is not implemented yet (%s)\n", s.ToString().c_str());
    std::printf("the invariant self-check cannot run. See docs/status.md for the order.\n");
    return 2;
  }
  if (!s.ok()) {
    return Fail("RTree::Open failed", s.ToString());
  }

  // -------------------------------------------------------------------------
  // 负载：随机 MBR 增删，同时维护真值表
  // -------------------------------------------------------------------------
  std::map<uint64_t, Truth> truth;
  std::mt19937_64 rng(args.seed);
  const uint64_t key_space = args.ops;  // 键空间与操作数同阶，制造大量重复
  std::uniform_int_distribution<uint64_t> key_dist(0, key_space);
  std::uniform_real_distribution<double> coord(0.0, 1000.0);
  std::uniform_real_distribution<double> extent(0.0, 5.0);
  uint64_t inserts = 0;
  uint64_t erases = 0;
  uint64_t overwrites = 0;

  auto random_mbr = [&](void) -> ltm::Mbr {
    std::vector<double> lo(args.dim), hi(args.dim);
    for (uint16_t d = 0; d < args.dim; ++d) {
      const double c = coord(rng);
      const double e = extent(rng);
      lo[d] = c - e;
      hi[d] = c + e;
    }
    return MakeMbr(args.dim, lo, hi);
  };

  // 逐步对账（--trace）：每一步都比对"树自报的条目数"与真值表。
  // 结构类损坏（I9/MBR 陈旧）由 Validate 抓，但**丢条目**只要顺手刷新了父页 MBR
  // 就能让 I9 全绿 —— 唯一抓得住它的就是这个计数对账。
  const bool trace = args.trace;
  uint64_t last_step = 0;
  std::string last_op;
  auto reconcile = [&](uint64_t i, const char* op) -> ltm::Status {
    last_step = i;
    last_op = op;
    if (!trace) {
      return ltm::Status::Ok();
    }
    const uint64_t in_tree = tree.stats().num_entries;
    if (in_tree == truth.size()) {
      return ltm::Status::Ok();
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf), "step %llu (%s): in tree %llu entries, truth table %zu",
                  static_cast<unsigned long long>(i), op,
                  static_cast<unsigned long long>(in_tree), truth.size());
    return ltm::Status::Error(ltm::StatusCode::kCorruption, buf);
  };

  // 计数对账只能发现"总量变了"。若某次操作**丢掉一条又留下一条**（例如覆盖时
  // 删对了、插错了页），总量纹丝不动而具体记录已经错位 —— 只有逐条回查抓得住。
  auto verify_entry = [&](uint64_t i, const char* op, const ltm::IndexEntry& e,
                          bool expect_present) -> ltm::Status {
    if (!trace) {
      return ltm::Status::Ok();
    }
    ltm::StatusOr<ltm::RecordRef> got = tree.Search(e);
    if (expect_present) {
      if (!got.ok()) {
        return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                  "step " + std::to_string(i) + " step (" + op + ") lookup failed: " +
                                      got.status().ToString());
      }
      if (got.value().key != e.key || !SameMbr(got.value().mbr, e.mbr) ||
          std::string(got.value().payload, got.value().payload_len) !=
              std::string(e.payload.begin(), e.payload.end())) {
        return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                  "step " + std::to_string(i) + " step (" + op +
                                      ") lookup content disagrees with what was inserted");
      }
      return ltm::Status::Ok();
    }
    if (got.ok()) {
      return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                "step " + std::to_string(i) + " step (" + op + ") is still found on lookup");
    }
    return ltm::Status::Ok();
  };

  // 全量对账（--trace 下逐步执行）：把树整体扫一遍与真值表逐条比对。
  // 计数对账 + 单条回查都抓不住"某条记录的 MBR 被就地改写成另一个值"——
  // 总量没变、也没碰最后一条，只有全量扫描能定位它。
  auto full_check = [&](uint64_t i, const char* op) -> ltm::Status {
    if (!trace) {
      return ltm::Status::Ok();
    }
    std::map<uint64_t, Truth> snap;
    const ltm::Status ss = tree.ScanAll([&snap](const ltm::RecordRef& r) {
      snap[r.key] = Truth{r.mbr, std::string(r.payload, r.payload_len)};
      return true;
    });
    if (!ss.ok()) {
      return ss;
    }
    if (snap.size() != truth.size()) {
      return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                "step " + std::to_string(i) + " step (" + op + "): full scan " +
                                    std::to_string(snap.size()) + " entries, truth table " +
                                    std::to_string(truth.size()) + " entries");
    }
    // 叶链全扫（ScanAll）与**从根下钻**（全空间查询）必须看到同一批记录。
    // 只走叶链的 ScanAll 对"叶页脱离了根可达范围"完全无感 —— 那正是
    // "计数对、不变量绿、却删不掉"的形态。
    std::map<uint64_t, int> by_descent;
    ltm::Query all;
    all.kind = ltm::QueryKind::kIntersect;
    {
      std::vector<double> lo(args.dim, -1e18), hi(args.dim, 1e18);
      all.mbr = MakeMbr(args.dim, lo, hi);
    }
    const ltm::Status ds = tree.SpatialQuery(all, [&by_descent](const ltm::RecordRef& r) {
      ++by_descent[r.key];
      return true;
    });
    if (!ds.ok()) {
      return ds;
    }
    for (const auto& kv : snap) {
      auto d = by_descent.find(kv.first);
      if (d == by_descent.end()) {
        return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                  "step " + std::to_string(i) + " step (" + op + "):key=" +
                                      std::to_string(kv.first) +
                                      " on the leaf chain but **unreachable from the root** (leaf page unlinked)");
      }
      if (d->second != 1) {
        return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                  "step " + std::to_string(i) + " step (" + op + "):key=" +
                                      std::to_string(kv.first) + " reachable by root descent: " +
                                      std::to_string(d->second) + " times");
      }
    }
    // 反方向：**从根下钻看得见、叶链却看不见**的记录 —— 说明叶链从中间断开或
    // 整段被跳过（ScanAll 从 leftmost_leaf 出发沿 next_page 走）。
    // 这类错误不会让任何记录消失于下钻、也不违背 I9，但会让 ScanAll 凭空少一批。
    for (const auto& kv : by_descent) {
      if (snap.find(kv.first) == snap.end()) {
        return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                  "step " + std::to_string(i) + " step (" + op + "):key=" +
                                      std::to_string(kv.first) +
                                      " reachable from the root but **absent from the leaf chain** (chain broken or skipping)");
      }
    }
    for (const auto& kv : truth) {
      auto g = snap.find(kv.first);
      if (g == snap.end()) {
        return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                  "step " + std::to_string(i) + " step (" + op + "):key=" +
                                      std::to_string(kv.first) + " not found by a full scan");
      }
      if (!SameMbr(g->second.mbr, kv.second.mbr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "step %llu (%s): the MBR of key=%llu was rewritten, truth lo=(%.3f,%.3f) "
                      "hi=(%.3f,%.3f) vs in-tree lo=(%.3f,%.3f) hi=(%.3f,%.3f)",
                      static_cast<unsigned long long>(i), op,
                      static_cast<unsigned long long>(kv.first), kv.second.mbr.lo[0],
                      kv.second.mbr.lo[1], kv.second.mbr.hi[0], kv.second.mbr.hi[1],
                      g->second.mbr.lo[0], g->second.mbr.lo[1], g->second.mbr.hi[0],
                      g->second.mbr.hi[1]);
        return ltm::Status::Error(ltm::StatusCode::kCorruption, buf);
      }
      if (g->second.payload != kv.second.payload) {
        return ltm::Status::Error(ltm::StatusCode::kCorruption,
                                  "step " + std::to_string(i) + " step (" + op + "):key=" +
                                      std::to_string(kv.first) + ": payload differs");
      }
    }
    return ltm::Status::Ok();
  };

  for (uint64_t i = 0; i < args.ops; ++i) {
    const uint64_t k = key_dist(rng);
    if ((rng() % 100) < 65 || truth.empty()) {
      const std::string p(8 + (rng() % 25), 'x');
      const ltm::Mbr m = random_mbr();
      const bool existed = truth.count(k) != 0;
      truth[k] = Truth{m, p};
      ltm::IndexEntry e;
      e.key = k;
      e.mbr = m;
      e.payload.assign(p.begin(), p.end());
      const ltm::Status r = tree.Insert(e);
      if (!r.ok()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "step %llu", static_cast<unsigned long long>(i));
        return Fail("Insert failed", std::string(buf) + ": " + r.ToString());
      }
      if (existed) {
        ++overwrites;
      }
      ++inserts;
      const ltm::Status rs = reconcile(i, existed ? "overwrite insert" : "insert");
      if (!rs.ok()) {
        return Fail("entry count comparison failed", rs.ToString());
      }
      const ltm::Status vs = verify_entry(i, "insert", e, /*expect_present=*/true);
      if (!vs.ok()) {
        return Fail("per-entry lookup failed", vs.ToString());
      }
      const ltm::Status fs = full_check(i, "insert");
      if (!fs.ok()) {
        return Fail("full comparison failed", fs.ToString());
      }
    } else {
      auto it = truth.lower_bound(k);
      if (it == truth.end()) {
        it = truth.begin();
      }
      const uint64_t victim = it->first;
      const ltm::Mbr victim_mbr = it->second.mbr;
      truth.erase(it);
      ltm::IndexEntry e;
      e.key = victim;
      e.mbr = victim_mbr;
      const ltm::Status r = tree.Erase(e);
      if (!r.ok()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "step %llu key=%llu", static_cast<unsigned long long>(i),
                      static_cast<unsigned long long>(victim));
        // 删除失败时立刻自检：能把"结构性损坏（I9/MBR 陈旧）"和"定位逻辑漏路径"
        // 区分开 —— 前者 Validate 会指出具体页，后者 Validate 全绿。
        const ltm::Status vs = tree.Validate();
        std::printf("  invariant check after the failed erase: %s\n", vs.ok() ? "all green" : vs.ToString().c_str());
        return Fail("Erase failed", std::string(buf) + ": " + r.ToString());
      }
      ++erases;
      const ltm::Status rs = reconcile(i, "erase");
      if (!rs.ok()) {
        return Fail("entry count comparison failed", rs.ToString());
      }
      const ltm::Status vs = verify_entry(i, "erase", e, /*expect_present=*/false);
      if (!vs.ok()) {
        return Fail("per-entry lookup failed", vs.ToString());
      }
      const ltm::Status fs = full_check(i, "erase");
      if (!fs.ok()) {
        return Fail("full comparison failed", fs.ToString());
      }
    }
  }

  // 空树通过本程序没有意义 —— 直接判失败，防止验收退化成"什么都不做也绿"。
  if (truth.size() < 100) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "the workload is too small (%zu entries alive) to validate the invariants",
                  truth.size());
    return Fail("invalid workload", buf);
  }

  // 第一阶段的规模快照（后面的清空阶段会把这棵树压小，报告里两个都要留）。
  // 必须**先 Validate 再取 stats**：num_pages/填充率/重叠面积是自检遍历
  // 顺带算出来的，stats() 本身不做遍历 —— 顺序反了会读到全 0 的假规模。
  {
    const ltm::Status vs = tree.Validate();
    if (!vs.ok()) {
      return Fail("invariant validation failed during the load phase", vs.ToString());
    }
  }
  const ltm::TreeStats st_loaded = tree.stats();
  // 非平凡性护栏：没有分裂过的树证明不了分裂路径是对的
  if (st_loaded.num_pages < 4 || st_loaded.height < 2) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "the tree is too flat (height=%u pages=%llu), so the split path was not exercised",
                  st_loaded.height, static_cast<unsigned long long>(st_loaded.num_pages));
    return Fail("invalid workload", buf);
  }

  // -------------------------------------------------------------------------
  // 第二阶段：大规模删除。混合负载里删除是零敲碎打的，页永远不会掉到
  // 40% 下溢线以下，于是**借键路径被测到了、合并路径一次都没走到**
  // （"合并 0"就是这种假绿）。这里集中删掉约 85% 的存活记录，逼出
  // 合并、根降级，并顺带在合并后的树上再做一轮增删，验证结构能重新长大。
  // -------------------------------------------------------------------------
  const uint64_t to_delete = (truth.size() * 85) / 100;
  for (uint64_t n = 0; n < to_delete; ++n) {
    // 从真值表里挑一个"偏大"的键，让删除尽量落到不同的叶页上
    auto it = truth.lower_bound(key_dist(rng));
    if (it == truth.end()) {
      it = truth.begin();
    }
    ltm::IndexEntry e;
    e.key = it->first;
    e.mbr = it->second.mbr;
    const uint64_t victim = it->first;
    truth.erase(it);
    const ltm::Status r = tree.Erase(e);
    if (!r.ok()) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "empty phase step %llu key=%llu", static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(victim));
      return Fail("Erase failed", std::string(buf) + ": " + r.ToString());
    }
    ++erases;
    // 逐步对账（full_check 在非 --trace 下是空操作，正式跑不付这个代价）
    const ltm::Status fs = full_check(n, "empty");
    if (!fs.ok()) {
      return Fail("full comparison failed during the empty phase", fs.ToString());
    }
  }
  if (truth.size() < 100) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "only %zu entries survive the empty phase, too few to continue", truth.size());
    return Fail("invalid workload", buf);
  }

  // 清空后再来一轮增删：验证"合并过的树还能重新长大"，顺带覆盖
  // 分裂→合并→再分裂的往复（这是早期版本最容易把结构写坏的地方）。
  for (uint64_t n = 0; n < 600; ++n) {
    const bool do_insert = (rng() % 100) < 60 || truth.empty();
    const uint64_t k = key_dist(rng);
    ltm::IndexEntry e;
    if (do_insert) {
      const std::string p(8 + (rng() % 25), 'y');
      const ltm::Mbr m = random_mbr();
      const bool existed = truth.count(k) != 0;
      truth[k] = Truth{m, p};
      e.key = k;
      e.mbr = m;
      e.payload.assign(p.begin(), p.end());
      const ltm::Status r = tree.Insert(e);
      if (!r.ok()) {
        return Fail("reinsert-after-empty failed", r.ToString());
      }
      ++inserts;
      if (existed) {
        ++overwrites;
      }
    } else {
      auto it = truth.lower_bound(k);
      if (it == truth.end()) {
        it = truth.begin();
      }
      e.key = it->first;
      e.mbr = it->second.mbr;
      truth.erase(it);
      const ltm::Status r = tree.Erase(e);
      if (!r.ok()) {
        return Fail("erase-after-empty failed", r.ToString());
      }
      ++erases;
    }
  }

  // 合并路径必须真的被走到 —— 否则这条闸门对合并是瞎的
  if (tree.stats().num_merges == 0) {
    return Fail("invalid workload", "the workload never triggered a merge, so the merge path was never exercised");
  }

  // -------------------------------------------------------------------------
  // 检查 1：ScanAll 与真值表逐条一致（R 树叶链不按键有序，故按 map 比对而非按序比对）
  // -------------------------------------------------------------------------
  std::map<uint64_t, Truth> got;
  const ltm::Status scan = tree.ScanAll([&got](const ltm::RecordRef& r) {
    got[r.key] = Truth{r.mbr, std::string(r.payload, r.payload_len)};
    return true;
  });
  if (!scan.ok()) {
    return Fail("ScanAll failed", scan.ToString());
  }
  if (got.size() != truth.size()) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "in tree %zu entries, truth table %zu", got.size(), truth.size());
    return Fail("entry counts differ", buf);
  }
  for (const auto& kv : truth) {
    auto g = got.find(kv.first);
    if (g == got.end()) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "key=%llu is not in the tree",
                    static_cast<unsigned long long>(kv.first));
      return Fail("ScanAll missed entries", buf);
    }
    if (!SameMbr(g->second.mbr, kv.second.mbr)) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "key=%llu: MBR differs",
                    static_cast<unsigned long long>(kv.first));
      return Fail("ScanAll MBR differs", buf);
    }
    if (g->second.payload != kv.second.payload) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "key=%llu: payload differs",
                    static_cast<unsigned long long>(kv.first));
      return Fail("ScanAll payload differs", buf);
    }
  }

  // -------------------------------------------------------------------------
  // 检查 2：Search 逐条命中（多点下钻最易漏记录，这条专门抓"只走一条路径"）
  // -------------------------------------------------------------------------
  for (const auto& kv : truth) {
    ltm::IndexEntry probe;
    probe.key = kv.first;
    probe.mbr = kv.second.mbr;
    ltm::StatusOr<ltm::RecordRef> found = tree.Search(probe);
    if (!found.ok()) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "key=%llu: %s", static_cast<unsigned long long>(kv.first),
                    found.status().ToString().c_str());
      return Fail("Search missed", buf);
    }
    const ltm::RecordRef& r = found.value();
    if (r.key != kv.first || !SameMbr(r.mbr, kv.second.mbr) ||
        std::string(r.payload, r.payload_len) != kv.second.payload) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "key=%llu: returned content disagrees with the truth table",
                    static_cast<unsigned long long>(kv.first));
      return Fail("Search returned the wrong content", buf);
    }
  }

  // 不存在的键必须报 kNotFound（"找不到"不能伪装成"找到了"）
  {
    ltm::IndexEntry probe;
    probe.key = key_space + 12345;
    probe.mbr = random_mbr();
    ltm::StatusOr<ltm::RecordRef> found = tree.Search(probe);
    if (found.ok() || found.status().code() != ltm::StatusCode::kNotFound) {
      return Fail("Search for a missing key", "expected kNotFound, got: " +
                                          (found.ok() ? std::string("hit") :
                                                        found.status().ToString()));
    }
  }

  // -------------------------------------------------------------------------
  // 检查 3：空间相交查询与暴力过滤等价
  // -------------------------------------------------------------------------
  uint64_t contain_checked = 0;  // 包含谓词非空命中的记录数（防"空集假绿"）
  for (int round = 0; round < 8; ++round) {
    std::vector<double> lo(args.dim), hi(args.dim);
    std::vector<double> blo(args.dim), bhi(args.dim);
    for (uint16_t d = 0; d < args.dim; ++d) {
      const double a = coord(rng);
      lo[d] = a;
      hi[d] = a + 40.0 + coord(rng) * 0.25;
      // 包含谓词需要一个大得多的框才不至于空集（记录 MBR 边长最大 10）
      blo[d] = a;
      bhi[d] = a + 300.0 + coord(rng) * 0.25;
    }
    const ltm::Mbr window = MakeMbr(args.dim, lo, hi);
    const ltm::Mbr big_window = MakeMbr(args.dim, blo, bhi);

    std::set<uint64_t> expected;
    for (const auto& kv : truth) {
      if (kv.second.mbr.Intersects(window)) {
        expected.insert(kv.first);
      }
    }
    std::set<uint64_t> actual;
    ltm::Query q;
    q.kind = ltm::QueryKind::kIntersect;
    q.mbr = window;
    const ltm::Status qs = tree.SpatialQuery(q, [&actual](const ltm::RecordRef& r) {
      actual.insert(r.key);
      return true;
    });
    if (!qs.ok()) {
      return Fail("SpatialQuery(kIntersect) failed", qs.ToString());
    }
    if (actual != expected) {
      char buf[192];
      std::snprintf(buf, sizeof(buf), "round %d: expected %zu entries, got %zu", round, expected.size(),
                    actual.size());
      return Fail("the spatial intersect query disagrees with brute-force filtering", buf);
    }

    // 包含谓词（kRangeSpatial）走的是另一条判据分支：**查询框包含记录 MBR**。
    // 它与相交只差一个包含关系，最容易被写成同一个（那样"包含"会多返回一批
    // 只是相交的记录，而且不会有任何不变量报警）。这里用大框保证非空。
    std::set<uint64_t> exp_contain;
    for (const auto& kv : truth) {
      if (big_window.Contains(kv.second.mbr)) {
        exp_contain.insert(kv.first);
      }
    }
    std::set<uint64_t> act_contain;
    ltm::Query qc;
    qc.kind = ltm::QueryKind::kRangeSpatial;
    qc.mbr = big_window;
    const ltm::Status cs = tree.SpatialQuery(qc, [&act_contain](const ltm::RecordRef& r) {
      act_contain.insert(r.key);
      return true;
    });
    if (!cs.ok()) {
      return Fail("SpatialQuery(kRangeSpatial) failed", cs.ToString());
    }
    if (act_contain != exp_contain) {
      char buf[192];
      std::snprintf(buf, sizeof(buf), "round %d: contains predicate expected %zu, got %zu (intersect %zu)",
                    round, exp_contain.size(), act_contain.size(), expected.size());
      return Fail("the spatial contains query disagrees with brute-force filtering", buf);
    }
    contain_checked += exp_contain.size();
  }
  if (contain_checked == 0) {
    return Fail("the contains predicate degenerated to the empty set", "none of the 8 query rounds contained a single record, so nothing was tested");
  }

  // -------------------------------------------------------------------------
  // 检查 4：KNN best-first 与"真值表暴力扫描"等价（独立预言机，不只跟 KnnBruteForce 对拍）
  // -------------------------------------------------------------------------
  const uint32_t k = static_cast<uint32_t>(std::min<size_t>(10, truth.size()));
  for (int round = 0; round < 5; ++round) {
    std::vector<double> c(args.dim);
    for (uint16_t d = 0; d < args.dim; ++d) {
      c[d] = coord(rng);
    }
    const ltm::Point probe = MakePoint(args.dim, c);

    std::vector<double> oracle;
    oracle.reserve(truth.size());
    for (const auto& kv : truth) {
      oracle.push_back(kv.second.mbr.DistanceTo(probe));
    }
    std::sort(oracle.begin(), oracle.end());
    oracle.resize(k);

    std::vector<double> fast;
    std::vector<uint64_t> fast_keys;
    ltm::Query q;
    q.kind = ltm::QueryKind::kKnn;
    q.probe = probe;
    q.k = k;
    const ltm::Status ks = tree.Knn(q, [&](const ltm::RecordRef& r, double dist) {
      fast.push_back(dist);
      fast_keys.push_back(r.key);
      return true;
    });
    if (!ks.ok()) {
      return Fail("Knn failed", ks.ToString());
    }
    if (fast.size() != oracle.size()) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "round %d: expected %zu entries, got %zu", round, oracle.size(),
                    fast.size());
      return Fail("KNN result counts differ", buf);
    }
    for (size_t i = 0; i < fast.size(); ++i) {
      if (std::fabs(fast[i] - oracle[i]) > kEps) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "round %d, distance %zu: %.12f vs %.12f", round, i, fast[i],
                      oracle[i]);
        return Fail("KNN disagrees with the brute-force scan", buf);
      }
    }

    // 同一棵树内的第二条独立路径：叶链暴力（KnnBruteForce）。
    // 光跟"真值表预言机"对拍只能证明答案对；跟 KnnBruteForce 对拍才能证明
    // **best-first 剪枝没有剪过头**（两者用完全不同的遍历，答案必须逐元素相同）。
    // 顺带把 brute_pages 填上 —— 剪枝率的分母来自这里，不调用它分母恒为 0，
    // 报告里的"剪枝率"会永远显示 0%（假绿）。
    std::vector<std::pair<uint64_t, double>> alt;
    const ltm::Status bs =
        tree.KnnBruteForce(q, [&alt](const ltm::RecordRef& r, double dist) {
          alt.emplace_back(r.key, dist);
          return true;
        });
    if (!bs.ok()) {
      return Fail("KnnBruteForce failed", bs.ToString());
    }
    if (alt.size() != fast.size()) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "round %d: best-first %zu entries, leaf-chain brute force %zu", round,
                    fast.size(), alt.size());
      return Fail("the two KNN implementations returned different counts", buf);
    }
    for (size_t i = 0; i < alt.size(); ++i) {
      if (alt[i].first != fast_keys[i] || std::fabs(alt[i].second - fast[i]) > kEps) {
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "round %d, item %zu: (key=%llu, %.12f) vs (key=%llu, %.12f)", round, i,
                      static_cast<unsigned long long>(fast_keys[i]), fast[i],
                      static_cast<unsigned long long>(alt[i].first), alt[i].second);
        return Fail("KNN best-first disagrees with leaf-chain brute force", buf);
      }
    }
  }

  // -------------------------------------------------------------------------
  // 检查 5：不变量自检（I9 MBR 精确包含 / I4 等长 / 填充率）
  // -------------------------------------------------------------------------
  s = tree.Validate();
  if (!s.ok()) {
    return Fail("invariant validation failed", s.ToString());
  }

  const ltm::TreeStats st = tree.stats();
  // 清空阶段之后树会变小，这里不再要求 height>=2（那是**装载后**的护栏，
  // 已在 st_loaded 处判定）；只要求填充率仍然满足 I3。
  if (st.avg_fill_rate < 0.40) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "avg fill %.1f%% < 40%% (invariant I3)", st.avg_fill_rate * 100.0);
    return Fail("fill rate too low", buf);
  }

  const ltm::RTree::KnnProbeStats ks = tree.knn_stats();
  const double prune = ks.brute_pages == 0
                           ? 0.0
                           : 1.0 - static_cast<double>(ks.pages_visited) /
                                       static_cast<double>(ks.brute_pages);
  std::printf("R tree validation passed\n");
  std::printf("  params   : ops=%llu capacity=%u frames=%u dim=%u policy=%s seed=%llu\n",
              static_cast<unsigned long long>(args.ops), args.capacity, args.frames, args.dim,
              args.policy.c_str(), static_cast<unsigned long long>(args.seed));
  std::printf("  loaded   : height=%u pages=%llu entries=%llu avg fill %.1f%%\n",
              st_loaded.height, static_cast<unsigned long long>(st_loaded.num_pages),
              static_cast<unsigned long long>(st_loaded.num_entries),
              st_loaded.avg_fill_rate * 100.0);
  std::printf("  emptied  : height=%u pages=%llu entries=%llu avg fill %.1f%%\n", st.height,
              static_cast<unsigned long long>(st.num_pages),
              static_cast<unsigned long long>(st.num_entries), st.avg_fill_rate * 100.0);
  std::printf("  size     : height=%u pages=%llu entries=%llu (inserts %llu / overwrites %llu / erases %llu)\n",
              st.height, static_cast<unsigned long long>(st.num_pages),
              static_cast<unsigned long long>(st.num_entries),
              static_cast<unsigned long long>(inserts),
              static_cast<unsigned long long>(overwrites),
              static_cast<unsigned long long>(erases));
  std::printf("  structure: splits %llu / merges %llu / borrows %llu, avg fill %.1f%%\n",
              static_cast<unsigned long long>(st.num_splits),
              static_cast<unsigned long long>(st.num_merges),
              static_cast<unsigned long long>(st.num_borrows), st.avg_fill_rate * 100.0);
  std::printf("  space    : avg internal-page MBR overlap area %.4f\n", st.avg_overlap_area);
  std::printf("  KNN      : visited %llu pages / brute force %llu pages, prune %.1f%% (scanned %llu entries)\n",
              static_cast<unsigned long long>(ks.pages_visited),
              static_cast<unsigned long long>(ks.brute_pages), prune * 100.0,
              static_cast<unsigned long long>(ks.entries_scanned));
  std::printf("  buffer   : hit rate %.1f%%, evictions %llu (dirty %llu)\n", pool.hit_rate() * 100.0,
              static_cast<unsigned long long>(pool.stats().evictions),
              static_cast<unsigned long long>(pool.stats().dirty_evictions));
  // 见 test_bplus_validate.cc 同处的说明：采集侧的四条漏洞计数要单独可见。
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
  std::printf("  checks   : I9 MBR exact union, I3 fill >= 40%%, I4 equal path lengths, ");
  std::printf("ScanAll/Search/SpatialQuery/KNN agree with the truth table\n");

  cleanup.completed = true;
  return 0;
}

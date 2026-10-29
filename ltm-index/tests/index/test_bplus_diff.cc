// tests/index/test_bplus_diff.cc
// B+ 树随机差分测试 + 不变量自检（方案 §5.1 的 M2 必做项）。
//
// 做法：用 std::map<uint64_t, std::string> 作为"真值表"，对同一组随机操作序列
// 分别作用在两边，然后逐步比较全量遍历结果。这是发现页式实现错误最有效的手段。
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {

struct Fixture {
  std::string page_file;
  std::string wal_file;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<WalWriter> wal;
  std::unique_ptr<BufferPool> pool;
  BPlusPageOps ops{32};
  BPlusTree tree;

  explicit Fixture(const std::string& tag, uint32_t frames = 64, uint16_t capacity = 32)
      : page_file("log/_bplus_" + tag + ".data"), wal_file("log/_bplus_" + tag + ".wal"),
        ops(capacity) {
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
    dopt.num_pages = 1 << 14;
    disk.reset(new DiskManager(dopt));
    WalOptions wopt;
    wal.reset(new WalWriter(wopt, disk.get()));
    BufferPoolOptions popt;
    popt.pool_frames = frames;
    pool.reset(new BufferPool(popt, disk.get(), wal.get(), nullptr, nullptr, nullptr));

    TreeBuildOptions topt;
    topt.order_or_capacity = capacity;
    Status s = tree.Open(pool.get(), &ops, topt, /*tree_id=*/1);
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

std::string MakePayload(uint64_t key, size_t len) {
  std::string s;
  s.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    s.push_back(static_cast<char>('a' + ((key + i) % 26)));
  }
  return s;
}

IndexEntry MakeEntry(uint64_t key, const std::string& payload) {
  IndexEntry e;
  e.key = key;
  e.mbr = Mbr::Invalid(0);  // 纯一维记录
  e.payload.assign(payload.begin(), payload.end());
  return e;
}

}  // namespace

TEST(BPlusDiff, SequentialInsertThenScan) {
  Fixture f("seq");
  std::map<uint64_t, std::string> truth;
  for (uint64_t k = 0; k < 500; ++k) {
    const std::string p = MakePayload(k, 8 + (k % 20));
    truth[k] = p;
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, p)).ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());

  std::vector<std::pair<uint64_t, std::string>> got;
  ASSERT_TRUE(f.tree.ScanAll([&got](const RecordRef& r) {
    got.emplace_back(r.key, std::string(r.payload, r.payload_len));
    return true;
  }).ok());
  ASSERT_EQ(got.size(), truth.size());
  size_t i = 0;
  for (const auto& kv : truth) {
    EXPECT_EQ(got[i].first, kv.first);
    EXPECT_EQ(got[i].second, kv.second);
    ++i;
  }
}

TEST(BPlusDiff, RandomInsertEraseMatchesStdMap) {
  Fixture f("rand", /*frames=*/32, /*capacity=*/16);
  std::map<uint64_t, std::string> truth;
  std::mt19937_64 rng(20240926);
  std::uniform_int_distribution<uint64_t> key_dist(0, 4000);

  for (int step = 0; step < 6000; ++step) {
    const uint64_t k = key_dist(rng);
    const bool do_insert = (rng() % 100) < 60 || truth.empty();
    if (do_insert) {
      const std::string p = MakePayload(k, 1 + (rng() % 40));
      truth[k] = p;
      const Status s = f.tree.Insert(MakeEntry(k, p));
      if (!s.ok()) {
        std::printf("  [err] Insert key=%llu failed: %s\n", static_cast<unsigned long long>(k),
                    s.ToString().c_str());
      }
      ASSERT_TRUE(s.ok());
    } else {
      // 随机删一个存在的键
      auto it = truth.lower_bound(k);
      if (it == truth.end()) {
        it = truth.begin();
      }
      const uint64_t victim = it->first;
      truth.erase(it);
      const Status s = f.tree.Erase(MakeEntry(victim, ""));
      if (!s.ok()) {
        std::printf("  [err] Erase key=%llu failed: %s\n", static_cast<unsigned long long>(victim),
                    s.ToString().c_str());
      }
      ASSERT_TRUE(s.ok());
    }

    // 每隔一段做一次小规模一致性抽查
    if (step % 500 == 0) {
      const Status pv = f.tree.Validate();
      if (!pv.ok()) {
        std::printf("  [err] periodic Validate failed at step %d: %s\n", step, pv.ToString().c_str());
      }
      ASSERT_TRUE(pv.ok());
    }
  }

  const Status vs = f.tree.Validate();
  if (!vs.ok()) {
    std::printf("  [err] Validate failed: %s\n", vs.ToString().c_str());
  }
  ASSERT_TRUE(vs.ok());

  // 全量比较
  std::vector<std::pair<uint64_t, std::string>> got;
  ASSERT_TRUE(f.tree.ScanAll([&got](const RecordRef& r) {
    got.emplace_back(r.key, std::string(r.payload, r.payload_len));
    return true;
  }).ok());
  ASSERT_EQ(got.size(), truth.size());
  size_t i = 0;
  for (const auto& kv : truth) {
    ASSERT_TRUE(i < got.size());
    EXPECT_EQ(got[i].first, kv.first);
    EXPECT_EQ(got[i].second, kv.second);
    ++i;
  }

  // 点查必须与真值表一致（存在的都能查到）
  for (const auto& kv : truth) {
    auto r = f.tree.Search(MakeEntry(kv.first, ""));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(std::string(r.value().payload, r.value().payload_len), kv.second);
  }
  std::printf("  [info] diff test done: final entries %zu, height %u, splits %llu, merges %llu, borrows %llu\n",
              truth.size(), f.tree.stats().height,
              static_cast<unsigned long long>(f.tree.stats().num_splits),
              static_cast<unsigned long long>(f.tree.stats().num_merges),
              static_cast<unsigned long long>(f.tree.stats().num_borrows));
}

TEST(BPlusDiff, EraseDownToEmptyThenReinsert) {
  Fixture f("shrink", /*frames=*/16, /*capacity=*/8);
  std::map<uint64_t, std::string> truth;
  for (uint64_t k = 0; k < 300; ++k) {
    const std::string p = MakePayload(k, 12);
    truth[k] = p;
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, p)).ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());

  // 逆序删除，逼出大量的借键/合并/根降级路径
  for (uint64_t k = 299; k < 300; --k) {
    truth.erase(k);
    ASSERT_TRUE(f.tree.Erase(MakeEntry(k, "")).ok());
    if (k % 50 == 0) {
      ASSERT_TRUE(f.tree.Validate().ok());
    }
    if (k == 0) {
      break;
    }
  }
  EXPECT_TRUE(truth.empty());
  ASSERT_TRUE(f.tree.Validate().ok());

  uint64_t left = 0;
  ASSERT_TRUE(f.tree.ScanAll([&left](const RecordRef&) {
    ++left;
    return true;
  }).ok());
  EXPECT_EQ(left, 0u);
  EXPECT_EQ(f.tree.stats().height, 0u);  // 应当降级回空叶根

  // 空树再插回去
  for (uint64_t k = 0; k < 100; ++k) {
    const std::string p = MakePayload(k, 10);
    ASSERT_TRUE(f.tree.Insert(MakeEntry(k, p)).ok());
  }
  ASSERT_TRUE(f.tree.Validate().ok());
  uint64_t count = 0;
  ASSERT_TRUE(f.tree.ScanAll([&count](const RecordRef&) {
    ++count;
    return true;
  }).ok());
  EXPECT_EQ(count, 100u);
}

TEST(BPlusDiff, RangeScanRespectsBounds) {
  Fixture f("range", /*frames=*/16, /*capacity=*/8);
  for (uint64_t k = 0; k < 400; ++k) {
    const Status si = f.tree.Insert(MakeEntry(k, MakePayload(k, 6)));
    if (!si.ok()) {
      std::printf("  [err] Insert key=%llu failed: %s\n", static_cast<unsigned long long>(k),
                  si.ToString().c_str());
    }
    ASSERT_TRUE(si.ok());
  }
  Query q;
  q.kind = QueryKind::kRange1D;
  q.key_lo = 100;
  q.key_hi = 199;
  std::vector<uint64_t> seen;
  ASSERT_TRUE(f.tree.RangeScan(q, [&seen](const RecordRef& r) {
    seen.push_back(r.key);
    return true;
  }).ok());
  ASSERT_EQ(seen.size(), 100u);
  EXPECT_EQ(seen.front(), 100u);
  EXPECT_EQ(seen.back(), 199u);

  // 提前终止
  size_t n = 0;
  ASSERT_TRUE(f.tree.RangeScan(q, [&n](const RecordRef&) {
    ++n;
    return n < 5;
  }).ok());
  EXPECT_EQ(n, 5u);

  // 空区间
  q.key_lo = 1000;
  q.key_hi = 2000;
  size_t m = 0;
  ASSERT_TRUE(f.tree.RangeScan(q, [&m](const RecordRef&) {
    ++m;
    return true;
  }).ok());
  EXPECT_EQ(m, 0u);
}

TEST(BPlusDiff, DuplicateKeyOverwrites) {
  Fixture f("dup", /*frames=*/8, /*capacity=*/8);
  ASSERT_TRUE(f.tree.Insert(MakeEntry(7, "first")).ok());
  uint64_t count = 0;
  ASSERT_TRUE(f.tree.ScanAll([&count](const RecordRef&) {
    ++count;
    return true;
  }).ok());
  EXPECT_EQ(count, 1u);

  ASSERT_TRUE(f.tree.Insert(MakeEntry(7, "second-longer")).ok());
  count = 0;
  std::string value;
  ASSERT_TRUE(f.tree.ScanAll([&count, &value](const RecordRef& r) {
    ++count;
    value.assign(r.payload, r.payload_len);
    return true;
  }).ok());
  EXPECT_EQ(count, 1u);  // 覆盖而不是新增
  EXPECT_EQ(value, std::string("second-longer"));
  ASSERT_TRUE(f.tree.Validate().ok());
}

TEST(BPlusDiff, PureInsertKeepsFillRateAboveThreshold) {
  // 【回归钉子】分裂必须把被搬走的槽腾出的字节记为碎片并压实源页。
  //
  // 条目区从页尾向下生长，槽的 offset 取决于到达顺序、与键序无关；分裂丢掉的
  // 槽 [mid, n) 的偏移与保留槽交错，于是在保留槽之间留下**空洞**。而 free_space
  // 的口径是"最高地址 - 最低偏移"的跨度，空洞被算成已用 —— 实测一页 28 条、
  // 真实占用 897 字节却被记成 3931，free_space 只剩 5 字节。
  //
  // 后果是不变量被破坏而非仅仅浪费空间：页只装了 28/32 条就报 kPageFull，
  // 在 28 条处中点分裂成 14/14，两半都低于阈值 16，Validate 立刻报 I3。
  // 触发条件是**纯插入**（没有删除/合并去顺手压实页），所以混合负载的差分
  // 测试与 M2 验收都照不到它 —— 这条用例专门只插不删。
  Fixture f("pureins", /*frames=*/64, /*capacity=*/32);
  const uint64_t kOps = 4000;
  const uint64_t kKeySpace = kOps;  // 乘数是奇数且与键空间互素 → 键互不相同
  for (uint64_t i = 0; i < kOps; ++i) {
    ASSERT_TRUE(f.tree.Insert(MakeEntry((i * 2654435761u) % kKeySpace, MakePayload(i, 8 + (i % 25)))).ok());
    // 每 200 条自检一次：缺陷在页被多次分裂之后才显形（首次约在第 1389 条），
    // 只在末尾查一次会漏掉"中途坏过、后来被压实修好"的窗口。
    if (i % 200 == 0) {
      const Status vs = f.tree.Validate();
      if (!vs.ok()) {
        std::printf("self-check failed after pure-insert %llu: %s\n", static_cast<unsigned long long>(i),
                    vs.ToString().c_str());
        ASSERT_TRUE(vs.ok());
      }
    }
  }
  ASSERT_TRUE(f.tree.Validate().ok());
  const TreeStats st = f.tree.stats();  // 规模由 Validate 的遍历算出，必须在其后取
  EXPECT_EQ(st.num_entries, kOps);
  // 填充率必须高于阈值 —— 假 kPageFull 会把它压到阈值以下
  EXPECT_TRUE(st.avg_fill_rate >= 0.5);
}

TEST(BPlusDiff, SurvivesReopenWithSmallBufferPool) {
  // 关键场景：缓冲池远小于树占用的页数，必须靠淘汰/回读保证正确性
  const std::string tag = "reopen";
  const std::string page_file = "log/_bplus_" + tag + ".data";
  const std::string wal_file = "log/_bplus_" + tag + ".wal";
#if defined(_WIN32)
  std::system("if not exist log mkdir log");
#else
  std::system("mkdir -p log");
#endif
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());

  const uint64_t kN = 800;
  {
    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    dopt.num_pages = 1 << 13;
    DiskManager disk(dopt);
    WalOptions wopt;
    WalWriter wal(wopt, &disk);
    BufferPoolOptions popt;
    popt.pool_frames = 8;  // 故意极小
    BufferPool pool(popt, &disk, &wal, nullptr, nullptr, nullptr);
    BPlusPageOps ops(16);
    BPlusTree tree;
    TreeBuildOptions topt;
    topt.order_or_capacity = 16;
    ASSERT_TRUE(tree.Open(&pool, &ops, topt, 1).ok());
    for (uint64_t k = 0; k < kN; ++k) {
      const Status si = tree.Insert(MakeEntry(k, MakePayload(k, 10)));
      if (!si.ok()) {
        std::printf("  [err] Insert key=%llu failed: %s\n", static_cast<unsigned long long>(k),
                    si.ToString().c_str());
      }
      ASSERT_TRUE(si.ok());
    }
    ASSERT_TRUE(tree.Validate().ok());
    ASSERT_TRUE(pool.FlushAll().ok());
    const double hr = pool.hit_rate();
    std::printf("  [info] small pool: hit rate %.3f, evictions %llu, dirty evictions %llu\n", hr,
                static_cast<unsigned long long>(pool.stats().evictions),
                static_cast<unsigned long long>(pool.stats().dirty_evictions));
  }

  // 重新打开：数据必须完整
  {
    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    dopt.num_pages = 1 << 13;
    DiskManager disk(dopt);
    WalOptions wopt;
    WalWriter wal(wopt, &disk);
    BufferPoolOptions popt;
    popt.pool_frames = 8;
    BufferPool pool(popt, &disk, &wal, nullptr, nullptr, nullptr);
    BPlusPageOps ops(16);
    BPlusTree tree;
    TreeBuildOptions topt;
    topt.order_or_capacity = 16;
    ASSERT_TRUE(tree.Open(&pool, &ops, topt, 1).ok());
    ASSERT_TRUE(tree.Validate().ok());
    uint64_t count = 0;
    uint64_t expect_key = 0;
    bool ordered = true;
    ASSERT_TRUE(tree.ScanAll([&count, &expect_key, &ordered](const RecordRef& r) {
      if (r.key != expect_key) {
        ordered = false;
      }
      ++expect_key;
      ++count;
      return true;
    }).ok());
    EXPECT_EQ(count, kN);
    EXPECT_TRUE(ordered);
  }
  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());
}

int main(int argc, char** argv) {
  Logger::Instance().SetLevel(LogLevel::kError);
  return ltm::test::RunAll(argc, argv);
}

// tests/storage/test_recovery.cc
// M4-3：崩溃恢复（Analysis/Redo/Undo）与检查点的单元测试。
//
// 【这些用例在测什么】M4-2 只做到了"提交后强杀不丢已提交数据"，崩溃恢复本身是空的。
// 本文件把恢复的三条核心性质变成可重复执行的断言：
//   A 干净关闭 ⇒ 恢复必须是空操作（否则检查点压缩恢复时间这件事无从谈起）
//   B 未提交事务 ⇒ 恢复必须把它**回滚掉**（页级 REDO + 操作级 UNDO 的 UNDO 半边）
//   C 恢复本身可重入 ⇒ 连跑两次的结果与跑一次相同（CLR 存在的全部理由）
//   D 撕裂页/校验和损坏 ⇒ 恢复必须从 WAL 把它修回来，而不是被幂等规则"跳过"
//   E 检查点 ⇒ 日志被截断、数据不丢、恢复从 checkpoint_lsn 开始
//   F 日志头 ⇒ 往返正确；M4-2 的无头日志被明确拒绝（不猜起点）
//
// 【"崩溃"是怎么造出来的，为什么不用 FaultInjector】故障注入构建（build-fi）里
// 才有 FaultInjector，而单测跑在普通构建里 —— 用它的用例会静默变成空跑（本项目
// 缺陷 #37 就是这个形状）。所以这里用**公开 API 能构造出的真实崩溃现场**：
//   写事务开到一半（BeginWriteOp 之后不 Commit），然后让缓冲池析构。
// 析构会把脏页刷盘（等价于"崩溃前页已经落盘"），但**故意不声明干净关闭** ——
// 磁盘上的状态于是与"事务写到一半进程被强杀"逐字节一致。这不是模拟，是同一个状态。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/recovery.h"
#include "storage/wal.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {

constexpr uint32_t kTestFrames = 32;
constexpr uint16_t kTestCapacity = 32;

// ---------------------------------------------------------------------------
// 一个可以反复开关的测试环境：DiskManager -> RecoveryManager(持有 WalWriter)
// -> BufferPool -> B+ 树。
//
// 【构造顺序是契约】RecoveryManager 必须**先于** BufferPool 构造：它持有 WalWriter
// （同一条 LSN 序列），而缓冲池借用它写 WAL。销毁顺序相反：池先走（它的析构要在
// 日志还活着的时候写 clean_shutdown），然后才是恢复管理器与磁盘。
// ---------------------------------------------------------------------------
struct RecEnv {
  std::string page_file;
  std::string wal_file;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<RecoveryManager> rec;
  std::unique_ptr<BufferPool> pool;
  BPlusPageOps ops{kTestCapacity};
  BPlusTree tree;
  bool tree_open = false;

  RecEnv(const std::string& tag, bool wipe, uint32_t frames = kTestFrames)
      : page_file("log/_rec_" + tag + ".data"), wal_file("log/_rec_" + tag + ".wal") {
#if defined(_WIN32)
    std::system("if not exist log mkdir log");
#else
    std::system("mkdir -p log");
#endif
    if (wipe) {
      std::remove(page_file.c_str());
      std::remove(wal_file.c_str());
    }
    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    dopt.num_pages = 1u << 14;
    disk.reset(new DiskManager(dopt));
    WalOptions wopt;
    rec.reset(new RecoveryManager(wopt, disk.get()));
    BufferPoolOptions popt;
    popt.pool_frames = frames;
    pool.reset(new BufferPool(popt, disk.get(), rec->wal(), rec.get(), nullptr, nullptr));
  }

  ~RecEnv() {
    pool.reset();
    rec.reset();
    disk.reset();
  }

  Status OpenTree(uint16_t capacity = kTestCapacity, TreeId tree_id = 1) {
    TreeBuildOptions topt;
    topt.order_or_capacity = capacity;
    const Status s = tree.Open(pool.get(), &ops, topt, tree_id);
    tree_open = s.ok();
    return s;
  }

  Status Recover() { return rec->Recover(pool.get()); }

  void RemoveFiles() {
    std::remove(page_file.c_str());
    std::remove(wal_file.c_str());
  }

  RecEnv(const RecEnv&) = delete;
  RecEnv& operator=(const RecEnv&) = delete;
};

std::vector<char> Payload(uint64_t key, size_t len) {
  std::vector<char> s(len, 'x');
  for (size_t i = 0; i < len; ++i) {
    s[i] = static_cast<char>('a' + ((key + i) % 26));
  }
  return s;
}

IndexEntry MakeEntry(uint64_t key) {
  IndexEntry e;
  e.key = key;
  e.payload = Payload(key, 24);
  return e;
}

// 从树里收集全部键（走真实查询路径，不看内部状态）
std::vector<uint64_t> CollectKeys(BPlusTree& tree) {
  std::vector<uint64_t> keys;
  Query q;
  q.kind = QueryKind::kRange1D;
  q.key_lo = 0;
  q.key_hi = 0xFFFFFFFFFFFFFFFFull;
  const Status s = tree.RangeScan(q, [&keys](const RecordRef& r) {
    keys.push_back(r.key);
    return true;
  });
  if (!s.ok()) {
    std::printf("  RangeScan failed: %s\n", s.ToString().c_str());
  }
  return keys;
}

// 直接改页文件里的字节（测试专用）：把页的后半段清零 = 撕裂写
bool ZeroSecondHalf(const std::string& path, PageId pid) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) {
    return false;
  }
  std::vector<char> zeros(kPageSize / 2, 0);
  f.seekp(static_cast<std::streamoff>(pid) * kPageSize + kPageSize / 2);
  f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  f.flush();
  return static_cast<bool>(f);
}

// 直接改页文件里的字节（测试专用）：翻转页中部的一段 = 内容被改坏但头尾完好
bool FlipMiddleBytes(const std::string& path, PageId pid, uint32_t off, uint32_t len) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) {
    return false;
  }
  std::vector<char> buf(len, 0);
  f.seekg(static_cast<std::streamoff>(pid) * kPageSize + off);
  f.read(buf.data(), static_cast<std::streamsize>(len));
  for (char& c : buf) {
    c = static_cast<char>(c ^ 0xFF);
  }
  f.clear();
  f.seekp(static_cast<std::streamoff>(pid) * kPageSize + off);
  f.write(buf.data(), static_cast<std::streamsize>(len));
  f.flush();
  return static_cast<bool>(f);
}

// 整文件复制（测试专用）：用于"把数据文件回退到检查点那一刻"
bool CopyFileBytes(const std::string& from, const std::string& to) {
  std::ifstream in(from, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << in.rdbuf();
  return static_cast<bool>(out);
}

// 把文件截断到指定长度（测试专用：制造"日志只写到一半"的崩溃现场）
bool TruncateFileBytes(const std::string& path, uint64_t size) {
  std::error_code ec;
  std::filesystem::resize_file(path, size, ec);
  return !ec;
}

// 收集日志里所有被 REDO 覆盖的数据页（撕裂页用例需要"这些页确实有 REDO"，
// 否则损坏根本不会被重放覆盖，用例就变成了空跑）
std::vector<PageId> RedoDataPages(DiskManager* disk) {
  std::vector<PageId> pages;
  const Status s = ForEachWalRecord(disk, [&pages](const WalRecordHeader& h, const char*) {
    if (h.type == WalRecordType::kPageRedo && h.page_id != kMetaPageId) {
      for (PageId p : pages) {
        if (p == h.page_id) {
          return true;
        }
      }
      pages.push_back(h.page_id);
    }
    return true;
  });
  if (!s.ok()) {
    std::printf("  WAL scan failed: %s\n", s.ToString().c_str());
  }
  return pages;
}

bool Contains(const std::vector<uint64_t>& v, uint64_t k) {
  for (uint64_t x : v) {
    if (x == k) {
      return true;
    }
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// F. 日志头的往返与格式判据
// ---------------------------------------------------------------------------
TEST(Recovery, F_LogHeaderRoundTripAndFormatGuards) {
  const std::string page_file = "log/_rec_hdr.data";
  const std::string wal_file = "log/_rec_hdr.wal";
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
  DiskManager disk(dopt);

  // 1) 空文件：没有头部（kNotFound），Ensure 之后就有了
  WalLogHeader h{};
  EXPECT_TRUE(ReadWalLogHeader(&disk, &h).code() == StatusCode::kNotFound);
  bool created = false;
  ASSERT_TRUE(EnsureWalLogHeader(&disk, &h, &created).ok());
  EXPECT_TRUE(created);
  EXPECT_EQ(disk.WalSize(), static_cast<uint64_t>(kWalDataStart));
  EXPECT_EQ(h.last_checkpoint_lsn, kInvalidLsn);
  EXPECT_EQ(h.clean_shutdown, static_cast<uint8_t>(0));

  // 2) 往返：写一个带检查点与干净关闭标记的头部，读回来必须逐字段相同
  WalLogHeader w{};
  w.last_checkpoint_lsn = 12345;
  w.clean_shutdown = 1;
  ASSERT_TRUE(WriteWalLogHeader(&disk, w).ok());
  WalLogHeader back{};
  ASSERT_TRUE(ReadWalLogHeader(&disk, &back).ok());
  EXPECT_EQ(back.last_checkpoint_lsn, static_cast<Lsn>(12345));
  EXPECT_EQ(back.clean_shutdown, static_cast<uint8_t>(1));

  // 3) 头部校验和必须真的起作用：改一个字节就该被判为损坏。
  //    这条很重要 —— last_checkpoint_lsn 被改坏会让恢复从一个错误的位置开始，
  //    而"少重放一段日志"是静默的。
  {
    std::fstream f(wal_file, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(f));
    char c = 0;
    f.seekg(16);  // last_checkpoint_lsn 的中间一个字节
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0x5A);
    f.clear();
    f.seekp(16);
    f.write(&c, 1);
    f.flush();
  }
  EXPECT_TRUE(ReadWalLogHeader(&disk, &back).code() == StatusCode::kCorruption);

  // 4) 无头部的旧日志（M4-2 布局）必须被拒绝，而不是"猜一个起点"继续跑
  std::remove(wal_file.c_str());
  {
    std::ofstream out(wal_file, std::ios::binary);
    std::vector<char> junk(kWalDataStart + 64, 'J');
    out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  }
  DiskManager disk2(dopt);
  WalLogHeader h2{};
  const Status s = EnsureWalLogHeader(&disk2, &h2, &created);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.code() == StatusCode::kCorruption);

  std::remove(page_file.c_str());
  std::remove(wal_file.c_str());
}

// ---------------------------------------------------------------------------
// A. 干净关闭 ⇒ 恢复是空操作
// ---------------------------------------------------------------------------
TEST(Recovery, A_CleanShutdownNeedsNoRedoAndNoUndo) {
  const std::string tag = "clean";
  uint64_t entries = 0;
  {
    RecEnv env(tag, /*wipe=*/true);
    ASSERT_TRUE(env.OpenTree().ok());
    for (uint64_t k = 0; k < 400; ++k) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    }
    entries = env.tree.stats().num_entries;
    ASSERT_TRUE(env.tree.Validate().ok());
    // 析构：FlushAll + MarkCleanShutdown（没有未提交的写事务）
  }
  {
    RecEnv env(tag, /*wipe=*/false);
    ASSERT_TRUE(env.Recover().ok());
    const RecoveryManager::Report& r = env.rec->report();
    EXPECT_TRUE(r.clean_shutdown);
    EXPECT_FALSE(r.recovery_needed);
    EXPECT_EQ(r.records_redone, static_cast<uint64_t>(0));
    EXPECT_EQ(r.txns_undone, static_cast<uint64_t>(0));
    ASSERT_TRUE(env.OpenTree().ok());
    EXPECT_EQ(env.tree.stats().num_entries, entries);
    ASSERT_TRUE(env.tree.Validate().ok());
    env.RemoveFiles();
  }
}

// ---------------------------------------------------------------------------
// B. 未提交事务必须被回滚
// ---------------------------------------------------------------------------
// 构造：先提交 400 条，再开一个**不提交**的写事务写 20 条，然后让池析构。
// 析构会把脏页刷盘（所以未提交的改动**真的到了盘上**，与崩溃现场一致），
// 但因为写事务还开着，它不会声明干净关闭。
TEST(Recovery, B_AnUncommittedTransactionIsRolledBackByUndo) {
  const std::string tag = "undo";
  const uint64_t kCommitted = 400;
  const uint64_t kUncommittedFirst = 100000;
  const uint64_t kUncommittedCount = 20;
  {
    RecEnv env(tag, /*wipe=*/true);
    ASSERT_TRUE(env.OpenTree().ok());
    for (uint64_t k = 0; k < kCommitted; ++k) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    }
    ASSERT_TRUE(env.pool->BeginWriteOp().ok());
    for (uint64_t i = 0; i < kUncommittedCount; ++i) {
      // tree.Insert 内部还会开一层 ScopedWriteOp；它可重入，内层不会提交，
      // 所以这些改动整体停在"未提交"状态。
      ASSERT_TRUE(env.tree.Insert(MakeEntry(kUncommittedFirst + i)).ok());
    }
    // 故意不 CommitWriteOp：这就是崩溃现场
  }
  {
    RecEnv env(tag, /*wipe=*/false);
    ASSERT_TRUE(env.Recover().ok());
    const RecoveryManager::Report& r = env.rec->report();
    EXPECT_FALSE(r.clean_shutdown);
    EXPECT_TRUE(r.recovery_needed);
    EXPECT_GE(r.losers, static_cast<uint64_t>(1));
    EXPECT_GE(r.undo_records_applied, static_cast<uint64_t>(1));
    EXPECT_EQ(r.clrs_written, r.undo_records_applied);
    ASSERT_TRUE(env.OpenTree().ok());
    const std::vector<uint64_t> keys = CollectKeys(env.tree);
    EXPECT_EQ(keys.size(), static_cast<size_t>(kCommitted));
    EXPECT_FALSE(Contains(keys, kUncommittedFirst));
    EXPECT_FALSE(Contains(keys, kUncommittedFirst + kUncommittedCount - 1));
    ASSERT_TRUE(env.tree.Validate().ok());
    env.RemoveFiles();
  }
}

// ---------------------------------------------------------------------------
// C. 恢复可重入：连跑两次 == 跑一次
// ---------------------------------------------------------------------------
// 【CLR 的全部理由】回滚如果只改页、不写补偿记录，页头就会带着"未提交事务那条更新"
// 的 LSN；第二次恢复的 Redo 于是认为"这一页已经是那个状态了"而跳过，Undo 又因为
// 事务已写 kAbort 不再回滚 —— 未提交的数据会**永久留下**。CLR 让页的 LSN 前进到
// 回滚之后，第二次恢复既不会重放旧更新，也不需要再回滚。
TEST(Recovery, C_RecoveryIsIdempotentWhenRunTwice) {
  const std::string tag = "idem";
  {
    RecEnv env(tag, /*wipe=*/true);
    ASSERT_TRUE(env.OpenTree().ok());
    for (uint64_t k = 0; k < 200; ++k) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    }
    ASSERT_TRUE(env.pool->BeginWriteOp().ok());
    for (uint64_t i = 0; i < 10; ++i) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(5000 + i)).ok());
    }
  }
  std::vector<uint64_t> after_first;
  {
    RecEnv env(tag, /*wipe=*/false);
    ASSERT_TRUE(env.Recover().ok());
    ASSERT_TRUE(env.OpenTree().ok());
    after_first = CollectKeys(env.tree);
    ASSERT_TRUE(env.tree.Validate().ok());
    // 池析构时写事务已关，会被标成干净关闭
  }
  {
    RecEnv env(tag, /*wipe=*/false);
    ASSERT_TRUE(env.Recover().ok());
    const RecoveryManager::Report& r = env.rec->report();
    // 第二次打开时日志头已被标成干净关闭，且没有 loser ⇒ 空操作
    EXPECT_FALSE(r.recovery_needed);
    EXPECT_EQ(r.records_redone, static_cast<uint64_t>(0));
    ASSERT_TRUE(env.OpenTree().ok());
    const std::vector<uint64_t> after_second = CollectKeys(env.tree);
    EXPECT_EQ(after_first.size(), after_second.size());
    EXPECT_EQ(after_first.size(), static_cast<size_t>(200));
    ASSERT_TRUE(env.tree.Validate().ok());
    env.RemoveFiles();
  }
}

// ---------------------------------------------------------------------------
// D. 撕裂页与校验和损坏都必须被恢复修回来
// ---------------------------------------------------------------------------
TEST(Recovery, D_DamagedPagesAreRepairedFromTheLog) {
  const std::string tag = "repair";
  const uint64_t kKeys = 400;
  PageId torn = kInvalidPageId;
  PageId flipped = kInvalidPageId;
  {
    RecEnv env(tag, /*wipe=*/true);
    ASSERT_TRUE(env.OpenTree().ok());
    for (uint64_t k = 0; k < kKeys; ++k) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    }
    // 再开一个未提交事务，保证恢复一定会走 Redo（否则干净关闭会短路掉整个恢复）
    ASSERT_TRUE(env.pool->BeginWriteOp().ok());
    for (uint64_t i = 0; i < 8; ++i) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(9000 + i)).ok());
    }
    ASSERT_TRUE(env.pool->FlushAll().ok());
    const std::vector<PageId> pages = RedoDataPages(env.disk.get());
    ASSERT_TRUE(pages.size() >= 2u);  // 少于两页，这条用例就是空跑
    torn = pages[0];
    flipped = pages[1];
  }
  // 破坏两个不同的页：一个撕裂（后半页清零），一个内容被改坏（校验和不符）
  const std::string page_file = "log/_rec_" + tag + ".data";
  ASSERT_TRUE(ZeroSecondHalf(page_file, torn));
  ASSERT_TRUE(FlipMiddleBytes(page_file, flipped, 128, 32));
  {
    RecEnv env(tag, /*wipe=*/false);
    ASSERT_TRUE(env.Recover().ok());
    const RecoveryManager::Report& r = env.rec->report();
    EXPECT_GE(r.torn_pages_repaired + r.checksum_repairs, static_cast<uint64_t>(1));
    ASSERT_TRUE(env.OpenTree().ok());
    const std::vector<uint64_t> keys = CollectKeys(env.tree);
    EXPECT_EQ(keys.size(), static_cast<size_t>(kKeys));
    ASSERT_TRUE(env.tree.Validate().ok());
    env.RemoveFiles();
  }
}

// ---------------------------------------------------------------------------
// E. 检查点：日志被截断、数据不丢、恢复从 checkpoint_lsn 开始
// ---------------------------------------------------------------------------
// 【崩溃现场是怎么精确造出来的】检查点做完时，页文件里是"检查点那一刻"的完整状态。
// 把页文件复制一份留底，让后续写入照常进行（并且被析构刷盘），最后**把留底覆盖回去**
// —— 磁盘状态于是与"检查点刚做完就断电"逐字节一致：检查点之前的改动在数据文件里，
// 检查点之后的改动只在 WAL 里。恢复必须从 checkpoint_lsn 开始把这些改动重做出来。
// 这比"依赖缓冲池恰好没有刷某些页"可靠得多：后者取决于缓冲池大小，会随实现漂移。
TEST(Recovery, E_CheckpointTruncatesTheLogAndKeepsTheData) {
  const std::string tag = "ckpt";
  const uint64_t kBefore = 300;
  const uint64_t kAfterCheckpoint = 50;
  const uint64_t kUncommitted = 5;
  uint64_t log_before = 0;
  uint64_t log_after = 0;
  Lsn ckpt_lsn = kInvalidLsn;
  const std::string page_file = "log/_rec_" + tag + ".data";
  const std::string snapshot = page_file + ".snap";
  std::remove(snapshot.c_str());
  {
    RecEnv env(tag, /*wipe=*/true);
    ASSERT_TRUE(env.OpenTree().ok());
    for (uint64_t k = 0; k < kBefore; ++k) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    }
    log_before = env.disk->WalSize();
    CheckpointManager ckpt(WalOptions(), env.disk.get(), env.rec.get(), env.pool.get());
    ASSERT_TRUE(ckpt.RunCheckpoint().ok());
    EXPECT_EQ(ckpt.num_checkpoints(), static_cast<uint64_t>(1));
    log_after = env.disk->WalSize();
    ckpt_lsn = env.rec->checkpoint_lsn();
    EXPECT_TRUE(ckpt_lsn != kInvalidLsn);
    // 截断必须真的发生，而且必须留下头部与检查点记录
    EXPECT_TRUE(log_after < log_before);
    EXPECT_GE(log_after, static_cast<uint64_t>(kWalDataStart));
    EXPECT_EQ(ckpt.last_kept_bytes(), log_after);
    // 截断后头部必须还在（TruncateWal 会冲掉保留区，检查点要负责重建它）
    WalLogHeader h{};
    ASSERT_TRUE(ReadWalLogHeader(env.disk.get(), &h).ok());
    EXPECT_EQ(h.last_checkpoint_lsn, ckpt_lsn);
    // 留底：这一刻的页文件 = "检查点刚刚落盘"
    ASSERT_TRUE(CopyFileBytes(page_file, snapshot));

    // 检查点之后再写一批（提交），再开一个不提交的事务 —— 关闭因此是不干净的
    for (uint64_t k = kBefore; k < kBefore + kAfterCheckpoint; ++k) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    }
    ASSERT_TRUE(env.pool->BeginWriteOp().ok());
    for (uint64_t i = 0; i < kUncommitted; ++i) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(70000 + i)).ok());
    }
  }
  // 断电：数据文件退回检查点那一刻（此后的一切只存在于 WAL 里）
  ASSERT_TRUE(CopyFileBytes(snapshot, page_file));
  {
    RecEnv env(tag, /*wipe=*/false);
    ASSERT_TRUE(env.Recover().ok());
    const RecoveryManager::Report& r = env.rec->report();
    EXPECT_EQ(r.checkpoint_lsn, ckpt_lsn);
    EXPECT_EQ(r.redo_start_lsn, ckpt_lsn);
    EXPECT_TRUE(r.recovery_needed);
    EXPECT_GE(r.records_redone, static_cast<uint64_t>(1));
    ASSERT_TRUE(env.OpenTree().ok());
    EXPECT_EQ(env.tree.stats().num_entries, kBefore + kAfterCheckpoint);
    const std::vector<uint64_t> keys = CollectKeys(env.tree);
    EXPECT_EQ(keys.size(), static_cast<size_t>(kBefore + kAfterCheckpoint));
    EXPECT_FALSE(Contains(keys, static_cast<uint64_t>(70000)));
    ASSERT_TRUE(env.tree.Validate().ok());
    env.RemoveFiles();
  }
  std::remove(snapshot.c_str());
  std::printf("  checkpoint truncated the log from %llu to %llu bytes (lsn %llu)\n",
              static_cast<unsigned long long>(log_before), static_cast<unsigned long long>(log_after),
              static_cast<unsigned long long>(ckpt_lsn));
}

// ---------------------------------------------------------------------------
// G. 在检查点之后的**每一个**日志前缀上崩溃，恢复后都必须自洽
// ---------------------------------------------------------------------------
// 【这是崩溃矩阵的确定性版本】故障注入只能撞到几个离散的计数点（第 n 次 WAL 追加、
// 第 n 次页刷盘），而"崩溃可以发生在日志的任意一个字节位置上"。本用例把这一点做实：
// 把日志截断到**每一条记录的边界**、以及**每条记录中间**（模拟写到一半被强杀），
// 每次都从同一份崩溃现场重新恢复一遍，断言 Validate() 全绿 —— 而 Validate() 会交叉
// 检查"页里数出来的条目数 == 元数据计数"，于是"REDO 落了盘而 UNDO 没落"这类
// **只有一半记录存在**的窗口会被当场抓住。
//
// 【为什么页文件快照取在检查点之后，而不是"跑完让池析构刷盘"】截断日志会造出
// **真实崩溃到不了的状态**：如果页文件里含有某些改动，而那些改动的日志记录被我们
// 截掉了，那就等于"数据文件比日志更超前"—— 而写前日志规则（FlushFrame 在写页之前
// 一定先 Sync）保证了真实的崩溃现场永远不会这样。用那种状态去测恢复，测的是
// "恢复能不能修复一个不可能发生的世界"，只会得到假失败。
// 检查点之后的快照则一定满足覆盖关系：检查点的 flush_all 已经把"此刻之前的一切"
// 写进数据文件，而描述这些内容的记录（begin/end）就在日志保留区里、位于所有
// 待测前缀之前。于是每个前缀都对应一个**可达**的崩溃现场。
//
// 这条用例的来历：崩溃矩阵先在 wal_append n=40 / random 上抓到了同一个 bug
// （页里 6 条、元数据说 5 条）。修法是把 UNDO 记录写在 REDO 之前（见 wal.cc 的注释）。
// 本用例把那个离散反例变成了几百个确定性的前缀反例。
TEST(Recovery, G_EveryWalPrefixRecoversToAConsistentState) {
  const std::string tag = "prefix";
  const uint64_t kCommitted = 40;
  const uint64_t kAfterCheckpoint = 10;
  const uint64_t kUncommitted = 5;
  const std::string page_file = "log/_rec_" + tag + ".data";
  const std::string wal_file = "log/_rec_" + tag + ".wal";
  const std::string page_snap = page_file + ".snap";
  const std::string wal_snap = wal_file + ".snap";
  for (const std::string& p : {page_file, wal_file, page_snap, wal_snap}) {
    std::remove(p.c_str());
  }
  uint64_t ckpt_offset = 0;
  {
    RecEnv env(tag, /*wipe=*/true);
    ASSERT_TRUE(env.OpenTree().ok());
    for (uint64_t k = 0; k < kCommitted; ++k) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    }
    CheckpointManager ckpt(WalOptions(), env.disk.get(), env.rec.get(), env.pool.get());
    ASSERT_TRUE(ckpt.RunCheckpoint().ok());
    // 快照：数据文件 = 检查点那一刻的完整状态（覆盖关系成立）
    ASSERT_TRUE(CopyFileBytes(page_file, page_snap));
    ckpt_offset = env.disk->WalSize();
    // 检查点之后：先写一批已提交的，再开一个不提交的事务 —— 关闭因此是不干净的
    for (uint64_t k = kCommitted; k < kCommitted + kAfterCheckpoint; ++k) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(k)).ok());
    }
    ASSERT_TRUE(env.pool->BeginWriteOp().ok());
    for (uint64_t i = 0; i < kUncommitted; ++i) {
      ASSERT_TRUE(env.tree.Insert(MakeEntry(60000 + i)).ok());
    }
  }
  ASSERT_TRUE(CopyFileBytes(wal_file, wal_snap));

  // 记录边界（含每条记录中间的那个偏移），只取检查点之后的部分
  std::vector<uint64_t> cuts;
  {
    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    DiskManager disk(dopt);
    const uint64_t total = disk.WalSize();
    uint64_t off = kWalDataStart;
    while (off + kWalHeaderSize <= total) {
      char head[kWalHeaderSize];
      if (!disk.ReadWal(off, head, kWalHeaderSize).ok()) {
        break;
      }
      WalRecordHeader h{};
      std::memcpy(&h, head, kWalHeaderSize);
      if (h.total_len < kWalHeaderSize || off + h.total_len > total) {
        break;
      }
      off += h.total_len;
      if (off > ckpt_offset) {
        cuts.push_back(off);
        cuts.push_back(off - 1);  // 记录中间：撕裂的尾部
      }
    }
  }
  ASSERT_TRUE(cuts.size() > 50u);

  uint64_t checked = 0;
  uint64_t failed = 0;
  uint64_t nonempty = 0;
  for (uint64_t cut : cuts) {
    if (!CopyFileBytes(page_snap, page_file) || !CopyFileBytes(wal_snap, wal_file) ||
        !TruncateFileBytes(wal_file, cut)) {
      ++failed;
      continue;
    }
    RecEnv env(tag, /*wipe=*/false);
    const Status r = env.Recover();
    if (!r.ok()) {
      std::printf("  prefix %llu: recovery failed: %s\n", static_cast<unsigned long long>(cut),
                  r.ToString().c_str());
      ++failed;
      continue;
    }
    const Status o = env.OpenTree();
    if (!o.ok()) {
      std::printf("  prefix %llu: open failed: %s\n", static_cast<unsigned long long>(cut),
                  o.ToString().c_str());
      ++failed;
      continue;
    }
    const Status v = env.tree.Validate();
    if (!v.ok()) {
      std::printf("  prefix %llu: validate failed: %s\n", static_cast<unsigned long long>(cut),
                  v.ToString().c_str());
      ++failed;
    }
    if (env.tree.stats().num_entries > 0) {
      ++nonempty;
    }
    ++checked;
  }
  EXPECT_EQ(failed, 0u);
  EXPECT_EQ(checked, cuts.size());
  // 非平凡性护栏：如果绝大多数前缀恢复出来都是空树，这条用例等于没测
  EXPECT_GE(nonempty, cuts.size() / 3);
  std::printf("  %llu log prefixes recovered, %llu of them with a non-empty tree, %llu failed\n",
              static_cast<unsigned long long>(checked), static_cast<unsigned long long>(nonempty),
              static_cast<unsigned long long>(failed));
  for (const std::string& p : {page_file, wal_file, page_snap, wal_snap}) {
    std::remove(p.c_str());
  }
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

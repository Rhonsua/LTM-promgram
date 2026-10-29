// tests/concurrency/test_write_abort.cc
//
// M6 · C1 的前置条件：写事务 Abort 必须回滚**内存状态**，而不只是写一条 kAbort 记录。
//
// ---------------------------------------------------------------------------
// 【为什么这条判据必须先存在】
// docs/status.md「下一步」第 4 条点名：`ScopedWriteOp::Abort` 只写 kAbort、不回滚内存页。
// 崩溃恢复是对的（日志里有 kAbort，Undo 阶段会把它回滚掉），但**运行期**留下的是
// 一份与日志矛盾的索引。真实触发路径不是假设：
//
//   HybridIndex::Put 先写 R 树（空间副本）、再写 B+ 树（记录本体），两者罩在同一个
//   写事务里。第二步失败时 ScopedWriteOp 析构触发 Abort —— 此时 R 树那一条**留在
//   内存里**，B+ 侧没有对应记录，I8 双向交叉校验当场破裂，且该副本会被后续的
//   查询真的读到。
//
// C0 下写被独占闩串行化，这条还咬不到人；C1 一旦放开写并行它就是真缺陷。
// 所以它是 C1 的前置条件，不是一个可以往后排的 TODO。
//
// 【本用例是可证伪的 —— 这就是它存在的意义】
// 判据是"abort 之后，运行期内存里必须看不到被 abort 的键，且落盘重开后也看不到"。
// 修复前它**必须变红**，红输出留档在 report/abort_rollback/（见该目录的 README）。
// 没有这个红，"Abort 已回滚"就不能记成"通过" —— 见缺陷 #37（假绿）。
//
// 【为什么它抓得住，而 Validate() 抓不住】
// 被 abort 的事务在内存里留下的是一棵**结构完好**的树，只是多了若干条记录：
// 分裂/分裂回溯/链指针全都做完了。所以 Validate() 仍然全绿、I1..I9 一条不响 ——
// 这正是它危险的地方。能抓住它的只有"与事务前状态逐键比对"这种外部判据。
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "index/bplus_tree.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/meta_page.h"
#include "storage/wal.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;

namespace {

constexpr uint64_t kBaseKeys = 400;   // 事务开始前就存在的键
constexpr uint64_t kTxnKeys = 200;    // 事务里插入、随后被 abort 掉的键

IndexEntry MakeEntry(uint64_t key) {
  IndexEntry e;
  e.key = key;
  e.mbr = Mbr::Invalid(0);
  const std::string p = "v" + std::to_string(key);
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
  std::string tag;
  std::string page_file;
  std::string wal_file;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<WalWriter> wal;
  std::unique_ptr<BufferPool> pool;
  BPlusPageOps ops{64};
  BPlusTree tree;
  bool open_ok = false;
  uint32_t frames_ = 256;

  explicit Fixture(const std::string& t, uint32_t frames = 256) : tag(t), frames_(frames) {
#if defined(_WIN32)
    std::system("if not exist log mkdir log");
#else
    std::system("mkdir -p log");
#endif
    page_file = "log/_abort_" + tag + ".data";
    wal_file = "log/_abort_" + tag + ".wal";
    Remove();
    Open();
  }

  // 用同一对文件名重新建池建树 —— "落盘重开"就是靠它
  void Open() {
    DiskOptions dopt;
    dopt.page_file = page_file;
    dopt.wal_file = wal_file;
    dopt.num_pages = 1 << 12;
    disk.reset(new DiskManager(dopt));
    WalOptions wopt;
    wal.reset(new WalWriter(wopt, disk.get()));
    BufferPoolOptions popt;
    popt.pool_frames = frames_;
    pool.reset(new BufferPool(popt, disk.get(), wal.get(), nullptr, nullptr, nullptr));
    TreeBuildOptions topt;
    topt.order_or_capacity = 64;
    const Status s = tree.Open(pool.get(), &ops, topt, /*tree_id=*/1);
    open_ok = s.ok();
    if (!open_ok) {
      std::printf("Fixture open failed: %s\n", s.ToString().c_str());
    }
  }

  void Remove() {
    std::remove(page_file.c_str());
    std::remove(wal_file.c_str());
  }

  ~Fixture() {
    pool.reset();
    disk.reset();
    Remove();
  }
};

// 把 [lo, hi) 的键插进去；任一失败即记一笔失败
void InsertRange(BPlusTree& tree, uint64_t lo, uint64_t hi) {
  for (uint64_t k = lo; k < hi; ++k) {
    const Status s = tree.Insert(MakeEntry(k));
    if (!s.ok()) {
      LTM_FAIL_("Insert(" + std::to_string(k) + ") failed: " + s.ToString());
      return;
    }
  }
}

// 统计 [lo, hi) 里能被查到的键数。用浮点判断而不是 EXPECT_EQ，是为了让
// 失败信息里直接带上"漏了几个/多了几个"。
size_t CountPresent(BPlusTree& tree, uint64_t lo, uint64_t hi) {
  size_t n = 0;
  for (uint64_t k = lo; k < hi; ++k) {
    if (tree.Search(MakeProbe(k)).ok()) {
      ++n;
    }
  }
  return n;
}

// 基线段：事务开始前已提交的 400 条键（0..399）
void BuildBase(Fixture& f) { InsertRange(f.tree, 0, kBaseKeys); }

}  // namespace

// ===========================================================================
// 核心判据：Abort 之后，运行期内存里必须看不到被 abort 的键
// ===========================================================================
TEST(WriteAbort, RollsBackMemoryPages) {
  Fixture f("mem");
  ASSERT_TRUE(f.open_ok);
  BuildBase(f);

  ASSERT_EQ(size_t(0), CountPresent(f.tree, 100000, 100000 + kTxnKeys));

  // 一个外层写事务罩住内层 Insert：BPlusTree::Insert 自带一个 ScopedWriteOp，
  // 它只会把深度计数加一，真正的 Commit 由这里决定。所以在 AbortWriteOp 之前，
  // 这些记录**确实已经写进内存页**了。
  ASSERT_TRUE(f.pool->BeginWriteOp().ok());
  InsertRange(f.tree, 100000, 100000 + kTxnKeys);

  // 前提断言：证明这个用例真的改到了内存，而不是"什么都没发生所以当然查不到"。
  // 少了这一句，下面那条判据在"插入根本没生效"的情况下也会绿 —— 那是假绿。
  ASSERT_EQ(size_t(kTxnKeys), CountPresent(f.tree, 100000, 100000 + kTxnKeys));

  ASSERT_TRUE(f.pool->AbortWriteOp().ok());

  // ★ 判据主体
  const size_t still_there = CountPresent(f.tree, 100000, 100000 + kTxnKeys);
  EXPECT_EQ(size_t(0), still_there);

  // 基线段一条都不能少（回滚不能把事务前的记录一起roll掉）
  EXPECT_EQ(size_t(kBaseKeys), CountPresent(f.tree, 0, kBaseKeys));

  // 说明这条缺陷为什么静默：被 abort 的树在内存里结构是完好的
  const Status v = f.tree.Validate();
  if (!v.ok()) {
    std::printf("  Validate said: %s\n", v.ToString().c_str());
  }
  EXPECT_TRUE(v.ok());
}

// ===========================================================================
// 落盘面：abort 之后 FlushAll + 重开，同样不能看见被 abort 的键
//
// 这一条覆盖的是"内存回滚了、但脏帧已经把 after-image 写进盘"的另一半。
// 带缺陷时 FlushAll 会把 after-image 落盘，重开就能查到幽灵键。
// ===========================================================================
TEST(WriteAbort, AbortedStateDoesNotSurviveReopen) {
  Fixture f("reopen");
  ASSERT_TRUE(f.open_ok);
  BuildBase(f);

  ASSERT_TRUE(f.pool->BeginWriteOp().ok());
  InsertRange(f.tree, 100000, 100000 + kTxnKeys);
  ASSERT_TRUE(f.pool->AbortWriteOp().ok());

  ASSERT_TRUE(f.pool->FlushAll().ok());
  f.pool.reset();
  f.disk.reset();

  f.Open();
  ASSERT_TRUE(f.open_ok);

  EXPECT_EQ(size_t(0), CountPresent(f.tree, 100000, 100000 + kTxnKeys));
  EXPECT_EQ(size_t(kBaseKeys), CountPresent(f.tree, 0, kBaseKeys));
}

// ===========================================================================
// 反向对照：Commit 路径不能被"顺手改坏"
//
// 回滚和提交共用同一个 before-image 表。这一条盯的是"修回滚时把提交也一起
// 撤销了"这种过度修正 —— 没有它，一个把所有写事务都回滚掉实现也能让上面两条变绿。
// ===========================================================================
TEST(WriteAbort, CommitStillApplies) {
  Fixture f("commit");
  ASSERT_TRUE(f.open_ok);
  BuildBase(f);

  ASSERT_TRUE(f.pool->BeginWriteOp().ok());
  InsertRange(f.tree, 100000, 100000 + kTxnKeys);
  ASSERT_TRUE(f.pool->CommitWriteOp().ok());

  EXPECT_EQ(size_t(kTxnKeys), CountPresent(f.tree, 100000, 100000 + kTxnKeys));
  EXPECT_EQ(size_t(kBaseKeys), CountPresent(f.tree, 0, kBaseKeys));
  EXPECT_TRUE(f.tree.Validate().ok());
}

// ===========================================================================
// 分配器面：事务里 NewPage 过，abort 之后分配游标与空闲链也必须回到事务前
//
// 被回滚掉的页如果不还回去，next_unallocated 就永久前移 —— 恢复出来的空闲链
// 与实际页布局对不上（不变量 I7）。这一条读的是元数据页的内存镜像。
// ===========================================================================
TEST(WriteAbort, RollsBackAllocatorState) {
  Fixture f("alloc");
  ASSERT_TRUE(f.open_ok);
  // 塞够多的键，逼出至少一次分裂 ⇒ 事务里真的 NewPage 过
  BuildBase(f);
  ASSERT_TRUE(f.pool->EnsureMetaLoaded().ok());

  const uint32_t next_before = f.pool->meta().next_unallocated;
  const PageId head_before = f.pool->meta().free_list_head;

  ASSERT_TRUE(f.pool->BeginWriteOp().ok());
  // 400 条 base + 200 条事务键，capacity 64 ⇒ 必然分裂、必然分配新页
  InsertRange(f.tree, 100000, 100000 + kTxnKeys);

  ASSERT_TRUE(f.pool->EnsureMetaLoaded().ok());
  // 前提断言：事务里确实分配过页，否则这条判据是空转
  ASSERT_TRUE(next_before != f.pool->meta().next_unallocated);

  ASSERT_TRUE(f.pool->AbortWriteOp().ok());

  ASSERT_TRUE(f.pool->EnsureMetaLoaded().ok());
  EXPECT_EQ(next_before, f.pool->meta().next_unallocated);
  EXPECT_EQ(head_before, f.pool->meta().free_list_head);
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

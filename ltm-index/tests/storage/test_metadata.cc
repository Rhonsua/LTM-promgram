// Metadata bootstrap, rejection and WAL recovery boundaries.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include "index/bplus_tree.h"
#include "storage/meta_page.h"
#include "storage/recovery.h"
#include "tests/harness/test_harness.h"
#include "util/log.h"

using namespace ltm;
namespace {
struct Env {
  std::string data, wal_path;
  std::unique_ptr<DiskManager> disk;
  std::unique_ptr<RecoveryManager> rec;
  std::unique_ptr<BufferPool> pool;
  BPlusPageOps ops{8};
  BPlusTree tree;
  explicit Env(const std::string& tag, bool wipe) : data("log/_meta_" + tag + ".data"), wal_path("log/_meta_" + tag + ".wal") {
    std::filesystem::create_directories("log");
    if (wipe) { std::remove(data.c_str()); std::remove(wal_path.c_str()); }
    DiskOptions d; d.page_file = data; d.wal_file = wal_path; d.num_pages = 4096;
    disk.reset(new DiskManager(d)); rec.reset(new RecoveryManager(WalOptions(), disk.get()));
    BufferPoolOptions p; p.pool_frames = 16;
    pool.reset(new BufferPool(p, disk.get(), rec->wal(), rec.get()));
  }
  Status Open() {
    TreeBuildOptions o; o.order_or_capacity = 8; return tree.Open(pool.get(), &ops, o, 1);
  }
  ~Env() { pool.reset(); rec.reset(); disk.reset(); }
};
IndexEntry Record(uint64_t key) {
  IndexEntry e; e.key = key; const auto text = std::to_string(key) + ":metadata-test";
  e.payload.assign(text.begin(), text.end()); return e;
}
std::vector<char> Read(const std::string& path) {
  std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
bool Write(const std::string& path, const std::vector<char>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); return static_cast<bool>(f);
}
void Populate(Env* env) { for (uint64_t key = 0; key < 64; ++key) ASSERT_TRUE(env->tree.Insert(Record(key)).ok()); }
void Verify(Env* env) {
  ASSERT_TRUE(env->tree.Validate().ok());
  EXPECT_EQ(env->tree.stats().num_entries, 64ull);
  for (uint64_t key = 0; key < 64; ++key) {
    auto got = env->tree.Search(Record(key)); ASSERT_TRUE(got.ok()); const auto expected = Record(key);
    EXPECT_EQ(got.value().payload_len, static_cast<uint32_t>(expected.payload.size()));
    EXPECT_EQ(std::memcmp(got.value().payload, expected.payload.data(), expected.payload.size()), 0);
  }
}
}

TEST(Metadata, FreshDatabaseAndHealthyReopen) {
  { Env env("healthy", true); ASSERT_TRUE(env.rec->Recover(env.pool.get()).ok()); ASSERT_TRUE(env.Open().ok()); Populate(&env); }
  { Env env("healthy", false); ASSERT_TRUE(env.rec->Recover(env.pool.get()).ok()); ASSERT_TRUE(env.Open().ok()); Verify(&env); }
}
TEST(Metadata, ExistingDamageIsRejectedWithoutRewritingData) {
  for (int mode = 0; mode < 11; ++mode) {
    const auto tag = "damage" + std::to_string(mode); std::string path;
    { Env env(tag, true); path = env.data; ASSERT_TRUE(env.Open().ok()); Populate(&env);
      CheckpointManager checkpoint(WalOptions(), env.disk.get(), env.rec.get(), env.pool.get());
      ASSERT_TRUE(checkpoint.RunCheckpoint().ok()); }
    auto bytes = Read(path); ASSERT_TRUE(bytes.size() >= kPageSize); Page page(bytes.data());
    switch (mode) {
      case 0: page.mutable_header()->page_id = 1; break;
      case 1: bytes[kPageHeaderSize] ^= 1; break;
      case 2: bytes[kPageSize - 12] ^= 1; break;
      case 3: bytes[kPageSize - 1] ^= 1; break;
      case 4: page.mutable_header()->level = 3; page.UpdateChecksum(); break;
      case 5: page.mutable_header()->type = PageType::kBPlusLeaf; page.UpdateChecksum(); break;
      case 6: MetaOf(page.mutable_payload())->magic ^= 1; page.UpdateChecksum(); break;
      case 7: MetaOf(page.mutable_payload())->format_version = 2; page.UpdateChecksum(); break;
      case 8: std::fill(bytes.begin(), bytes.begin() + kPageSize, char{0}); break;
      case 9: page.mutable_header()->page_id = 1; page.UpdateChecksum(); break;
      case 10: bytes.resize(77); break;
    }
    ASSERT_TRUE(Write(path, bytes));
    { Env env(tag, false); ASSERT_TRUE(env.rec->Recover(env.pool.get()).ok());
      const auto status = env.Open(); EXPECT_FALSE(status.ok());
      EXPECT_TRUE(status.code() == StatusCode::kCorruption || status.code() == StatusCode::kChecksumMismatch); }
    EXPECT_TRUE(Read(path) == bytes);
  }
}
TEST(Metadata, EmptyOrMissingDataWithExistingWalIsNotANewDatabase) {
  for (int mode = 0; mode < 2; ++mode) {
    const auto tag = "missing" + std::to_string(mode); std::string data, wal_path;
    { Env env(tag, true); data = env.data; wal_path = env.wal_path; ASSERT_TRUE(env.Open().ok()); Populate(&env);
      CheckpointManager checkpoint(WalOptions(), env.disk.get(), env.rec.get(), env.pool.get());
      ASSERT_TRUE(checkpoint.RunCheckpoint().ok()); }
    ASSERT_TRUE(Write(data, {}));
    if (mode == 1) {
      // A valid checkpoint header still proves history when all records vanish.
      auto log = Read(wal_path); ASSERT_TRUE(log.size() >= kWalDataStart);
      log.resize(kWalDataStart); ASSERT_TRUE(Write(wal_path, log));
    }
    { Env env(tag, false); ASSERT_TRUE(env.rec->Recover(env.pool.get()).ok()); EXPECT_FALSE(env.Open().ok()); }
    EXPECT_TRUE(Read(data).empty());
  }
}
TEST(Metadata, UncleanMetadataDamageWithWalImagesIsRecovered) {
  { Env env("redo", true); ASSERT_TRUE(env.Open().ok()); Populate(&env); ASSERT_TRUE(env.pool->FlushAll().ok());
    env.pool.reset(); ASSERT_TRUE(env.rec->MarkUncleanShutdown().ok());
    std::vector<char> raw(kPageSize); ASSERT_TRUE(env.disk->ReadPage(0, raw.data()).ok()); raw[kPageHeaderSize] ^= 1;
    ASSERT_TRUE(env.disk->WritePage(0, raw.data()).ok()); ASSERT_TRUE(env.disk->SyncPageFile().ok()); }
  { Env env("redo", false); ASSERT_TRUE(env.rec->Recover(env.pool.get()).ok());
    EXPECT_TRUE(env.rec->report().checksum_repairs > 0); EXPECT_TRUE(env.rec->report().records_redone > 0);
    ASSERT_TRUE(env.Open().ok()); Verify(&env); }
}
TEST(Metadata, UncleanDamageAfterCheckpointWithoutImageIsRejected) {
  std::string path; std::vector<char> bytes;
  { Env env("no_image", true); path = env.data; ASSERT_TRUE(env.Open().ok()); Populate(&env);
    CheckpointManager checkpoint(WalOptions(), env.disk.get(), env.rec.get(), env.pool.get());
    ASSERT_TRUE(checkpoint.RunCheckpoint().ok()); env.pool.reset(); ASSERT_TRUE(env.rec->MarkUncleanShutdown().ok()); }
  bytes = Read(path); bytes[kPageHeaderSize] ^= 1; ASSERT_TRUE(Write(path, bytes));
  { Env env("no_image", false); ASSERT_TRUE(env.rec->Recover(env.pool.get()).ok());
    EXPECT_EQ(env.rec->report().records_redone, 0ull); EXPECT_FALSE(env.Open().ok()); }
  EXPECT_TRUE(Read(path) == bytes);
}
TEST(Metadata, InitialTreeRollbackCanBootstrapFromVerifiedClr) {
  { Env env("initial_undo", true); ASSERT_TRUE(env.pool->BeginWriteOp().ok()); ASSERT_TRUE(env.Open().ok());
    ASSERT_TRUE(env.pool->FlushAll().ok()); } // active outer transaction: destructor does not mark clean
  { Env env("initial_undo", false); ASSERT_TRUE(env.rec->Recover(env.pool.get()).ok());
    EXPECT_TRUE(env.rec->report().clrs_written > 0); ASSERT_TRUE(env.Open().ok());
    ASSERT_TRUE(env.tree.Validate().ok()); EXPECT_EQ(env.tree.stats().num_entries, 0ull);
    Populate(&env); Verify(&env); }
  { Env env("initial_undo", false); ASSERT_TRUE(env.rec->Recover(env.pool.get()).ok()); ASSERT_TRUE(env.Open().ok()); Verify(&env); }
}
TEST(Metadata, FakeBlankShellWithoutMatchingClrIsRejected) {
  Env env("fake_shell", true); std::vector<char> raw(kPageSize, 0); Page page(raw.data());
  page.mutable_header()->page_lsn = 123; page.UpdateChecksum();
  ASSERT_TRUE(env.disk->WritePage(0, raw.data()).ok()); ASSERT_TRUE(env.disk->SyncPageFile().ok());
  EXPECT_FALSE(env.Open().ok());
}
int main(int argc, char** argv) {
  Logger::Instance().SetLevel(LogLevel::kError); return test::RunAll(argc, argv);
}

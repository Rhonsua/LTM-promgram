#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "tests/harness/test_harness.h"
#include "util/config.h"

using namespace ltm;

namespace {

// 测试用临时文件目录：相对 ctest 的工作目录（build 目录）建立 log/，
// 不依赖调用者的当前目录，也不污染仓库。
std::string TempPath(const char* name) {
#if defined(_WIN32)
  std::system("if not exist log mkdir log");
#else
  std::system("mkdir -p log");
#endif
  return std::string("log/") + name;
}

}  // namespace

TEST(Config, DefaultsAreSane) {
  Config c = Config::Default();
  EXPECT_EQ(c.disk.page_size, 4096u);
  EXPECT_TRUE(c.pool.pool_frames > 0u);
  EXPECT_TRUE(c.pool.cold_threshold > 0);
  EXPECT_TRUE(c.wal.group_commit_records > 0u);
  EXPECT_TRUE(c.bench_ops > 0u);
  // 方案 §9.0 的默认值必须与文档一致
  EXPECT_EQ(c.pool.pool_frames, 1u << 12);
  EXPECT_EQ(static_cast<int>(c.pool.prefetch_depth), 2);
  EXPECT_EQ(static_cast<int>(c.pool.hot_delta), 8);
  EXPECT_EQ(static_cast<int>(c.pool.decay_shift), 3);
}

TEST(Config, SetAndGetRoundTrip) {
  Config c = Config::Default();
  ASSERT_TRUE(c.Set("disk.page_size", "8192").ok());
  EXPECT_EQ(c.disk.page_size, 8192u);
  EXPECT_EQ(c.Get("disk.page_size"), std::string("8192"));
  EXPECT_EQ(c.Get("disk.page_file"), c.disk.page_file);

  ASSERT_TRUE(c.Set("pool.enable_prefetch", "off").ok());
  EXPECT_FALSE(c.pool.enable_prefetch);
  EXPECT_EQ(c.Get("pool.enable_prefetch"), std::string("false"));

  ASSERT_TRUE(c.Set("migration.nonleaf_evict_penalty", "40").ok());
  EXPECT_EQ(c.migration.nonleaf_evict_penalty, 40);
}

TEST(Config, RejectsInvalidValues) {
  Config c = Config::Default();
  EXPECT_FALSE(c.Set("disk.page_size", "0").ok());       // 过小
  EXPECT_FALSE(c.Set("disk.page_size", "abc").ok());     // 非数字
  EXPECT_FALSE(c.Set("pool.pool_frames", "0").ok());     // 0 帧
  EXPECT_FALSE(c.Set("pool.cold_threshold", "300").ok());// 超 uint8
  EXPECT_FALSE(c.Set("pool.decay_shift", "9").ok());     // 位移过大
  EXPECT_FALSE(c.Set("pool.enable_prefetch", "maybe").ok());
  EXPECT_FALSE(c.Set("no.such.key", "1").ok());          // 未知键
  // 失败不得改变原值
  EXPECT_EQ(c.disk.page_size, 4096u);
}

TEST(Config, DumpCoversEveryKnownKey) {
  Config c = Config::Default();
  const std::string dump = c.Dump();
  EXPECT_TRUE(dump.find("disk.page_size") != std::string::npos);
  EXPECT_TRUE(dump.find("pool.pool_frames") != std::string::npos);
  EXPECT_TRUE(dump.find("migration.cold_threshold") != std::string::npos);
  EXPECT_TRUE(dump.find("bench.ops") != std::string::npos);
  // 实验记录要求：Dump() 必须能被 LoadFile() 无损读回
  const std::string path = TempPath("_test_config_dump.cfg");
  {
    std::ofstream ofs(path);
    ASSERT_TRUE(static_cast<bool>(ofs));
    ofs << dump;
  }
  Config c2 = Config::Default();
  Status s = c2.LoadFile(path);
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(c2.Get("pool.pool_frames"), c.Get("pool.pool_frames"));
  EXPECT_EQ(c2.Get("migration.cold_threshold"), c.Get("migration.cold_threshold"));
  std::remove(path.c_str());
}

TEST(Config, LoadFileHandlesCommentsAndErrors) {
  const std::string path = TempPath("_test_config_load.cfg");
  {
    std::ofstream ofs(path);
    ofs << "# a comment line\n"
        << "\n"
        << "this line deliberately has no equals sign\n";
  }
  Config c = Config::Default();
  Status s = c.LoadFile(path);
  // 第三行缺少 '='，必须报错并带行号
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.message().find("3") != std::string::npos);

  {
    std::ofstream ofs(path);
    ofs << "# a normal file\nbench.ops=1234\n";
  }
  Config c2 = Config::Default();
  EXPECT_TRUE(c2.LoadFile(path).ok());
  EXPECT_EQ(c2.bench_ops, 1234u);
  std::remove(path.c_str());
}

TEST(Config, CommandLineOverridesFile) {
  const std::string path = TempPath("_test_config_prio.cfg");
  {
    std::ofstream ofs(path);
    ofs << "pool.pool_frames=100\n";
  }
  const std::string cfg_path = TempPath("_test_config_prio.cfg");
  const std::string cfg_arg = "--config=" + cfg_path;
  const char* argv[] = {"prog", cfg_arg.c_str(), "--pool.pool_frames=999"};
  auto r = Config::FromCommandLine(3, const_cast<char**>(argv));
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().pool.pool_frames, 999u);  // 命令行优先于配置文件
  std::remove(path.c_str());
}

TEST(Config, LogLevelParsing) {
  Config c = Config::Default();
  ASSERT_TRUE(c.Set("log.level", "debug").ok());
  EXPECT_TRUE(c.log_level == LogLevel::kDebug);
  ASSERT_TRUE(c.Set("log.level", "warn").ok());
  EXPECT_TRUE(c.log_level == LogLevel::kWarn);
  ASSERT_TRUE(c.Set("log.level", "OFF").ok());
  EXPECT_TRUE(c.log_level == LogLevel::kOff);
  ASSERT_TRUE(c.Set("log.level", "none of the above").ok());  // 未知值回落 info，不报错
  EXPECT_TRUE(c.log_level == LogLevel::kInfo);
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

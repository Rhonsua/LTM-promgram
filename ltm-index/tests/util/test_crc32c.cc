#include <cstring>
#include <string>
#include <vector>

#include "tests/harness/test_harness.h"
#include "util/crc32c.h"

using namespace ltm;

TEST(Crc32c, KnownVectors) {
  // CRC-32C 标准测试向量
  EXPECT_EQ(Crc32c("123456789", 9), 0xE3069283u);
  // 空输入：初值 0xFFFFFFFF 异或末值 0xFFFFFFFF == 0
  EXPECT_EQ(Crc32c("", 0), 0u);
}

TEST(Crc32c, IncrementalEqualsOneShot) {
  const std::string data = "generic larger-than-memory tree index -- CRC32C incremental consistency";
  const uint32_t one_shot = Crc32c(data.data(), data.size());

  // 逐字节累加
  Crc32cHasher by_byte;
  for (char c : data) {
    by_byte.Update(&c, 1);
  }
  EXPECT_EQ(by_byte.value(), one_shot);

  // 任意切分点累加（覆盖 8 字节对齐与非对齐两种路径）
  for (size_t cut = 1; cut < data.size(); ++cut) {
    Crc32cHasher h;
    h.Update(data.data(), cut);
    h.Update(data.data() + cut, data.size() - cut);
    EXPECT_EQ(h.value(), one_shot);
  }
}

TEST(Crc32c, ValueIsIdempotentAndResetWorks) {
  const char* s = "abcdefghijklmnop";
  Crc32cHasher h;
  h.Update(s, 16);
  const uint32_t v1 = h.value();
  const uint32_t v2 = h.value();  // 再次取值为只读操作
  EXPECT_EQ(v1, v2);
  h.Reset();
  EXPECT_EQ(h.value(), 0u);  // Reset 后回到空输入的状态
  h.Update(s, 16);
  EXPECT_EQ(h.value(), v1);
}

TEST(Crc32c, DetectsSingleBitFlip) {
  std::vector<char> buf(4096, 'A');
  const uint32_t base = Crc32c(buf.data(), buf.size());
  for (size_t pos : {size_t(0), size_t(1), size_t(2048), size_t(4095)}) {
    buf[pos] = static_cast<char>(buf[pos] ^ 0x01);
    EXPECT_TRUE(Crc32c(buf.data(), buf.size()) != base);
    buf[pos] = static_cast<char>(buf[pos] ^ 0x01);
  }
  EXPECT_EQ(Crc32c(buf.data(), buf.size()), base);
}

TEST(Crc32c, DifferentLengthsDifferForZeroBuffer) {
  // 全零缓冲在不同长度下必须给出不同结果（页校验和的常见误用防线）
  std::vector<char> a(4096, 0);
  EXPECT_TRUE(Crc32c(a.data(), 4096) != Crc32c(a.data(), 4080));
}

TEST(Crc32c, HardwarePathReported) {
  // 只断言接口可用性；具体走哪条路径取决于编译与 CPU
  const bool hw = Crc32cUsingHardware();
  EXPECT_TRUE(hw == true || hw == false);
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

// src/util/crc32c.h
// CRC32C (Castagnoli, 多项式 0x1EDC6F41 反射形式 0x82F63B78)。
// 用途：页校验和（header + payload）、WAL 记录校验和。
// 硬件路径：x86 且编译期获得 SSE4.2 intrinsics 时使用 _mm_crc32_* 指令。
// 软件路径：运行时建表（256 项），保证跨平台可复现（同一输入必定同一输出）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace ltm {

// 一次性计算 [data, data+len) 的 CRC32C 最终值（含标准初值与末异或）
uint32_t Crc32c(const void* data, size_t len);

// 增量计算器：允许多段拼接得到与一次性计算完全相同的结果。
// 语义明确：内部保存"中间状态"，value() 只在最后做一次末异或，
// 避免 Crc32cExtend(prev, ...) 这类接口对 prev 含义产生歧义。
class Crc32cHasher {
 public:
  Crc32cHasher() = default;

  void Update(const void* data, size_t len);
  uint32_t value() const;  // 幂等，可在任意时刻调用

  void Reset() { state_ = kInitial; }

 private:
  static constexpr uint32_t kInitial = 0xFFFFFFFFu;
  uint32_t state_ = kInitial;
};

// 是否走硬件路径（构建时报告本机是否启用 SSE4.2 加速）
bool Crc32cUsingHardware();

}  // namespace ltm

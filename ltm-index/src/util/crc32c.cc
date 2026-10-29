#include "util/crc32c.h"

#include <array>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

// 编译期判定是否拿到 SSE4.2 intrinsics
#if (defined(_MSC_VER) && defined(_M_X64) && defined(__AVX__)) ||                \
    ((defined(__x86_64__) || defined(__i386__)) && defined(__SSE4_2__) &&        \
     (defined(__GNUC__) || defined(__clang__)))
#define LTM_CRC32C_HW_COMPILED 1
#endif

namespace ltm {
namespace {

constexpr uint32_t kPoly = 0x82F63B78u;  // CRC32C 反射多项式
constexpr uint32_t kInitial = 0xFFFFFFFFu;
constexpr uint32_t kFinalXor = 0xFFFFFFFFu;

struct Table {
  std::array<uint32_t, 256> t{};
  Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int k = 0; k < 8; ++k) {
        crc = (crc & 1u) ? ((crc >> 1) ^ kPoly) : (crc >> 1);
      }
      t[i] = crc;
    }
  }
};

const Table& GetTable() {
  static const Table table;
  return table;
}

#if defined(LTM_CRC32C_HW_COMPILED)
bool DetectHardware() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, 1);
  return (regs[2] & (1 << 20)) != 0;  // ECX bit 20 = SSE4.2
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
    return false;
  }
  return (ecx & (1u << 20)) != 0;
#else
  return false;
#endif
}
#endif

// 编译期可用硬件路径 且 运行期 CPU 支持，才真正走硬件
bool UseHardware() {
  static const bool usable =
#if defined(LTM_CRC32C_HW_COMPILED)
      DetectHardware();
#else
      false;
#endif
  return usable;
}

// 软件实现：crc 为"中间状态"（未做末异或）
uint32_t Software(uint32_t crc, const uint8_t* p, size_t len) {
  const Table& table = GetTable();
  for (size_t i = 0; i < len; ++i) {
    crc = table.t[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc;
}

#if defined(LTM_CRC32C_HW_COMPILED)
// 硬件实现：同样接受/返回"中间状态"。
// 关键事实：_mm_crc32_* 的语义与"未做末异或的 CRC32C 中间状态"完全一致，
// 因此可以安全地与软件状态互相衔接（本实现不混用，但保证语义等价）。
#if defined(_MSC_VER)
uint32_t Hardware(uint32_t crc, const uint8_t* p, size_t len) {
  while (len >= 8) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    crc = static_cast<uint32_t>(_mm_crc32_u64(crc, v));
    p += 8;
    len -= 8;
  }
  while (len > 0) {
    crc = _mm_crc32_u8(crc, *p);
    ++p;
    --len;
  }
  return crc;
}
#else
uint32_t Hardware(uint32_t crc, const uint8_t* p, size_t len) {
#if defined(__x86_64__)
  while (len >= 8) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    crc = static_cast<uint32_t>(__builtin_ia32_crc32di(crc, v));
    p += 8;
    len -= 8;
  }
#endif
  while (len > 0) {
    crc = __builtin_ia32_crc32qi(crc, *p);
    ++p;
    --len;
  }
  return crc;
}
#endif
#else
uint32_t Hardware(uint32_t crc, const uint8_t* p, size_t len) { return Software(crc, p, len); }
#endif

}  // namespace

void Crc32cHasher::Update(const void* data, size_t len) {
  if (len == 0) {
    return;
  }
  const uint8_t* p = static_cast<const uint8_t*>(data);
  state_ = UseHardware() ? Hardware(state_, p, len) : Software(state_, p, len);
}

uint32_t Crc32cHasher::value() const { return state_ ^ kFinalXor; }

uint32_t Crc32c(const void* data, size_t len) {
  Crc32cHasher h;
  h.Update(data, len);
  return h.value();
}

bool Crc32cUsingHardware() { return UseHardware(); }

// 供内部自检使用：软件路径永远可用，便于交叉验证硬件路径
namespace detail {
uint32_t Crc32cSoftwareReference(const void* data, size_t len) {
  return Software(kInitial, static_cast<const uint8_t*>(data), len) ^ kFinalXor;
}
}  // namespace detail

}  // namespace ltm

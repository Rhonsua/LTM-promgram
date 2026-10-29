#include "storage/fault_inject.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace ltm {

#if LTM_ENABLE_FAULT_INJECTION

namespace {
std::atomic<uint64_t> g_wal_append_calls{0};
std::atomic<uint64_t> g_page_flush_calls{0};
std::atomic<uint64_t> g_crash_after_wal{0};
std::atomic<uint64_t> g_crash_before_flush{0};
std::atomic<int> g_crash_ckpt_stage{0};

void Die(const char* where) {
  std::fprintf(stderr, "[FaultInjector] simulating a crash at %s (_exit(9), no flush)\n", where);
  std::fflush(stderr);
  std::_Exit(9);
}
}  // namespace

bool FaultInjector::enabled() { return true; }

void FaultInjector::Reset() {
  g_wal_append_calls.store(0);
  g_page_flush_calls.store(0);
  g_crash_after_wal.store(0);
  g_crash_before_flush.store(0);
  g_crash_ckpt_stage.store(0);
}

void FaultInjector::SetCrashAfterWalAppend(uint64_t n) { g_crash_after_wal.store(n); }
void FaultInjector::SetCrashBeforePageFlush(uint64_t n) { g_crash_before_flush.store(n); }
void FaultInjector::SetCrashDuringCheckpoint(int stage) { g_crash_ckpt_stage.store(stage); }

void FaultInjector::OnWalAppend() {
  const uint64_t c = g_wal_append_calls.fetch_add(1) + 1;
  const uint64_t target = g_crash_after_wal.load();
  if (target != 0 && c == target) {
    Die("after the WAL append");
  }
}

void FaultInjector::OnBeforePageFlush() {
  const uint64_t c = g_page_flush_calls.fetch_add(1) + 1;
  const uint64_t target = g_crash_before_flush.load();
  if (target != 0 && c == target) {
    Die("before the data page is flushed");
  }
}

void FaultInjector::OnCheckpointStage(int stage) {
  const int target = g_crash_ckpt_stage.load();
  if (target != 0 && stage == target) {
    Die("checkpoint phase");
  }
}

uint64_t FaultInjector::wal_append_calls() { return g_wal_append_calls.load(); }
uint64_t FaultInjector::page_flush_calls() { return g_page_flush_calls.load(); }

bool FaultInjector::CorruptPageBytes(const char* path, uint32_t page_size, PageId pid,
                                     uint32_t off, uint32_t len) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) {
    return false;
  }
  const std::streamoff pos = static_cast<std::streamoff>(pid) * page_size + off;
  f.seekp(pos);
  if (!f) {
    return false;
  }
  for (uint32_t i = 0; i < len; ++i) {
    char c = 0;
    f.seekg(pos + i);
    f.read(&c, 1);
    c = static_cast<char>(c ^ 0xFF);
    f.seekp(pos + i);
    f.write(&c, 1);
  }
  f.flush();
  return static_cast<bool>(f);
}

bool FaultInjector::TearPageWrite(const char* path, uint32_t page_size, PageId pid) {
  // 用零写掉页的后半部分：等价于"只写成功了一半"
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) {
    return false;
  }
  const std::streamoff half = static_cast<std::streamoff>(pid) * page_size + page_size / 2;
  f.seekp(half);
  std::vector<char> zeros(page_size / 2, 0);
  f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  f.flush();
  return static_cast<bool>(f);
}

#else  // 未启用故障注入：全部空操作

bool FaultInjector::enabled() { return false; }
void FaultInjector::Reset() {}
void FaultInjector::SetCrashAfterWalAppend(uint64_t) {}
void FaultInjector::SetCrashBeforePageFlush(uint64_t) {}
void FaultInjector::SetCrashDuringCheckpoint(int) {}
void FaultInjector::OnWalAppend() {}
void FaultInjector::OnBeforePageFlush() {}
void FaultInjector::OnCheckpointStage(int) {}
uint64_t FaultInjector::wal_append_calls() { return 0; }
uint64_t FaultInjector::page_flush_calls() { return 0; }
bool FaultInjector::CorruptPageBytes(const char*, uint32_t, PageId, uint32_t, uint32_t) {
  return false;
}
bool FaultInjector::TearPageWrite(const char*, uint32_t, PageId) { return false; }

#endif

}  // namespace ltm

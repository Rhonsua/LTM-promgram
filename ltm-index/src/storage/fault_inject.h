// src/storage/fault_inject.h
// 故障注入接口 —— 崩溃恢复测试的唯一入口（方案 §4.4.1）。
// 仅在 -DLTM_ENABLE_FAULT_INJECTION=1 时生效；未启用时所有方法为空操作，
// 因此生产代码可以无条件调用而不影响性能（编译期消除）。
#pragma once

#include <cstdint>

#include "util/types.h"

namespace ltm {

class FaultInjector {
 public:
  // ---- 全局开关 ----
  static bool enabled();
  static void Reset();
  // 设置"第几次触发点命中时崩溃"；0 表示不崩溃
  static void SetCrashAfterWalAppend(uint64_t n);
  static void SetCrashBeforePageFlush(uint64_t n);
  static void SetCrashDuringCheckpoint(int stage);

  // ---- 触发点（由 DiskManager / BufferPool / CheckpointManager 调用）----
  // 到达指定次数时调用 _exit(9)，模拟进程被强杀（不跑析构、不刷盘）
  static void OnWalAppend();
  static void OnBeforePageFlush();
  static void OnCheckpointStage(int stage);

  // ---- 主动损坏（供测试构造损坏场景）----
  // 翻转目标页文件中的一段字节；需在页已落盘后调用
  static bool CorruptPageBytes(const char* path, uint32_t page_size, PageId pid, uint32_t off,
                               uint32_t len);
  // 只写页的前半部分（模拟撕裂写）
  static bool TearPageWrite(const char* path, uint32_t page_size, PageId pid);

  // ---- 计数器（测试断言用）----
  static uint64_t wal_append_calls();
  static uint64_t page_flush_calls();
};

}  // namespace ltm

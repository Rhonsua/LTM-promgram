// src/storage/disk_manager.h
// L0 设备层：页读写 / 日志追加 / fsync / 逻辑 I/O 统计（方案 §4.2）。
//
// 硬约束：
//   * 本文件之上（L1..L4）禁止出现任何文件 I/O，一切落盘都经此层。
//   * IoStats 是性能评测中 I/O 数据的唯一来源，禁止用系统工具"估"。
//   * 逻辑容量与物理文件解耦：页号 → 物理偏移的映射只在这里发生（方案 D2）。
//
// 本里程碑（M0）状态：接口冻结，实现为占位；M1-1 完成真实实现。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "util/config.h"
#include "util/status.h"
#include "util/types.h"

namespace ltm {

class DiskManager {
 public:
  struct IoStats {
    uint64_t page_reads = 0;
    uint64_t page_writes = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
    uint64_t wal_appends = 0;
    uint64_t wal_bytes = 0;
    uint64_t fsyncs = 0;
    uint64_t fsync_micros = 0;
  };

  explicit DiskManager(const DiskOptions& options);
  ~DiskManager();
  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // ---- 数据页 ----
  // 读页；越界读零填充并计数（便于发现"读到未分配页"的逻辑错误）
  Status ReadPage(PageId pid, char* out);
  Status WritePage(PageId pid, const char* in);
  Status SyncPageFile();
  // Physical byte length, including pending stream writes; bootstrap must not
  // mistake a truncated metadata page for an unwritten page.
  StatusOr<uint64_t> PageFileSize();
  uint64_t num_pages() const;
  uint32_t page_size() const;

  // ---- 日志 ----
  Status AppendWal(const char* buf, size_t len);
  Status SyncWal();
  Status ReadWal(uint64_t offset, char* out, size_t len);
  // 随机写日志（只用于改写文件头部的 LogFileHeader；记录区永远是追加写）。
  // offset + len 超出当前长度时把 wal_size 推进到那里（中间的空洞由文件系统补零）。
  Status WriteWalAt(uint64_t offset, const char* buf, size_t len);
  uint64_t WalSize() const;
  // 检查点后回收日志空间：保留 [keep_from_offset, end)
  //
  // 【注意】本方法把保留区搬到文件**偏移 0**，因此它会破坏 WAL 的 512 字节头部保留区。
  // 需要保留头部的调用方（CheckpointManager）必须自己按
  // 「TruncateWal(0) -> WriteWalAt(0, header) -> AppendWal(kept)」的顺序重建头部。
  Status TruncateWal(uint64_t keep_from_offset);

  const IoStats& stats() const;
  void ResetStats();
  const DiskOptions& options() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm

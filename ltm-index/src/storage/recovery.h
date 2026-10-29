// src/storage/recovery.h
// L2 持久化层：崩溃恢复（Analysis → Redo → Undo）与检查点（方案 §4.4.1）。
//
// 恢复流程（顺序不可改变）：
//   1. Analysis：扫日志，建立 dirty_page_table{pid -> 最新 redo lsn} 与
//                txn_table{txn -> committed/aborted}，并确定 Redo 起点
//                （最后一条 kCheckpointEnd 的 LSN）
//   2. Redo    ：重放 kPageRedo/kClrRedo；页头 page_lsn >= 记录 lsn 则跳过（幂等）
//   3. Undo    ：对"既没提交也没中止"的事务回滚；每个 (事务,页) 取该事务对该页
//                **最早**的一条 kOpUndo 的 before-image，并写 CLR（kClrRedo）
//   4. 收尾    ：SyncPageFile + 统计
//
// 【调用顺序是契约】Recover() 必须在 BufferPool 构造之后、**任何索引打开之前**调用，
// 且此时缓冲池里不应有已驻留的页：恢复直接读写页文件（不经过缓冲池），
// 若池中已有某页的旧镜像，后续访问会拿到恢复前的版本。
//
// 与方案的差异（逐条记在 docs/architecture.md 与方案附录 C）：
//   * Undo 用"每 (事务,页) 的最早 UNDO 记录"代替"按 LSN 逆序逐条回滚"。
//     对**整页镜像**来说两者等价：同一页的多条 UNDO 记录逆序回滚后，页最终
//     就是该事务开始时的那一版，而那一版正是最早那条 UNDO 的 before-image。
//     事务之间不需要交错回滚 —— 本设计是单写者，事务在日志里天然串行。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/wal.h"
#include "util/config.h"
#include "util/status.h"
#include "util/types.h"

namespace ltm {

class RecoveryManager {
 public:
  struct Report {
    Lsn checkpoint_lsn = kInvalidLsn;
    Lsn redo_start_lsn = kInvalidLsn;
    Lsn end_lsn = kInvalidLsn;
    uint64_t records_scanned = 0;
    uint64_t pages_redone = 0;
    uint64_t records_skipped = 0;
    uint64_t txns_undone = 0;
    uint64_t torn_pages_repaired = 0;
    uint64_t checksum_repairs = 0;
    uint64_t elapsed_micros = 0;
    bool clean_shutdown = true;  // 上次运行是否干净关闭（来自日志头）

    // ---- M4-3 新增：把"恢复到底做了什么"拆到可判读的粒度 ----
    uint64_t records_redone = 0;         // 实际应用的重做记录数
    uint64_t undo_records_applied = 0;   // 实际应用的回滚记录数
    uint64_t clrs_written = 0;           // 写出的补偿记录数
    uint64_t txns_seen = 0;              // 分析阶段见到的事务数
    uint64_t losers = 0;                 // 需要回滚的事务数
    uint64_t dirty_pages = 0;            // 脏页表大小（Redo 的候选集）
    uint64_t wal_bytes = 0;              // 被扫描的日志字节数
    bool recovery_needed = false;        // 是否真的做了 Redo/Undo
  };

  RecoveryManager(const WalOptions& options, DiskManager* disk);
  ~RecoveryManager();
  RecoveryManager(const RecoveryManager&) = delete;
  RecoveryManager& operator=(const RecoveryManager&) = delete;

  // 执行完整恢复；应在 BufferPool 构造之后、任何索引打开之前调用
  Status Recover(BufferPool* pool);

  const Report& report() const;
  Lsn checkpoint_lsn() const;
  void set_checkpoint_lsn(Lsn lsn);

  // 日志头（持久化的 last_checkpoint_lsn / clean_shutdown 标志）。
  // 类型与 storage/wal.h 的 WalLogHeader 是同一个 —— 它是日志文件格式的一部分，
  // 不该在恢复层再定义一份等价结构（两份定义必然漂移）。
  using LogFileHeader = WalLogHeader;
  static constexpr uint64_t kLogMagic = kWalLogMagic;
  static constexpr uint32_t kLogFormatVersion = kWalLogFormatVersion;

  Status WriteLogHeader(const LogFileHeader& header);
  Status ReadLogHeader(LogFileHeader* out);

  // ---- M4-3 新增 ----
  // 本恢复管理器持有的写日志器。BufferPool 借它写 WAL（同一条 LSN 序列），
  // CheckpointManager 借它写检查点记录。所有权在这里，是为了让"恢复期写的 CLR"
  // 与"恢复之后的正常写"落在同一条 LSN 序列上。
  //
  // 注意：这三个访问器**不能**写成内联（Impl 是前向声明的不完整类型），
  // 定义在 recovery.cc 里。
  WalWriter* wal();
  // 把内存里的日志头（含 checkpoint_lsn / clean_shutdown）落盘并 fsync
  Status PersistLogHeader();
  const LogFileHeader& log_header() const;
  bool previous_run_was_clean() const;
  // 干净关闭：置 clean_shutdown=1 并落盘。由 BufferPool 析构（FlushAll 之后）调用。
  Status MarkCleanShutdown();
  // 打开时把 clean_shutdown 清 0（本次运行开始变"脏"）。构造函数里已自动执行，
  // 这里公开只为让测试能显式构造"上次干净 / 上次不干净"两种状态。
  Status MarkUncleanShutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class CheckpointManager {
 public:
  CheckpointManager(const WalOptions& options, DiskManager* disk, RecoveryManager* recovery,
                    BufferPool* pool);
  ~CheckpointManager();
  CheckpointManager(const CheckpointManager&) = delete;
  CheckpointManager& operator=(const CheckpointManager&) = delete;

  // 检查点阶段（供故障注入定位）：
  //   1 = 写 kCheckpointBegin   2 = flush_all   3 = 写 kCheckpointEnd   4 = 截断 WAL
  Status RunCheckpoint();

  uint64_t num_checkpoints() const;
  // 上一次检查点后保留在日志里的字节数（含头部保留区），用于"截断确实生效"的证据
  uint64_t last_kept_bytes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm

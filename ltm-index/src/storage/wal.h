// src/storage/wal.h
// L2 持久化层：单写者页级 REDO + 操作级 UNDO 的预写日志（方案 D4）。
//
// 文件布局（M4-3 起；此前是"记录从偏移 0 开始"的 M4-2 布局）：
//
//   +0                    +512                                  +wal_size
//   |  LogFileHeader      |  record, record, record ...          |
//   |  (128B 结构 + 补零) |  （每条 = 36B 记录头 + data）        |
//
//   512 字节保留区（kWalDataStart）的意义：
//     * 头部可以就地改写（last_checkpoint_lsn / clean_shutdown），不必搬动记录区；
//     * 记录区起点落在扇区边界上，且给未来留扩展位（header_bytes 字段自描述）。
//
// 日志记录布局（小端；写盘用 #pragma pack(1)，因此以下偏移是精确的）：
//   +0  uint64 lsn            单调递增，全局唯一
//   +8  uint64 txn_id         逻辑事务号（一次索引操作）
//   +16 uint32 total_len      本记录总长度（含头与校验和）
//   +20 uint32 checksum       crc32c(本记录除 checksum 字段外的全部字节)
//   +24 uint8  type           WalRecordType
//   +25 uint8  pad            显式填充，保证 page_off/checksum 的自然对齐
//   +26 uint16 page_off       页内偏移（kPageRedo/kOpUndo/kClrRedo 时有效）
//   +28 uint16 data_len       data 区长度
//   +30 uint16 pad2
//   +32 uint32 page_id
//   +36 data[data_len]        after-image（REDO）或 before-image（UNDO）
//
// 写路径（单写者，天然串行）：
//   BeginOp -> AppendPageDelta(每页一条，同时记录 before-image 供 UNDO) -> Commit
//   提交前必须先 fsync 本事务全部记录，然后才允许修改数据页。
//
// M4-3 状态：日志头 + 记录扫描器 + CLR 记录 + 检查点记录已实现；
// Analysis/Redo/Undo 与检查点在 storage/recovery.cc。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "storage/disk_manager.h"
#include "util/config.h"
#include "util/status.h"
#include "util/types.h"

namespace ltm {

enum class WalRecordType : uint8_t {
  kPageRedo = 1,
  kOpUndo = 2,
  kCommit = 3,
  kAbort = 4,
  kCheckpointBegin = 5,
  kCheckpointEnd = 6,
  kAllocPage = 7,
  kFreePage = 8,
  // 补偿日志记录（CLR，M4-3）：内容是"回滚之后的页镜像"，语义等价于 kPageRedo，
  // 但它**没有配对的 kOpUndo**。它存在的唯一理由是让被回滚的页获得一个比原
  // 更新记录更大的 page_lsn，从而让"重放"在再次崩溃后正确地跳过已经回滚掉的那些
  // 更新（否则第二次恢复会把未提交事务的 after-image 重新放回去，而且再也回滚不掉）。
  kClrRedo = 9,
};

const char* WalRecordTypeName(WalRecordType t);
constexpr uint32_t kWalHeaderSize = 36;
constexpr uint64_t kWalInvalidOffset = 0xFFFFFFFFFFFFFFFFull;

// ---------------------------------------------------------------------------
// 日志文件头（M4-3）
//
// 为什么必须有它：没有头部的日志在"重开"时无法回答两个问题 ——
//   1. 上一次运行是干净关闭的吗？（决定要不要做 Redo/Undo）
//   2. 最后一次检查点在哪？（决定 Redo 的起点，也决定恢复时间是否与日志长度无关）
// M4-2 用"扫全文件取最大 LSN"来猜 LSN 起点，那只能保证 LSN 不重号，
// 回答不了上面两条，而且扫全文件本身就是 O(日志长度)。
// ---------------------------------------------------------------------------
constexpr uint64_t kWalLogMagic = 0x4C544D57414C3031ull;  // "LTMWAL01"
constexpr uint32_t kWalLogFormatVersion = 2;             // 1 = M4-2（无头部）布局
constexpr uint32_t kWalLogHeaderSize = 128;              // 头部结构体本身
constexpr uint32_t kWalDataStart = 512;                  // 记录区起点（= 保留区长度）

#pragma pack(push, 1)
struct WalLogHeader {
  uint64_t magic = kWalLogMagic;
  uint32_t format_version = kWalLogFormatVersion;
  uint32_t header_bytes = kWalDataStart;  // 保留区长度，自描述以便未来扩展
  Lsn last_checkpoint_lsn = kInvalidLsn;  // kInvalidLsn = 从未做过检查点
  uint8_t clean_shutdown = 0;             // 1 = 上次是干净关闭（无需恢复）
  uint8_t reserved[3] = {};
  uint32_t checksum = 0;                  // crc32c(本结构前 28 字节)
};
#pragma pack(pop)
static_assert(sizeof(WalLogHeader) == 32, "WalLogHeader layout changed; update the docs");

// 读头部。文件为空（wal_size == 0）或头部未写入时返回 kNotFound。
// 头部存在但 magic/版本/校验和不符时返回 kCorruption（不是 kNotFound —— 那是损坏，不是"没有"）。
Status ReadWalLogHeader(DiskManager* disk, WalLogHeader* out);
// 写头部并 fsync（头部的两个字段都是"崩溃语义"的判据，不能停在缓冲里）。
Status WriteWalLogHeader(DiskManager* disk, const WalLogHeader& header);
// 保证日志文件有一个可用的头部：空文件则新建（clean_shutdown=0，未做检查点）。
// created 输出"本次是否新建"。非空但没有头部（M4-2 遗留日志）→ kCorruption。
Status EnsureWalLogHeader(DiskManager* disk, WalLogHeader* out, bool* created);

#pragma pack(push, 1)
struct WalRecordHeader {
  Lsn lsn = kInvalidLsn;
  TxnId txn_id = kInvalidTxnId;
  uint32_t total_len = 0;
  uint32_t checksum = 0;
  WalRecordType type = WalRecordType::kPageRedo;
  uint8_t pad = 0;
  uint16_t page_off = 0;
  uint16_t data_len = 0;
  uint16_t pad2 = 0;
  PageId page_id = kInvalidPageId;
};
#pragma pack(pop)
static_assert(sizeof(WalRecordHeader) == kWalHeaderSize, "WalRecordHeader layout changed; update the docs");

// 顺序扫描记录区（自动跳过 512 字节保留区）。
//
// 停止条件（三条，都是"正常停止"而不是错误）：
//   * 走到文件末尾；
//   * 记录头不合法（total_len 越界）；
//   * **记录校验和不符** —— 这是"追加写到一半就被强杀"的形态，也就是崩溃日志的尾部。
// 因此扫描结果是"可读前缀"，调用方不能假设它覆盖整个文件。
// 访问器返回 false 表示提前结束扫描。
using WalRecordVisitor = std::function<bool(const WalRecordHeader&, const char* data)>;
Status ForEachWalRecord(DiskManager* disk, const WalRecordVisitor& visit);

// 一页的一次修改：before 用于 UNDO，after 用于 REDO
struct PageImageDelta {
  PageId page_id = kInvalidPageId;
  uint16_t offset = 0;
  std::vector<char> before;
  std::vector<char> after;
};

class WalWriter {
 public:
  WalWriter(const WalOptions& options, DiskManager* disk);
  ~WalWriter();
  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  // 开启一个逻辑事务
  StatusOr<TxnId> BeginOp();
  // 为一个事务追加页级修改（内部生成 kPageRedo + kOpUndo 记录）。
  //
  // **返回 REDO 记录的 LSN**，调用方必须把它写进页头的 page_lsn（M4-2 的 D6/D2），
  // 否则写屏障无从判断"这一页的 REDO 到底落盘了没有"。
  // 之所以是 REDO 而不是 UNDO 的 LSN：REDO（after-image）才是让本页内容可被重放的
  // 那条记录；UNDO 排在它后面，只用于回滚。页头存一个偏大的 LSN（UNDO 的）只会让
  // 屏障更严，但会让"这一页由哪条记录负责"变得含混。
  StatusOr<Lsn> AppendPageDelta(TxnId txn, const PageImageDelta& delta);
  // 追加一条补偿记录（CLR）：只有 REDO 半边，用于恢复期回滚。返回它的 LSN。
  StatusOr<Lsn> AppendClr(TxnId txn, const PageImageDelta& delta);
  // 页分配/回收也必须进日志，否则恢复后页泄漏或双分配（不变量 I7）
  Status AppendAllocPage(TxnId txn, PageId pid);
  Status AppendFreePage(TxnId txn, PageId pid);
  // 提交：fsync 本事务全部记录后才返回成功
  Status Commit(TxnId txn);
  Status Abort(TxnId txn);

  // ---- 检查点记录（M4-4）----
  struct CheckpointMark {
    Lsn lsn = kInvalidLsn;
    uint64_t offset = 0;  // 该记录在日志文件里的起始偏移（截断时要保留它）
  };
  StatusOr<CheckpointMark> AppendCheckpointBegin();
  // 写 kCheckpointEnd 并返回它的 LSN。end 记录的 LSN 就是新的 last_checkpoint_lsn：
  // 恢复时从它开始重放，而它之前的记录已经全部落到数据文件里了。
  StatusOr<Lsn> AppendCheckpointEnd(Lsn begin_lsn);

  // 显式同步（group commit 的批量点）
  Status Sync();

  Lsn last_lsn() const;
  Lsn flushed_lsn() const;
  uint64_t bytes_written() const;

  // 测试/调试：把 WAL 导出为可读文本，用于人工核对日志内容
  Status DumpToText(const std::string& out_path) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm


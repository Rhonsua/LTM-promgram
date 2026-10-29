#include "storage/wal.h"

#include <cstddef>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "storage/fault_inject.h"
#include "util/crc32c.h"
#include "util/log.h"

namespace ltm {

const char* WalRecordTypeName(WalRecordType t) {
  switch (t) {
    case WalRecordType::kPageRedo: return "kPageRedo";
    case WalRecordType::kOpUndo: return "kOpUndo";
    case WalRecordType::kCommit: return "kCommit";
    case WalRecordType::kAbort: return "kAbort";
    case WalRecordType::kCheckpointBegin: return "kCheckpointBegin";
    case WalRecordType::kCheckpointEnd: return "kCheckpointEnd";
    case WalRecordType::kAllocPage: return "kAllocPage";
    case WalRecordType::kFreePage: return "kFreePage";
    case WalRecordType::kClrRedo: return "kClrRedo";
  }
  return "kUnknown";
}

namespace {

// 记录校验和：覆盖整条记录除 checksum 字段（偏移 20）之外的字节。
uint32_t RecordChecksum(const char* buf, size_t len) {
  Crc32cHasher hasher;
  hasher.Update(buf, offsetof(WalRecordHeader, checksum));
  const size_t after = offsetof(WalRecordHeader, checksum) + sizeof(uint32_t);
  if (len > after) {
    hasher.Update(buf + after, len - after);
  }
  return hasher.value();
}

}  // namespace

// ===========================================================================
// 日志文件头（M4-3）
//
// 【头部自己也要校验和】头部是"要不要做恢复"的唯一判据，读到一个半写坏的头部
// （比如 last_checkpoint_lsn 被截断）会让恢复从一个错误的位置开始 —— 那比"没有头部"
// 危险得多，因为它会静默地少重放一段日志。所以头部带 crc32c，不符即 kCorruption。
// ===========================================================================
Status ReadWalLogHeader(DiskManager* disk, WalLogHeader* out) {
  if (disk == nullptr || out == nullptr) {
    return InvalidArgument("ReadWalLogHeader needs a DiskManager and an output");
  }
  if (disk->WalSize() < kWalLogHeaderSize) {
    return Status::Error(StatusCode::kNotFound, "the log file is empty (no header yet)");
  }
  std::vector<char> buf(kWalLogHeaderSize, 0);
  LTM_RETURN_IF_ERROR(disk->ReadWal(0, buf.data(), buf.size()));
  WalLogHeader h{};
  std::memcpy(&h, buf.data(), sizeof(WalLogHeader));
  if (h.magic != kWalLogMagic) {
    return Status::Error(StatusCode::kNotFound, "the log file has no LTMWAL header");
  }
  if (h.format_version != kWalLogFormatVersion) {
    return Status::Error(StatusCode::kCorruption,
                         "log format version mismatch: " + std::to_string(h.format_version) + " (expected " +
                             std::to_string(kWalLogFormatVersion) + ")");
  }
  if (h.header_bytes != kWalDataStart) {
    return Status::Error(StatusCode::kCorruption,
                         "log header says the reserved area is " + std::to_string(h.header_bytes) +
                             " bytes, but this build writes records at " + std::to_string(kWalDataStart));
  }
  const uint32_t want = Crc32c(buf.data(), sizeof(WalLogHeader) - sizeof(uint32_t));
  if (want != h.checksum) {
    return Status::Error(StatusCode::kCorruption,
                         "log header checksum mismatch: stored " + std::to_string(h.checksum) + ", computed " +
                             std::to_string(want));
  }
  *out = h;
  return Status::Ok();
}

Status WriteWalLogHeader(DiskManager* disk, const WalLogHeader& header) {
  if (disk == nullptr) {
    return InvalidArgument("WriteWalLogHeader needs a DiskManager");
  }
  std::vector<char> buf(kWalDataStart, 0);  // 保留区整体重写，尾部补零
  WalLogHeader h = header;
  h.magic = kWalLogMagic;
  h.format_version = kWalLogFormatVersion;
  h.header_bytes = kWalDataStart;
  h.checksum = 0;
  h.checksum = Crc32c(reinterpret_cast<const char*>(&h), sizeof(WalLogHeader) - sizeof(uint32_t));
  std::memcpy(buf.data(), &h, sizeof(WalLogHeader));
  LTM_RETURN_IF_ERROR(disk->WriteWalAt(0, buf.data(), buf.size()));
  return disk->SyncWal();
}

Status EnsureWalLogHeader(DiskManager* disk, WalLogHeader* out, bool* created) {
  if (created != nullptr) {
    *created = false;
  }
  if (disk == nullptr) {
    return InvalidArgument("EnsureWalLogHeader needs a DiskManager");
  }
  const uint64_t size = disk->WalSize();
  if (size == 0) {
    WalLogHeader h{};  // 默认即"未做检查点 + 上次不是干净关闭"
    LTM_RETURN_IF_ERROR(WriteWalLogHeader(disk, h));
    if (out != nullptr) {
      *out = h;
    }
    if (created != nullptr) {
      *created = true;
    }
    return Status::Ok();
  }
  if (size < kWalDataStart) {
    return Status::Error(StatusCode::kCorruption,
                         "the log file is " + std::to_string(size) +
                             " bytes long, i.e. truncated inside the " + std::to_string(kWalDataStart) +
                             "-byte header area");
  }
  WalLogHeader h{};
  const Status s = ReadWalLogHeader(disk, &h);
  if (s.code() == StatusCode::kNotFound) {
    // 非空、但前 8 字节不是 magic：这是 M4-2 布局的日志（记录从偏移 0 开始）。
    // 本构建无法安全解释它（把记录当头部读会把 LSN 读成随机值），因此明确拒绝，
    // 而不是"猜一个起点"继续跑 —— 猜错的后果是静默少重放一段日志。
    return Status::Error(StatusCode::kCorruption,
                         "the log file has no LTMWAL header, so it predates the header format (M4-2). "
                         "This build cannot interpret it safely; delete the log file (and the data file, "
                         "since a log without a header cannot be replayed) and start over.");
  }
  LTM_RETURN_IF_ERROR(s);
  if (out != nullptr) {
    *out = h;
  }
  return Status::Ok();
}

Status ForEachWalRecord(DiskManager* disk, const WalRecordVisitor& visit) {
  if (disk == nullptr) {
    return InvalidArgument("ForEachWalRecord needs a DiskManager");
  }
  const uint64_t total = disk->WalSize();
  uint64_t off = kWalDataStart;
  char head[kWalHeaderSize];
  std::vector<char> rec;
  while (off + kWalHeaderSize <= total) {
    LTM_RETURN_IF_ERROR(disk->ReadWal(off, head, kWalHeaderSize));
    WalRecordHeader h{};
    std::memcpy(&h, head, kWalHeaderSize);
    if (h.total_len < kWalHeaderSize || h.total_len > total - off) {
      break;  // 头部不合法：日志尾部的正常形态
    }
    rec.assign(h.total_len, 0);
    LTM_RETURN_IF_ERROR(disk->ReadWal(off, rec.data(), rec.size()));
    const uint32_t want = RecordChecksum(rec.data(), h.total_len);
    if (want != h.checksum) {
      // 记录被写坏（追加到一半就被强杀）。停在它之前 —— 之后的字节不可信。
      LOG_WARN("WAL scan: record at offset ", off, " has checksum ", h.checksum, " but computes to ", want,
               "; treating it as a torn tail and stopping the scan");
      break;
    }
    if (!visit(h, rec.data() + kWalHeaderSize)) {
      return Status::Ok();
    }
    off += h.total_len;
  }
  return Status::Ok();
}

// ===========================================================================
// M4-3 现状说明
//
// 本文件现在是完整的"日志半边"：文件头、记录编码、顺序追加、CLR、检查点记录、
// 组提交与 fsync 屏障、记录扫描器。恢复（Analysis/Redo/Undo）在 storage/recovery.cc。
//
// 【崩溃注入的计数口径，M4-3 统一】OnWalAppend 只在 DiskManager::AppendWal 里调用，
// 即**每条物理记录**一次。M4-2 时 WalWriter 的每个 Append* 也各调一次，于是一条
// delta 会命中 3 次（WalWriter 一次 + REDO/UNDO 两条记录各经 DiskManager 一次），
// 计数器混了"delta"与"record"两种粒度 —— 而崩溃矩阵要判的正是"第 n 条记录落了盘没"。
// 现在只有一处调用点，n 的含义是明确的物理记录序号。
// ===========================================================================

struct WalWriter::Impl {
  Impl(const WalOptions& o, DiskManager* d) : opt(o), disk(d) {}
  WalOptions opt;
  DiskManager* disk;
  Lsn next_lsn = 1;
  Lsn flushed_lsn = 0;
  TxnId next_txn = 1;
  uint64_t bytes = 0;
  uint64_t pending_records = 0;

  // -------------------------------------------------------------------------
  // C1 · 内部互斥
  //
  // 【为什么 WAL 也需要锁】WAL 的写入方是单写者（D4），但 **Sync()/flushed_lsn()/
  // last_lsn() 会被读者线程调到**：C1 撤掉 C0 之后，读者的缺页/淘汰路径会经
  // `PrepareVictim -> FlushFrame` 把脏页刷盘，而 FlushFrame 要读 `flushed_lsn()`、
  // 必要时还要 `Sync()`。这些与写者的 Append/Commit 并发时就是数据竞争
  // （`next_lsn`/`flushed_lsn`/`pending_records` 与底层缓冲区）。
  //
  // 【为什么是 recursive】`Commit()` 内部会调 `Sync()`（第 422 行），两层都要锁。
  // 用 recursive_mutex 最简单，代价可忽略（WAL 操作本来就是 I/O 主导）。
  //
  // 【它检测不到什么】ASAN 看不见数据竞争，本机 MSVC 又没有 TSAN —— 所以这把锁
  // 是"按分析加上去"的，不是"被工具证明必需"的。结题报告的局限一节要如实写。
  mutable std::recursive_mutex mu;

  // 每个活跃事务已写入的字节区间，Commit 时用于统计与（M4 的）批量 fsync
  std::unordered_map<TxnId, uint64_t> txn_records;

  // 从已有日志里扫出最大 LSN（M4-2 的 D8）。
  //
  // 【为什么必须有】WAL 文件是**追加**的：DiskManager 打开时把 wal_size 取成文件长度
  // （disk_manager.cc），AppendWal 从 wal_size 处续写。而 next_lsn 每次构造都
  // 从 1 重新开始 —— 于是重开数据库后，新记录会与旧记录**重号**。
  //
  // 后果不是"日志看着乱"这么抽象：页头里存着上一轮写入的 page_lsn（例如 480），
  // 而新一轮的 flushed_lsn 从 0 起步，M4-2 的写屏障会把每一个旧页都判成
  // "日志尚未落盘"。更坏的是这个断言**只会在误报方向响**，真实的违反反而会被淹掉。
  // 触发路径就在现有测试套件里：B+ 与 R 树都有"关池 -> 重开 -> 校验"的用例。
  //
  // M4-3 起改用 ForEachWalRecord：它跳过 512 字节头部、并做记录级 crc 校验，
  // 于是"崩溃后尾部被撕裂"的日志只会扫到**可读前缀** —— 任何引用了撕裂记录
  // 之后 LSN 的页仍然会被屏障抓住，不会因为这一步被放过。
  Lsn ScanMaxLsn() const {
    if (disk == nullptr) {
      return kInvalidLsn;
    }
    Lsn max_lsn = kInvalidLsn;
    LTM_UNUSED(ForEachWalRecord(disk, [&max_lsn](const WalRecordHeader& h, const char*) {
      if (h.lsn > max_lsn) {
        max_lsn = h.lsn;
      }
      return true;
    }));
    return max_lsn;
  }

  Status WriteRecord(const WalRecordHeader& header, const std::vector<char>& data) {
    if (disk == nullptr) {
      return Status::Error(StatusCode::kIoError, "WalWriter has no DiskManager bound");
    }
    std::vector<char> buf(kWalHeaderSize + data.size());
    WalRecordHeader h = header;
    h.data_len = static_cast<uint16_t>(data.size());
    h.total_len = static_cast<uint32_t>(buf.size());
    h.checksum = 0;
    std::memcpy(buf.data(), &h, kWalHeaderSize);
    if (!data.empty()) {
      std::memcpy(buf.data() + kWalHeaderSize, data.data(), data.size());
    }
    // 校验和覆盖除 checksum 字段外的全部字节：先算前半段，再算后半段
    Crc32cHasher hasher;
    hasher.Update(buf.data(), offsetof(WalRecordHeader, checksum));
    const size_t after = offsetof(WalRecordHeader, checksum) + sizeof(uint32_t);
    hasher.Update(buf.data() + after, buf.size() - after);
    const uint32_t checksum = hasher.value();
    std::memcpy(buf.data() + offsetof(WalRecordHeader, checksum), &checksum, sizeof(checksum));

    LTM_RETURN_IF_ERROR(disk->AppendWal(buf.data(), buf.size()));
    bytes += buf.size();
    ++pending_records;
    return Status::Ok();
  }
};

WalWriter::WalWriter(const WalOptions& options, DiskManager* disk) : impl_(new Impl(options, disk)) {
  // 【顺序很重要】必须先保证日志有头部，再扫最大 LSN：扫描器从 kWalDataStart 起跳，
  // 而空文件的 wal_size 是 0，此时任何记录都不会被扫到（这正是我们要的语义）。
  bool created = false;
  WalLogHeader hdr{};
  const Status hs = EnsureWalLogHeader(disk, &hdr, &created);
  if (!hs.ok()) {
    LOG_ERROR("WalWriter: the log file has no usable header: ", hs.ToString());
  } else if (created) {
    LOG_INFO("WalWriter: created a fresh log header (last_checkpoint_lsn=", hdr.last_checkpoint_lsn,
             ", clean_shutdown=", static_cast<int>(hdr.clean_shutdown), ")");
  }
  // D8：让 LSN 在**跨运行**意义上也单调（理由见 Impl::ScanMaxLsn 的注释）。
  const Lsn recovered = impl_->ScanMaxLsn();
  if (recovered != kInvalidLsn) {
    impl_->next_lsn = recovered + 1;
    // 已经躺在日志文件里的记录，对本次运行而言就是"稳定存储上的前缀"，于是把它当作
    // 已落盘 —— 重开后旧页携带的 page_lsn 才不会被写屏障误判成"未落盘"。
    impl_->flushed_lsn = recovered;
    LOG_INFO("WalWriter: the log file already holds ", impl_->disk->WalSize(),
             " bytes; continuing the LSN sequence at ", impl_->next_lsn);
  }
  LOG_INFO("WalWriter ready: records start at offset ", kWalDataStart,
           " (the first ", kWalDataStart, " bytes hold the log header); group_commit_ms=",
           options.group_commit_ms, " group_commit_records=", options.group_commit_records);
}

WalWriter::~WalWriter() = default;

StatusOr<TxnId> WalWriter::BeginOp() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  const TxnId txn = impl_->next_txn++;
  impl_->txn_records[txn] = 0;
  return txn;
}

// 【记录顺序：UNDO 必须先于 REDO —— 这条是崩溃矩阵找出来的，不是设计出来的】
//
// 最初写的是 REDO 在前、UNDO 在后。崩溃矩阵立刻抓到一个反例（wal_append n=40 /
// random workload）：恢复后**页里的条目数比元数据计数多 1**，而键集合完全正确。
//
// 根因就在这两条记录的**中间**：崩溃可以发生在"REDO 已落盘、UNDO 还没写"的那一刻。
// 此时这一页在日志里只有 after-image，没有 before-image ——
//   * Redo 阶段：把它重放上去（页里多出那条未提交的插入）；
//   * Undo 阶段：这一页**没有 UNDO 记录**，于是"按 UNDO 记录定位待回滚页"的做法
//     根本不知道要回滚它；
//   * 而元数据页的 delta 排在它后面，压根没写进日志，所以计数没被加过。
// 结果就是"页里 6 条、元数据说 5 条"——数据看起来是对的，不变量却已经破了。
//
// 把 UNDO 写在前面的效果：崩溃点落在两条记录之间时，留在日志里的是 **UNDO 而没有
// REDO**。这恰好是安全的方向 ——
//   * 这一页不会进脏页表（没有 REDO），Redo 不碰它；
//   * Undo 找得到它，把 before-image 写回去，页回到事务开始态；
//   * 而"事务开始态"正是它此刻在盘上应当有的样子：这一页在崩溃前一直被钉住，
//     不可能被刷盘，所以盘上是事务开始之前的状态，before-image 只会把它推进到
//     一个与已提交历史一致的状态，不会覆盖任何已提交的改动（单写者，loser 必是最后一个事务）。
// 换句话说：UNDO 在前时，"两条记录只落了一条"的所有情形都是可恢复的；REDO 在前时
// 有一种不可恢复。这个方向性不是风格问题。
StatusOr<Lsn> WalWriter::AppendPageDelta(TxnId txn, const PageImageDelta& delta) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  // 一条操作级 UNDO（before-image）+ 一条页级 REDO（after-image），顺序见上
  WalRecordHeader undo{};
  undo.lsn = impl_->next_lsn++;
  undo.txn_id = txn;
  undo.type = WalRecordType::kOpUndo;
  undo.page_off = delta.offset;
  undo.page_id = delta.page_id;
  LTM_RETURN_IF_ERROR(impl_->WriteRecord(undo, delta.before));

  WalRecordHeader redo{};
  redo.lsn = impl_->next_lsn++;
  redo.txn_id = txn;
  redo.type = WalRecordType::kPageRedo;
  redo.page_off = delta.offset;
  redo.page_id = delta.page_id;
  LTM_RETURN_IF_ERROR(impl_->WriteRecord(redo, delta.after));
  const Lsn redo_lsn = redo.lsn;

  auto it = impl_->txn_records.find(txn);
  if (it != impl_->txn_records.end()) {
    it->second += 2;
  }
  return redo_lsn;
}

// CLR（补偿记录，M4-3）：只有 REDO 半边。
//
// 【为什么不能复用 AppendPageDelta 来写 CLR】那会同时写出一条 kOpUndo，内容是
// "回滚之前的那一版"。下次恢复若再次回滚这个事务，就会把这条 UNDO 应用上去 ——
// 等于把已经回滚掉的 after-image 又放回页里，而且这次连回滚记录都指向错误的方向。
// CLR 的语义是"这件事已经回滚了，别再回滚一次"，所以它必须没有 UNDO 配对。
StatusOr<Lsn> WalWriter::AppendClr(TxnId txn, const PageImageDelta& delta) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  WalRecordHeader h{};
  h.lsn = impl_->next_lsn++;
  h.txn_id = txn;
  h.type = WalRecordType::kClrRedo;
  h.page_off = delta.offset;
  h.page_id = delta.page_id;
  LTM_RETURN_IF_ERROR(impl_->WriteRecord(h, delta.after));
  auto it = impl_->txn_records.find(txn);
  if (it != impl_->txn_records.end()) {
    it->second += 1;
  }
  return h.lsn;
}

Status WalWriter::AppendAllocPage(TxnId txn, PageId pid) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  WalRecordHeader h{};
  h.lsn = impl_->next_lsn++;
  h.txn_id = txn;
  h.type = WalRecordType::kAllocPage;
  h.page_id = pid;
  return impl_->WriteRecord(h, {});
}

Status WalWriter::AppendFreePage(TxnId txn, PageId pid) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  WalRecordHeader h{};
  h.lsn = impl_->next_lsn++;
  h.txn_id = txn;
  h.type = WalRecordType::kFreePage;
  h.page_id = pid;
  return impl_->WriteRecord(h, {});
}

// 检查点记录：begin 记录里带上"上一次检查点的 LSN"，这样即使 begin/end 之间崩溃，
  // 分析阶段也能从 begin 里回退到一个已知的检查点位置（否则只能从日志头开始重放）。
StatusOr<WalWriter::CheckpointMark> WalWriter::AppendCheckpointBegin() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  CheckpointMark mark;
  mark.offset = (impl_->disk != nullptr) ? impl_->disk->WalSize() : 0;
  WalRecordHeader h{};
  h.lsn = impl_->next_lsn++;
  h.txn_id = kInvalidTxnId;
  h.type = WalRecordType::kCheckpointBegin;
  h.page_id = kInvalidPageId;
  LTM_RETURN_IF_ERROR(impl_->WriteRecord(h, {}));
  mark.lsn = h.lsn;
  return mark;
}

StatusOr<Lsn> WalWriter::AppendCheckpointEnd(Lsn begin_lsn) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  WalRecordHeader h{};
  h.lsn = impl_->next_lsn++;
  h.txn_id = kInvalidTxnId;
  h.type = WalRecordType::kCheckpointEnd;
  h.page_id = kInvalidPageId;
  // data 区放 8 字节的 begin_lsn：分析阶段用它验证 begin/end 配对，
  // 也让人工 dump 日志时能一眼看出这是哪一次检查点的收尾。
  std::vector<char> data(sizeof(Lsn), 0);
  std::memcpy(data.data(), &begin_lsn, sizeof(Lsn));
  LTM_RETURN_IF_ERROR(impl_->WriteRecord(h, data));
  return h.lsn;
}

Status WalWriter::Commit(TxnId txn) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  WalRecordHeader h{};
  h.lsn = impl_->next_lsn++;
  h.txn_id = txn;
  h.type = WalRecordType::kCommit;
  LTM_RETURN_IF_ERROR(impl_->WriteRecord(h, {}));
  impl_->txn_records.erase(txn);
  // 不变量 I6：提交记录必须先落到稳定存储，然后才允许调用方去改数据页。
  // 这里**无条件** Sync，而不是等 group_commit_records 凑够：组提交只能推迟
  // "批量刷盘"这件事，不能让"已提交"退化成"还没落盘"。
  return Sync();
}

Status WalWriter::Abort(TxnId txn) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  WalRecordHeader h{};
  h.lsn = impl_->next_lsn++;
  h.txn_id = txn;
  h.type = WalRecordType::kAbort;
  LTM_RETURN_IF_ERROR(impl_->WriteRecord(h, {}));
  impl_->txn_records.erase(txn);
  return Status::Ok();
}

Status WalWriter::Sync() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  if (impl_->disk == nullptr) {
    return Status::Error(StatusCode::kIoError, "WalWriter has no DiskManager bound");
  }
  LTM_RETURN_IF_ERROR(impl_->disk->SyncWal());
  impl_->flushed_lsn = impl_->next_lsn - 1;
  impl_->pending_records = 0;
  return Status::Ok();
}

Lsn WalWriter::last_lsn() const {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->next_lsn - 1;
}

Lsn WalWriter::flushed_lsn() const {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->flushed_lsn;
}

uint64_t WalWriter::bytes_written() const {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->bytes;
}

Status WalWriter::DumpToText(const std::string& out_path) const {
  if (impl_->disk == nullptr) {
    return Status::Error(StatusCode::kIoError, "WalWriter has no DiskManager bound");
  }
  std::ofstream ofs(out_path);
  if (!ofs) {
    return Status::Error(StatusCode::kIoError, "cannot write: " + out_path);
  }
  ofs << "# lsn txn_id type page_id page_off data_len total_len checksum\n";
  uint64_t count = 0;
  Lsn max_lsn = kInvalidLsn;
  const Status s = ForEachWalRecord(impl_->disk, [&](const WalRecordHeader& h, const char*) {
    ofs << h.lsn << " " << h.txn_id << " " << WalRecordTypeName(h.type) << " " << h.page_id << " "
        << h.page_off << " " << h.data_len << " " << h.total_len << " " << h.checksum << "\n";
    if (h.lsn > max_lsn) {
      max_lsn = h.lsn;
    }
    ++count;
    return true;
  });
  if (!s.ok()) {
    ofs << "# scan aborted: " << s.ToString() << "\n";
  }
  ofs << "# total " << count << " records, " << impl_->disk->WalSize() << " bytes, max_lsn " << max_lsn << "\n";
  LOG_INFO("WAL exported as text: ", out_path, "(", count, " records)");
  return Status::Ok();
}

}  // namespace ltm

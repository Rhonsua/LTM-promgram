#include "storage/recovery.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "storage/fault_inject.h"
#include "storage/page.h"
#include "util/crc32c.h"
#include "util/log.h"

namespace ltm {

namespace {

// 一页在盘上是否"被写过"。用于把"撕裂页"与"从未写过的页"分开：
// 两者都会让 page_id_copy != pid，但前者要计入修复统计，后者只是正常现象
// （刚分配、还没来得及落盘的页在文件里就是全零）。
bool PageNeverWritten(const char* img) {
  const Page p(const_cast<char*>(img));
  return p.header().page_id == 0 && p.trailer().page_id_copy == 0 && p.trailer().checksum == 0;
}

// 页是否损坏：自描述不一致（撕裂写）或校验和不符（字节被改坏）。
// 【为什么必须连校验和一起看】只比对 page_id_copy 的话，一段"内容被改坏但头尾完好"
// 的页会被判成健康页，随后被 `page_lsn >= rec.lsn` 的幂等规则跳过 —— 损坏就永久留下了。
bool PageDamaged(const char* img, PageId pid) {
  const Page p(const_cast<char*>(img));
  if (p.header().page_id != pid || p.trailer().page_id_copy != pid) {
    return true;
  }
  if (p.header().page_lsn != p.trailer().page_lsn_copy) {
    return true;
  }
  const uint32_t want = Crc32c(img, kPageSize - kPageTrailerSize);
  return want != p.trailer().checksum;
}

}  // namespace

struct RecoveryManager::Impl {
  Impl(const WalOptions& o, DiskManager* d) : opt(o), disk(d) {}
  WalOptions opt;
  DiskManager* disk;
  std::unique_ptr<WalWriter> wal;
  LogFileHeader header{};
  bool header_created = false;
  bool prev_clean = false;
  Report report;
};

RecoveryManager::RecoveryManager(const WalOptions& options, DiskManager* disk)
    : impl_(new Impl(options, disk)) {
  if (disk == nullptr) {
    LOG_ERROR("RecoveryManager: no DiskManager bound; recovery is disabled");
    return;
  }
  // 1) 日志头必须先就绪，WalWriter 才能正确地跳过保留区并续上 LSN 序列
  const Status hs = EnsureWalLogHeader(disk, &impl_->header, &impl_->header_created);
  if (!hs.ok()) {
    LOG_ERROR("RecoveryManager: the log has no usable header: ", hs.ToString());
  }
  impl_->prev_clean = (impl_->header.clean_shutdown != 0);
  impl_->report.clean_shutdown = impl_->prev_clean;
  impl_->report.checkpoint_lsn = impl_->header.last_checkpoint_lsn;

  impl_->wal.reset(new WalWriter(options, disk));

  // 2) 本次运行从这一刻起就是"不干净"的：把 clean_shutdown 清 0 并落盘。
  //    这一步必须在任何写操作之前 —— 否则"崩溃前恰好把 1 留在盘上"就会让下次
  //    打开跳过恢复，而这次运行里写的东西还没落盘。
  if (impl_->prev_clean) {
    impl_->header.clean_shutdown = 0;
    const Status s = WriteWalLogHeader(disk, impl_->header);
    if (!s.ok()) {
      LOG_ERROR("RecoveryManager: cannot clear the clean-shutdown flag: ", s.ToString());
    }
  }
  LOG_INFO("RecoveryManager ready: log header ", (impl_->header_created ? "created" : "loaded"),
           ", previous run ", (impl_->prev_clean ? "shut down cleanly" : "did NOT shut down cleanly"),
           ", last checkpoint lsn ", impl_->header.last_checkpoint_lsn);
}

RecoveryManager::~RecoveryManager() = default;

Status RecoveryManager::Recover(BufferPool* pool) {
  Impl& im = *impl_;
  if (im.disk == nullptr) {
    return InvalidArgument("Recover() needs a DiskManager");
  }
  const auto t0 = std::chrono::steady_clock::now();
  // 幂等：允许重复调用（测试里会故意连跑两次验证"恢复本身可重入"）
  const bool prev_clean = im.report.clean_shutdown;
  const Lsn prev_checkpoint = im.report.checkpoint_lsn;
  im.report = Report{};
  im.report.clean_shutdown = prev_clean;
  im.report.checkpoint_lsn = prev_checkpoint;

  // 恢复直接读写页文件，不经过缓冲池。若池里已经有页驻留，那些帧是恢复前的版本，
  // 之后的访问会拿到陈旧内容 —— 这是调用顺序错误，必须显式报出来而不是静默。
  if (pool != nullptr && pool->resident_pages() != 0) {
    LOG_WARN("Recover() was called with ", pool->resident_pages(),
             " page(s) already resident in the buffer pool; those frames hold pre-recovery "
             "content. Call Recover() before opening any index.");
  }

  // ---- 0. 读日志头 ----
  LogFileHeader hdr{};
  const Status hs = ReadWalLogHeader(im.disk, &hdr);
  if (!hs.ok()) {
    if (hs.code() == StatusCode::kNotFound) {
      LOG_INFO("Recover: the log file is empty; nothing to replay");
      im.report.recovery_needed = false;
      im.report.end_lsn = im.wal->last_lsn();
      return Status::Ok();
    }
    return hs;
  }
  im.header = hdr;
  im.report.checkpoint_lsn = hdr.last_checkpoint_lsn;
  // 【必须用打开时捕获的 prev_clean，而不是现在盘上的这个字节】构造函数在打开时
  // 就把 clean_shutdown 清成了 0（本次运行从这一刻起是"脏"的）。在这里重新读它，
  // 读到的永远是 0，于是"上次干净关闭 ⇒ 不需要恢复"这条短路永远不会生效。
  im.report.clean_shutdown = im.prev_clean;
  im.report.redo_start_lsn = hdr.last_checkpoint_lsn;
  im.report.wal_bytes = im.disk->WalSize();

  // ========================================================================
  // 1. Analysis
  //
  // 三个表：
  //   dirty_page_table : pid -> 该页在检查点之后**最新**的一条重做记录的 LSN
  //                      （Redo 的候选集；不在表里的页一律不碰）
  //   txn_table        : txn -> committed / aborted
  //   undo_first       : (txn, pid) -> 该事务对该页**最早**的一条 UNDO 记录 LSN
  //                      （Undo 阶段的定位依据，理由见 recovery.h 顶部）
  // ========================================================================
  struct TxnInfo {
    bool committed = false;
    bool aborted = false;
  };
  std::unordered_map<TxnId, TxnInfo> txn_table;
  std::unordered_map<PageId, Lsn> dirty;
  std::unordered_map<TxnId, std::unordered_map<PageId, Lsn>> undo_first;
  Lsn redo_start = hdr.last_checkpoint_lsn;
  uint64_t scanned = 0;
  Lsn max_lsn = kInvalidLsn;

  Status scan = ForEachWalRecord(im.disk, [&](const WalRecordHeader& h, const char*) {
    ++scanned;
    if (h.lsn > max_lsn) {
      max_lsn = h.lsn;
    }
    switch (h.type) {
      case WalRecordType::kCheckpointEnd:
        // 检查点结束意味着"此刻之前的所有脏页都已落盘" → 脏页表清空、Redo 起点前移。
        // 这正是"恢复时间不随日志长度线性增长"的来源。
        dirty.clear();
        redo_start = h.lsn;
        break;
      case WalRecordType::kPageRedo:
      case WalRecordType::kClrRedo: {
        if (h.lsn <= redo_start) {
          break;
        }
        auto it = dirty.find(h.page_id);
        if (it == dirty.end() || h.lsn > it->second) {
          dirty[h.page_id] = h.lsn;
        }
        break;
      }
      case WalRecordType::kOpUndo: {
        auto& per_page = undo_first[h.txn_id];
        auto it = per_page.find(h.page_id);
        if (it == per_page.end() || h.lsn < it->second) {
          per_page[h.page_id] = h.lsn;
        }
        break;
      }
      case WalRecordType::kCommit:
        txn_table[h.txn_id].committed = true;
        break;
      case WalRecordType::kAbort:
        txn_table[h.txn_id].aborted = true;
        break;
      default:
        break;  // kAllocPage / kFreePage / kCheckpointBegin：见下面的说明
    }
    return true;
  });
  LTM_RETURN_IF_ERROR(scan);

  im.report.records_scanned = scanned;
  im.report.end_lsn = max_lsn;
  im.report.redo_start_lsn = redo_start;
  im.report.txns_seen = txn_table.size();
  im.report.dirty_pages = dirty.size();

  // 失败者（loser）= 既没提交也没中止的事务。单写者下最多只会有一个"当前"事务，
  // 但崩溃点落在检查点之前时可能积累多个。
  std::vector<TxnId> losers;
  for (const auto& kv : undo_first) {
    const auto it = txn_table.find(kv.first);
    if (it == txn_table.end() || (!it->second.committed && !it->second.aborted)) {
      losers.push_back(kv.first);
    }
  }
  std::sort(losers.begin(), losers.end());
  im.report.losers = losers.size();

  // 【干净关闭的短路】上次干净关闭 ⇒ 所有脏页都在关闭时落盘了，Redo 必然是空转
  // （页头的 page_lsn 已经 >= 每一条记录的 LSN）。此时跳过 Redo/Undo 是**正确**的，
  // 而不是"偷懒"：这也是检查点能压缩恢复时间的同一原理。崩溃路径永远走不到这里 ——
  // 强杀不会把 clean_shutdown 置 1。
  if (im.prev_clean && losers.empty()) {
    im.report.records_skipped = scanned;
    im.report.recovery_needed = false;
    im.report.end_lsn = im.wal->last_lsn();
    im.report.elapsed_micros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    LOG_INFO("Recover: the previous run shut down cleanly and no transaction is in flight; "
             "no redo and no undo needed (",
             scanned, " records scanned)");
    return Status::Ok();
  }

  im.report.recovery_needed = true;
  LOG_INFO("Recover: analysis done -- ", scanned, " records, ", dirty.size(), " dirty pages, ",
           txn_table.size(), " transactions, ", losers.size(), " loser(s); redo starts at lsn ", redo_start);

  // ========================================================================
  // 2. Redo
  //
  // 逐条应用 after-image，顺序严格是：
  //     写区间 → 置 page_lsn → **重算校验和** → 落盘
  // 顺序不能变：页的 CRC 覆盖 page_lsn（page.h 的注释与 M4-2 的缺陷 #C8.4 都写明了），
  // 先算 CRC 再改 LSN 会让每个被重放的页在下次打开时判成校验和不符。
  // ========================================================================
  std::unordered_set<PageId> redone_pages;
  // 扫描回调返回 bool（false = 停止扫描），因此不能在里面用 LTM_RETURN_IF_ERROR ——
  // 那会 return 一个 Status 而不是 bool。失败状态统一收进 err。
  Status err;
  Status redo_status = ForEachWalRecord(im.disk, [&](const WalRecordHeader& h, const char* data) {
    if (h.type != WalRecordType::kPageRedo && h.type != WalRecordType::kClrRedo) {
      return true;
    }
    if (h.lsn <= redo_start) {
      return true;  // 检查点之前：这些改动已经随检查点的 flush_all 落到数据文件里
    }
    if (dirty.find(h.page_id) == dirty.end()) {
      return true;  // 该页没有任何需要重放的记录（分析阶段已判定）
    }
    if (h.data_len == 0 || static_cast<uint32_t>(h.page_off) + h.data_len > kPageSize) {
      ++im.report.records_skipped;
      return true;
    }
    std::vector<char> img(kPageSize, 0);
    Status s = im.disk->ReadPage(h.page_id, img.data());
    if (!s.ok()) {
      err = s;
      return false;
    }
    const bool never_written = PageNeverWritten(img.data());
    const bool damaged = PageDamaged(img.data(), h.page_id);
    if (!damaged) {
      const Page p(img.data());
      if (p.header().page_lsn >= h.lsn) {
        ++im.report.records_skipped;  // 幂等：这一页已经包含了本条记录的效果
        return true;
      }
    } else if (!never_written) {
      // 损坏页：不走幂等判断，一律重放（否则损坏会被"跳过"永久留下）。
      // 只统计一次 —— 同一页的第一条重做记录修好它之后，后面就再也读不到损坏了。
      const Page p(img.data());
      if (p.trailer().page_id_copy != h.page_id) {
        ++im.report.torn_pages_repaired;
      } else {
        ++im.report.checksum_repairs;
      }
    }
    std::memcpy(img.data() + h.page_off, data, h.data_len);
    Page out(img.data());
    out.mutable_header()->page_lsn = h.lsn;
    out.UpdateChecksum();
    s = im.disk->WritePage(h.page_id, img.data());
    if (!s.ok()) {
      err = s;
      return false;
    }
    ++im.report.records_redone;
    redone_pages.insert(h.page_id);
    return true;
  });
  LTM_RETURN_IF_ERROR(redo_status);
  LTM_RETURN_IF_ERROR(err);
  im.report.pages_redone = redone_pages.size();

  // ========================================================================
  // 3. Undo
  //
  // 对每个 loser 事务：把它的 (事务,页) 目标集逐页回滚，每页写一条 CLR。
  // CLR 的作用不是"好看"：它让被回滚的页获得一个比原更新记录更大的 page_lsn，
  // 于是**再次崩溃后再恢复**时，Redo 会正确地跳过那些已经回滚掉的更新。
  // 少了它，第二次恢复会把未提交事务的 after-image 重新放回去 —— 而且此后
  // 没有任何东西能把它再回滚掉（事务已经写了 kAbort）。
  // ========================================================================
  uint64_t undone_txns = 0;
  Status err2;
  for (auto it = losers.rbegin(); it != losers.rend(); ++it) {  // 逆序：与日志顺序相反
    const TxnId txn = *it;
    const auto& targets = undo_first[txn];
    bool applied_any = false;
    Status undo_status = ForEachWalRecord(im.disk, [&](const WalRecordHeader& h, const char* data) {
      if (h.type != WalRecordType::kOpUndo || h.txn_id != txn) {
        return true;
      }
      const auto pit = targets.find(h.page_id);
      if (pit == targets.end() || pit->second != h.lsn) {
        return true;  // 同一页只取最早的那条 UNDO
      }
      if (static_cast<uint32_t>(h.page_off) + h.data_len > kPageSize) {
        return true;
      }
      std::vector<char> img(kPageSize, 0);
      Status s = im.disk->ReadPage(h.page_id, img.data());
      if (!s.ok()) {
        err2 = s;
        return false;
      }
      std::memcpy(img.data() + h.page_off, data, h.data_len);
      // CLR 必须先落日志（拿到 LSN）再改页：页头要存的是 CLR 的 LSN
      PageImageDelta clr;
      clr.page_id = h.page_id;
      clr.offset = 0;
      clr.after.assign(img.begin(), img.end());
      auto clr_lsn = im.wal->AppendClr(txn, clr);
      if (!clr_lsn.ok()) {
        err2 = clr_lsn.status();
        return false;
      }
      Page out(img.data());
      out.mutable_header()->page_lsn = clr_lsn.value();
      out.UpdateChecksum();
      s = im.disk->WritePage(h.page_id, img.data());
      if (!s.ok()) {
        err2 = s;
        return false;
      }
      ++im.report.undo_records_applied;
      ++im.report.clrs_written;
      applied_any = true;
      return true;
    });
    LTM_RETURN_IF_ERROR(undo_status);
    LTM_RETURN_IF_ERROR(err2);
    // 写 kAbort：下次恢复看到它就知道这个事务已经回滚完了，不必再回滚一次。
    LTM_RETURN_IF_ERROR(im.wal->Abort(txn));
    if (applied_any) {
      ++undone_txns;
    }
    LOG_INFO("Recover: rolled back txn ", txn, " (", targets.size(), " page(s))");
  }
  im.report.txns_undone = undone_txns;
  if (!losers.empty()) {
    LTM_RETURN_IF_ERROR(im.wal->Sync());
  }

  // ---- 4. 收尾 ----
  LTM_RETURN_IF_ERROR(im.disk->SyncPageFile());
  im.report.end_lsn = im.wal->last_lsn();
  im.report.elapsed_micros = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  LOG_INFO("Recover: done in ", im.report.elapsed_micros, " us -- redone records ",
           im.report.records_redone, " (", im.report.pages_redone, " pages), skipped ",
           im.report.records_skipped, ", undo records ", im.report.undo_records_applied, " (",
           im.report.clrs_written, " CLRs), torn pages repaired ", im.report.torn_pages_repaired,
           ", checksum repairs ", im.report.checksum_repairs);
  return Status::Ok();
}

const RecoveryManager::Report& RecoveryManager::report() const { return impl_->report; }

WalWriter* RecoveryManager::wal() { return impl_->wal.get(); }

Lsn RecoveryManager::checkpoint_lsn() const { return impl_->report.checkpoint_lsn; }

void RecoveryManager::set_checkpoint_lsn(Lsn lsn) {
  impl_->report.checkpoint_lsn = lsn;
  impl_->header.last_checkpoint_lsn = lsn;
}

const RecoveryManager::LogFileHeader& RecoveryManager::log_header() const { return impl_->header; }

bool RecoveryManager::previous_run_was_clean() const { return impl_->prev_clean; }

Status RecoveryManager::WriteLogHeader(const LogFileHeader& header) {
  if (impl_->disk == nullptr) {
    return InvalidArgument("WriteLogHeader needs a DiskManager");
  }
  impl_->header = header;
  return WriteWalLogHeader(impl_->disk, header);
}

Status RecoveryManager::ReadLogHeader(LogFileHeader* out) {
  if (impl_->disk == nullptr) {
    return InvalidArgument("ReadLogHeader needs a DiskManager");
  }
  LTM_RETURN_IF_ERROR(ReadWalLogHeader(impl_->disk, out));
  impl_->header = *out;
  return Status::Ok();
}

Status RecoveryManager::PersistLogHeader() {
  if (impl_->disk == nullptr) {
    return InvalidArgument("PersistLogHeader needs a DiskManager");
  }
  return WriteWalLogHeader(impl_->disk, impl_->header);
}

Status RecoveryManager::MarkCleanShutdown() {
  if (impl_->disk == nullptr) {
    return InvalidArgument("MarkCleanShutdown needs a DiskManager");
  }
  impl_->header.clean_shutdown = 1;
  return WriteWalLogHeader(impl_->disk, impl_->header);
}

Status RecoveryManager::MarkUncleanShutdown() {
  if (impl_->disk == nullptr) {
    return InvalidArgument("MarkUncleanShutdown needs a DiskManager");
  }
  impl_->header.clean_shutdown = 0;
  return WriteWalLogHeader(impl_->disk, impl_->header);
}

// ---------------------------------------------------------------------------
// CheckpointManager
//
// 四个阶段与故障注入点一一对应（方案 §4.4.1）。顺序不可交换：
//   begin → flush_all → end → （头部 + 截断）
// flush_all 必须在 end 之前：end 记录一旦落盘，恢复就会从它开始重放，
// 而"从它开始"的前提正是"它之前的脏页都已经在数据文件里了"。
// ---------------------------------------------------------------------------
struct CheckpointManager::Impl {
  Impl(const WalOptions& o, DiskManager* d, RecoveryManager* r, BufferPool* p)
      : opt(o), disk(d), recovery(r), pool(p) {}
  WalOptions opt;
  DiskManager* disk;
  RecoveryManager* recovery;
  BufferPool* pool;
  uint64_t num_checkpoints = 0;
  uint64_t last_kept_bytes = 0;
};

CheckpointManager::CheckpointManager(const WalOptions& options, DiskManager* disk,
                                     RecoveryManager* recovery, BufferPool* pool)
    : impl_(new Impl(options, disk, recovery, pool)) {}

CheckpointManager::~CheckpointManager() = default;

Status CheckpointManager::RunCheckpoint() {
  Impl& im = *impl_;
  if (im.disk == nullptr || im.recovery == nullptr || im.pool == nullptr) {
    return InvalidArgument("RunCheckpoint needs a DiskManager, a RecoveryManager and a BufferPool");
  }
  WalWriter* wal = im.recovery->wal();
  if (wal == nullptr) {
    return InvalidArgument("RunCheckpoint needs a WalWriter (owned by the RecoveryManager)");
  }
  // 【不许在写事务中途做检查点】检查点会截断日志，而截断的语义是"end 之前的一切都
  // 已经落在数据文件里了"—— 对**已提交**的改动，这条成立；但对一个还开着的写事务，
  // 它的 before-image（UNDO）会被一起截掉，于是"崩溃后回滚这个事务"这件事就失去了
  // 依据：页里可能已经带着未提交的改动（steal 刷盘），而日志里再也找不到它的原始版本。
  // 本设计的检查点由应用在两次操作之间发起，所以这条约束不会带来任何不便。
  if (im.pool->write_op_active()) {
    return InvalidArgument(
        "a checkpoint must not run while a write transaction is in flight: truncating the log "
        "would drop the undo images of the open transaction");
  }

  // 1) begin 记录。记下它在文件里的偏移 —— 截断时要从这里保留起。
  FaultInjector::OnCheckpointStage(1);
  auto mark = wal->AppendCheckpointBegin();
  if (!mark.ok()) {
    return mark.status();
  }

  // 2) 全部脏页落盘（含元数据页）。这一步之后，"end 之前的一切"都在数据文件里了。
  FaultInjector::OnCheckpointStage(2);
  LTM_RETURN_IF_ERROR(im.pool->FlushAll());

  // 3) end 记录 + fsync。end 的 LSN 就是新的 last_checkpoint_lsn。
  FaultInjector::OnCheckpointStage(3);
  auto end = wal->AppendCheckpointEnd(mark.value().lsn);
  if (!end.ok()) {
    return end.status();
  }
  LTM_RETURN_IF_ERROR(wal->Sync());

  // 4) 更新头部 + 截断日志。
  //
  // 【为什么不能直接 TruncateWal(mark.offset)】DiskManager::TruncateWal 把保留区搬到
  // 文件偏移 0，那会把 512 字节的日志头一起冲掉。所以顺序是：
  //   读出要保留的字节 → TruncateWal(文件末尾) 清空 → 重写头部 → 追加保留区。
  // 用 TruncateWal(WalSize()) 而不是 TruncateWal(0) 是有意的：前者"一条都不保留"，
  // 内部不会把整个日志读进内存（后者的 remain 是文件全长）。
  FaultInjector::OnCheckpointStage(4);
  const uint64_t keep_from = mark.value().offset;
  const uint64_t total = im.disk->WalSize();
  if (keep_from > total) {
    return Status::Error(StatusCode::kCorruption,
                         "the checkpoint begin record is past the end of the log");
  }
  std::vector<char> kept(static_cast<size_t>(total - keep_from), 0);
  if (!kept.empty()) {
    LTM_RETURN_IF_ERROR(im.disk->ReadWal(keep_from, kept.data(), kept.size()));
  }
  im.recovery->set_checkpoint_lsn(end.value());
  LTM_RETURN_IF_ERROR(im.disk->TruncateWal(total));  // 清空日志文件
  LTM_RETURN_IF_ERROR(im.recovery->PersistLogHeader());  // 头部回到偏移 0
  if (!kept.empty()) {
    LTM_RETURN_IF_ERROR(im.disk->AppendWal(kept.data(), kept.size()));
  }
  LTM_RETURN_IF_ERROR(im.disk->SyncWal());

  ++im.num_checkpoints;
  im.last_kept_bytes = im.disk->WalSize();
  LOG_INFO("checkpoint #", im.num_checkpoints, ": flushed all dirty pages, wrote the end record at lsn ",
           end.value(), ", truncated the log from ", total, " to ", im.disk->WalSize(), " bytes");
  return Status::Ok();
}

uint64_t CheckpointManager::num_checkpoints() const { return impl_->num_checkpoints; }

uint64_t CheckpointManager::last_kept_bytes() const { return impl_->last_kept_bytes; }

}  // namespace ltm

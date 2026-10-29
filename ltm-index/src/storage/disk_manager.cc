#include "storage/disk_manager.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "storage/fault_inject.h"
#include "util/log.h"

#if defined(_WIN32)
#include <io.h>
#include <share.h>  // _SH_DENYNO，_sopen_s 的共享模式
#else
#include <unistd.h>
#endif

namespace ltm {

namespace {

std::string ParentDir(const std::string& path) {
  std::filesystem::path p(path);
  return p.has_parent_path() ? p.parent_path().string() : std::string(".");
}

Status EnsureParent(const std::string& path) {
  const std::string dir = ParentDir(path);
  if (dir.empty() || dir == ".") {
    return Status::Ok();
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return Status::Error(StatusCode::kIoError, "cannot create directory " + dir + ": " + ec.message());
  }
  return Status::Ok();
}

// fsync equivalent: push the file's bytes all the way down to the device.
//
// Why a bare flush() is not enough: fstream::flush() only hands the user-space
// buffer to the OS. The bytes may still sit in the kernel/device cache, so a
// power loss loses them. Invariant I6 requires "a dirty page may not reach disk
// before its REDO has been fsynced", so this must be a real device barrier.
//
// Why a second handle: std::fstream exposes no native handle (_fileno needs a
// FILE*, and an fstream is not bound to one). The Windows cache manager caches
// dirty pages per *file*, so FlushFileBuffers (what the CRT wraps as _commit)
// on any GENERIC_WRITE handle to that file flushes the file's dirty data --
// including bytes written through the fstream's own handle. On POSIX, fsync
// acts on the open file description, so opening a second fd and fsyncing it
// reaches the same inode.
//
// Cost: one extra open/close per sync, negligible next to a real fsync, and it
// keeps SyncFstream stateless (no handle lifetime to track in Impl).
Status SyncFstream(std::fstream& f, const char* what) {
  f.flush();
  if (!f) {
    return Status::Error(StatusCode::kIoError, std::string("flush failed: ") + what);
  }
#if defined(_WIN32)
  int fd = -1;
  if (_sopen_s(&fd, what, _O_WRONLY | _O_BINARY, _SH_DENYNO, 0) != 0 || fd < 0) {
    return Status::Error(StatusCode::kIoError, std::string("fsync open failed: ") + what);
  }
  const int rc = _commit(fd);
  _close(fd);
  if (rc != 0) {
    return Status::Error(StatusCode::kIoError, std::string("fsync failed: ") + what);
  }
#else
  const int fd = ::open(what, O_WRONLY);
  if (fd < 0) {
    return Status::Error(StatusCode::kIoError, std::string("fsync open failed: ") + what);
  }
  const int rc = ::fsync(fd);
  const int saved = errno;
  ::close(fd);
  if (rc != 0) {
    return Status::Error(StatusCode::kIoError,
                         std::string("fsync failed: ") + what + ": " + std::strerror(saved));
  }
#endif
  return Status::Ok();
}

}  // namespace

struct DiskManager::Impl {
  Impl(const DiskOptions& o) : opt(o) {}
  DiskOptions opt;
  IoStats stats;
  std::fstream page_file;
  std::fstream wal_file;
  uint64_t wal_size = 0;
  bool opened = false;
};

DiskManager::DiskManager(const DiskOptions& options) : impl_(new Impl(options)) {
  Impl& im = *impl_;
  if (im.opt.page_size < 512 || (im.opt.page_size & (im.opt.page_size - 1)) != 0) {
    LOG_ERROR("page_size must be a power of two >= 512, got ", im.opt.page_size);
    return;
  }
  if (!EnsureParent(im.opt.page_file).ok() || !EnsureParent(im.opt.wal_file).ok()) {
    LOG_ERROR("cannot create the data directory");
    return;
  }
  im.page_file.open(im.opt.page_file, std::ios::in | std::ios::out | std::ios::binary);
  if (!im.page_file) {
    // 文件不存在则以截断方式创建
    im.page_file.clear();
    im.page_file.open(im.opt.page_file, std::ios::out | std::ios::binary);
    im.page_file.close();
    im.page_file.open(im.opt.page_file, std::ios::in | std::ios::out | std::ios::binary);
  }
  im.wal_file.open(im.opt.wal_file, std::ios::in | std::ios::out | std::ios::binary);
  if (!im.wal_file) {
    im.wal_file.clear();
    im.wal_file.open(im.opt.wal_file, std::ios::out | std::ios::binary);
    im.wal_file.close();
    im.wal_file.open(im.opt.wal_file, std::ios::in | std::ios::out | std::ios::binary);
  }
  im.wal_file.seekg(0, std::ios::end);
  im.wal_size = static_cast<uint64_t>(im.wal_file.tellg());
  im.opened = static_cast<bool>(im.page_file) && static_cast<bool>(im.wal_file);
  LOG_INFO("DiskManager opened: page_file=", im.opt.page_file, " wal_file=", im.opt.wal_file,
           " page_size=", im.opt.page_size, " wal_size=", im.wal_size,
           im.opened ? "" : "  [open failed]");
}

DiskManager::~DiskManager() {
  Impl& im = *impl_;
  if (im.page_file) {
    im.page_file.flush();
    im.page_file.close();
  }
  if (im.wal_file) {
    im.wal_file.flush();
    im.wal_file.close();
  }
}

Status DiskManager::ReadPage(PageId pid, char* out) {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kIoError, "DiskManager is not ready");
  }
  if (pid >= im.opt.num_pages) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "page number out of range: " + std::to_string(pid) + " >= " +
                             std::to_string(im.opt.num_pages));
  }
  const std::streamoff off = static_cast<std::streamoff>(pid) * im.opt.page_size;
  im.page_file.clear();
  im.page_file.seekg(off);
  im.page_file.read(out, im.opt.page_size);
  const std::streamsize got = im.page_file.gcount();
  if (got < static_cast<std::streamsize>(im.opt.page_size)) {
    // 读到文件末尾之外（页从未写过）：补零并计数。
    // 这不是错误 —— 新分配页的第一次读取必然落在这里。
    std::memset(out + got, 0, im.opt.page_size - static_cast<size_t>(got));
    im.page_file.clear();
  }
  ++im.stats.page_reads;
  im.stats.bytes_read += im.opt.page_size;
  return Status::Ok();
}

Status DiskManager::WritePage(PageId pid, const char* in) {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kIoError, "DiskManager is not ready");
  }
  if (pid >= im.opt.num_pages) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "page number out of range: " + std::to_string(pid) + " >= " +
                             std::to_string(im.opt.num_pages));
  }
  FaultInjector::OnBeforePageFlush();
  const std::streamoff off = static_cast<std::streamoff>(pid) * im.opt.page_size;
  im.page_file.clear();
  im.page_file.seekp(off);
  im.page_file.write(in, im.opt.page_size);
  if (!im.page_file) {
    return Status::Error(StatusCode::kIoError, "write page failed: pid=" + std::to_string(pid));
  }
  ++im.stats.page_writes;
  im.stats.bytes_written += im.opt.page_size;
  if (im.opt.sync_on_write) {
    LTM_RETURN_IF_ERROR(SyncPageFile());
  }
  return Status::Ok();
}

Status DiskManager::SyncPageFile() {
  Impl& im = *impl_;
  LTM_RETURN_IF_ERROR(SyncFstream(im.page_file, im.opt.page_file.c_str()));
  ++im.stats.fsyncs;
  return Status::Ok();
}

uint64_t DiskManager::num_pages() const { return impl_->opt.num_pages; }

StatusOr<uint64_t> DiskManager::PageFileSize() {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kIoError, "DiskManager is not ready");
  }
  im.page_file.clear();
  im.page_file.flush();
  if (!im.page_file) {
    return Status::Error(StatusCode::kIoError, "page file flush failed before size query");
  }
  im.page_file.seekg(0, std::ios::end);
  const auto end = im.page_file.tellg();
  if (!im.page_file || end < std::streampos(0)) {
    return Status::Error(StatusCode::kIoError, "page file size query failed");
  }
  return static_cast<uint64_t>(end);
}

uint32_t DiskManager::page_size() const { return impl_->opt.page_size; }

Status DiskManager::AppendWal(const char* buf, size_t len) {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kIoError, "DiskManager is not ready");
  }
  FaultInjector::OnWalAppend();
  im.wal_file.clear();
  im.wal_file.seekp(static_cast<std::streamoff>(im.wal_size));
  im.wal_file.write(buf, static_cast<std::streamsize>(len));
  if (!im.wal_file) {
    return Status::Error(StatusCode::kIoError, "WAL append failed");
  }
  im.wal_size += len;
  ++im.stats.wal_appends;
  im.stats.wal_bytes += len;
  return Status::Ok();
}

Status DiskManager::SyncWal() {
  Impl& im = *impl_;
  LTM_RETURN_IF_ERROR(SyncFstream(im.wal_file, im.opt.wal_file.c_str()));
  ++im.stats.fsyncs;
  return Status::Ok();
}

Status DiskManager::ReadWal(uint64_t offset, char* out, size_t len) {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kIoError, "DiskManager is not ready");
  }
  if (offset + len > im.wal_size) {
    return Status::Error(StatusCode::kInvalidArgument, "WAL read out of range");
  }
  im.wal_file.clear();
  im.wal_file.seekg(static_cast<std::streamoff>(offset));
  im.wal_file.read(out, static_cast<std::streamsize>(len));
  if (im.wal_file.gcount() != static_cast<std::streamsize>(len)) {
    return Status::Error(StatusCode::kIoError, "WAL read returned too few bytes");
  }
  return Status::Ok();
}

Status DiskManager::WriteWalAt(uint64_t offset, const char* buf, size_t len) {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kIoError, "DiskManager is not ready");
  }
  if (len == 0) {
    return Status::Ok();
  }
  im.wal_file.clear();
  im.wal_file.seekp(static_cast<std::streamoff>(offset));
  im.wal_file.write(buf, static_cast<std::streamsize>(len));
  if (!im.wal_file) {
    return Status::Error(StatusCode::kIoError, "WAL random write failed at offset " + std::to_string(offset));
  }
  if (offset + len > im.wal_size) {
    im.wal_size = offset + len;
  }
  ++im.stats.wal_appends;
  im.stats.wal_bytes += len;
  return Status::Ok();
}

uint64_t DiskManager::WalSize() const { return impl_->wal_size; }

Status DiskManager::TruncateWal(uint64_t keep_from_offset) {
  Impl& im = *impl_;
  if (keep_from_offset > im.wal_size) {
    return InvalidArgument("TruncateWal: keep_from exceeds the log length");
  }
  const size_t remain = static_cast<size_t>(im.wal_size - keep_from_offset);
  std::vector<char> buf(remain);
  if (remain > 0) {
    LTM_RETURN_IF_ERROR(ReadWal(keep_from_offset, buf.data(), remain));
  }
  im.wal_size = 0;
  im.wal_file.close();
  im.wal_file.open(im.opt.wal_file, std::ios::out | std::ios::binary | std::ios::trunc);
  im.wal_file.close();
  im.wal_file.open(im.opt.wal_file, std::ios::in | std::ios::out | std::ios::binary);
  if (remain > 0) {
    LTM_RETURN_IF_ERROR(AppendWal(buf.data(), remain));
  }
  LOG_INFO("WAL truncate: kept ", remain, " bytes (was ", keep_from_offset + remain, ")");
  return Status::Ok();
}

const DiskManager::IoStats& DiskManager::stats() const { return impl_->stats; }

void DiskManager::ResetStats() { impl_->stats = IoStats{}; }

const DiskOptions& DiskManager::options() const { return impl_->opt; }

}  // namespace ltm

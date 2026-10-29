// tests/crash/crash_test.cc
// M4-3 / M5 崩溃矩阵的执行程序（方案 §5.2 的守门程序 2，由 scripts/crash_matrix.py 驱动）。
//
// 【判据】恢复后的全量遍历必须**逐条等于**崩溃前已提交的状态，且结构不变量全绿。
// "已提交"这件事由一个**旁路日志**（journal.txt）定义：每完成一次操作、且该操作
// 返回成功之后，立刻把这条操作追加进 journal 并 fsync。恢复之后：
//   期望集合 = journal 回放的结果；实际集合 = 从恢复后的树里扫出来的结果。
// 两者必须逐条相同。
//
// 【为什么这个旁路日志是可信的，而不是"循环论证"】崩溃注入点只有两个：
// DiskManager::AppendWal 与 DiskManager::WritePage，而它们都只在**树操作内部**被调用。
// journal 的写入发生在操作返回之后，路径上没有任何 WAL 追加或页写盘 —— 于是
// "崩溃"永远发生在某次操作的中途，不可能落在"操作已提交但 journal 还没写"这个窗口里。
// 这是本判据成立的前提，所以它必须写在代码里（就是这一段）。
//
// 【两个引擎】`--engine=bplus` 跑单棵 B+ 树（M4-3 的验收对象）；
// `--engine=hybrid` 跑 HybridIndex（M5）—— 此时"两棵树要么都生效、要么都回滚"
// 这件事被同一条判据覆盖：恢复后每条已提交记录都必须 Get 得到，且 I8 交叉校验全绿。
//
// 【为什么由 Python 分三段跑，而不是程序自己 fork】本机项目路径含非 ASCII 字符，
// std::system 传出去的路径会经过一次 ANSI/GBK 往返；分三段（work / corrupt / verify）
// 让每一步都是一个干净的进程，路径由 Python 用 Unicode 传给 CreateProcess，
// 不经过任何命令行编码转换。
//
// 退出码：0 = 本段正常完成；9 = 故障注入触发的强杀（_exit(9)，与真实崩溃同形）；
//         1 = 本段自身失败（参数错误 / 断言失败）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "index/bplus_tree.h"
#include "index/hybrid_index.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/fault_inject.h"
#include "storage/page.h"
#include "storage/recovery.h"
#include "storage/wal.h"
#include "util/log.h"

using namespace ltm;

namespace {

constexpr uint16_t kBPlusCapacity = 32;
constexpr uint16_t kRTreeCapacity = 32;
constexpr uint16_t kDim = 2;
constexpr TreeId kBPlusTreeId = 1;
constexpr uint32_t kPoolFrames = 24;
constexpr size_t kPayloadLen = 24;

struct Args {
  std::string phase = "work";
  std::string engine = "bplus";
  std::string inject = "wal_append";
  std::string workload = "sequential";
  std::string dir = "log/crash";
  std::string end_mode = "clean";  // clean | unclean
  std::string corrupt_mode = "tear";
  uint64_t n = 1;
  uint64_t ops = 300;
  uint64_t seed = 42;
  uint64_t checkpoint_every = 0;  // 0 = 不跑检查点
  bool expect_repair = false;
};

Args Parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto val = [&s](const char* p) { return s.substr(std::strlen(p)); };
    if (s.rfind("--phase=", 0) == 0) {
      a.phase = val("--phase=");
    } else if (s.rfind("--engine=", 0) == 0) {
      a.engine = val("--engine=");
    } else if (s.rfind("--inject=", 0) == 0) {
      a.inject = val("--inject=");
    } else if (s.rfind("--workload=", 0) == 0) {
      a.workload = val("--workload=");
    } else if (s.rfind("--dir=", 0) == 0) {
      a.dir = val("--dir=");
    } else if (s.rfind("--end=", 0) == 0) {
      a.end_mode = val("--end=");
    } else if (s.rfind("--corrupt-mode=", 0) == 0) {
      a.corrupt_mode = val("--corrupt-mode=");
    } else if (s.rfind("--n=", 0) == 0) {
      a.n = std::strtoull(val("--n=").c_str(), nullptr, 10);
    } else if (s.rfind("--ops=", 0) == 0) {
      a.ops = std::strtoull(val("--ops=").c_str(), nullptr, 10);
    } else if (s.rfind("--seed=", 0) == 0) {
      a.seed = std::strtoull(val("--seed=").c_str(), nullptr, 10);
    } else if (s.rfind("--checkpoint-every=", 0) == 0) {
      a.checkpoint_every = std::strtoull(val("--checkpoint-every=").c_str(), nullptr, 10);
    } else if (s == "--expect-repair") {
      a.expect_repair = true;
    }
  }
  return a;
}

std::string PageFile(const Args& a) { return a.dir + "/crash.data"; }
std::string WalFile(const Args& a) { return a.dir + "/crash.wal"; }
std::string JournalFile(const Args& a) { return a.dir + "/journal.txt"; }

std::vector<char> PayloadFor(uint64_t key) {
  std::vector<char> p(kPayloadLen, 'x');
  for (size_t i = 0; i < kPayloadLen; ++i) {
    p[i] = static_cast<char>('a' + ((key + i * 7) % 26));
  }
  return p;
}

// 由键导出的确定性 MBR（混合引擎用）。刻意不用随机数：work 与 verify 是两个
// 独立进程，任何"共享随机状态"的假设都会变成隐藏依赖。
Mbr MbrForKey(uint64_t key) {
  const double x = static_cast<double>((key * 2654435761u) % 10000u) / 10.0;
  const double y = static_cast<double>((key * 40503u) % 10000u) / 10.0;
  const double e = 1.0 + static_cast<double>(key % 5u);
  Mbr m = Mbr::Invalid(kDim);
  m.lo.resize(kDim);
  m.hi.resize(kDim);
  m.lo[0] = x - e;
  m.hi[0] = x + e;
  m.lo[1] = y - e;
  m.hi[1] = y + e;
  return m;
}

IndexEntry MakeEntry(uint64_t key, bool with_mbr) {
  IndexEntry e;
  e.key = key;
  e.payload = PayloadFor(key);
  if (with_mbr) {
    e.mbr = MbrForKey(key);
  }
  return e;
}

// 期望集合：key -> payload 长度（内容由 key 决定，所以长度足够判定一致）
using Expected = std::map<uint64_t, size_t>;

// journal 追加一行并 fsync。fsync 是必须的：不 fsync 的话，崩溃后 journal 可能
// 少掉最后几行，而树里却有对应的已提交数据 —— 判据会变成"假失败"。
bool JournalAppend(FILE* f, char op, uint64_t key) {
  std::fprintf(f, "%c %llu\n", op, static_cast<unsigned long long>(key));
  std::fflush(f);
#if defined(_WIN32)
  return _commit(_fileno(f)) == 0;
#else
  return ::fsync(fileno(f)) == 0;
#endif
}

bool JournalLoad(const std::string& path, Expected* out) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 3) {
      continue;
    }
    const char op = line[0];
    const uint64_t key = std::strtoull(line.c_str() + 2, nullptr, 10);
    if (op == 'I') {
      (*out)[key] = kPayloadLen;
    } else if (op == 'D') {
      out->erase(key);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 引擎抽象：两个引擎跑同一套 work / verify 判据
//
// 【为什么要抽象】M4-3 的判据（"恢复后全量遍历 == 旁路日志"）对 M5 一字不改地成立，
// 而且对混合索引它还额外覆盖了 I8（两棵树要么都生效、要么都回滚）。把判据写两遍
// 等于允许两边漂移 —— 那正是"看起来测了两件事，其实只测了一件"的来源。
// ---------------------------------------------------------------------------
class Engine {
 public:
  virtual ~Engine() = default;
  virtual Status Open() = 0;
  virtual Status Put(uint64_t key) = 0;
  virtual Status Erase(uint64_t key) = 0;
  // 全量遍历：回调拿到 (key, payload_len)
  virtual Status ScanAll(const std::function<bool(uint64_t, size_t)>& emit) = 0;
  // 单条读取：命中返回 found=true 并给出载荷长度
  virtual Status GetOne(uint64_t key, size_t* payload_len, bool* found) = 0;
  virtual Status Validate() = 0;
  virtual Status Close() = 0;
  virtual Status RunCheckpoint() = 0;
  // 是否支持"刷盘但不标干净关闭"（损坏页用例需要它来构造现场）
  virtual bool supports_unclean_end() const = 0;
  virtual Status MarkUnclean() = 0;
};

// ---- 单棵 B+ 树 ----
class BPlusEngine final : public Engine {
 public:
  explicit BPlusEngine(const Args& a) : ops_(kBPlusCapacity) {
    DiskOptions dopt;
    dopt.page_file = PageFile(a);
    dopt.wal_file = WalFile(a);
    dopt.num_pages = 1u << 16;
    disk_.reset(new DiskManager(dopt));
    rec_.reset(new RecoveryManager(WalOptions(), disk_.get()));
    BufferPoolOptions popt;
    popt.pool_frames = kPoolFrames;  // 故意小：逼出"事务中途淘汰脏页"
    pool_.reset(new BufferPool(popt, disk_.get(), rec_->wal(), rec_.get(), nullptr, nullptr));
    ckpt_.reset(new CheckpointManager(WalOptions(), disk_.get(), rec_.get(), pool_.get()));
  }

  Status Open() override {
    // 【顺序是契约】恢复必须在任何索引打开之前完成，且此时池里不应有驻留页。
    // 漏掉这一步的后果不是"少做一点工作"：崩溃留下的页文件与日志不一致，
    // 索引会读到崩溃现场本身（本用例曾经真的漏掉过它，症状是 page 0 全零而
    // 日志非空，被 I6 检查当场抓住）。
    LTM_RETURN_IF_ERROR(rec_->Recover(pool_.get()));
    TreeBuildOptions topt;
    topt.order_or_capacity = kBPlusCapacity;
    topt.caller_tree_slot = 0;
    return tree_.Open(pool_.get(), &ops_, topt, kBPlusTreeId);
  }
  Status Put(uint64_t key) override { return tree_.Insert(MakeEntry(key, false)); }
  Status Erase(uint64_t key) override { return tree_.Erase(MakeEntry(key, false)); }
  Status ScanAll(const std::function<bool(uint64_t, size_t)>& emit) override {
    return tree_.ScanAll([&emit](const RecordRef& r) { return emit(r.key, r.payload_len); });
  }
  Status GetOne(uint64_t key, size_t* len, bool* found) override {
    auto hit = tree_.Search(MakeEntry(key, false));
    if (hit.status().code() == StatusCode::kNotFound) {
      *found = false;
      return Status::Ok();
    }
    if (!hit.ok()) {
      return hit.status();
    }
    *found = true;
    *len = hit.value().payload_len;
    return Status::Ok();
  }
  Status Validate() override { return tree_.Validate(); }
  Status Close() override {
    LTM_RETURN_IF_ERROR(pool_->FlushAll());
    return rec_->MarkCleanShutdown();
  }
  Status RunCheckpoint() override { return ckpt_->RunCheckpoint(); }
  bool supports_unclean_end() const override { return true; }
  Status MarkUnclean() override {
    LTM_RETURN_IF_ERROR(pool_->FlushAll());
    return rec_->MarkUncleanShutdown();
  }

 private:
  BPlusPageOps ops_;
  BPlusTree tree_;
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<RecoveryManager> rec_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<CheckpointManager> ckpt_;
};

// ---- 混合索引（B+ 与 R 树，单事务双树）----
class HybridEngine final : public Engine {
 public:
  explicit HybridEngine(const Args& a) : index_(new HybridIndex()) {
    opt_.config.disk.page_file = PageFile(a);
    opt_.config.disk.wal_file = WalFile(a);
    opt_.config.disk.num_pages = 1u << 16;
    opt_.config.pool.pool_frames = kPoolFrames;
    opt_.bplus.order_or_capacity = kBPlusCapacity;
    opt_.rtree.order_or_capacity = kRTreeCapacity;
    opt_.rtree.mbr_dim = kDim;
    opt_.rtree.split_policy = "quadratic";
  }

  Status Open() override { return index_->Open(opt_); }
  Status Put(uint64_t key) override { return index_->Put(MakeEntry(key, true)); }
  Status Erase(uint64_t key) override { return index_->Erase(key); }
  Status ScanAll(const std::function<bool(uint64_t, size_t)>& emit) override {
    ltm::Query q;
    q.kind = QueryKind::kRange1D;
    q.key_lo = 0;
    q.key_hi = 0xFFFFFFFFFFFFFFFFull;
    return index_->Query(q, [&emit](const IndexEntry& e) { return emit(e.key, e.payload.size()); });
  }
  Status GetOne(uint64_t key, size_t* len, bool* found) override {
    auto hit = index_->Get(key);
    if (hit.status().code() == StatusCode::kNotFound) {
      *found = false;
      return Status::Ok();
    }
    if (!hit.ok()) {
      return hit.status();
    }
    *found = true;
    *len = hit.value().payload.size();
    return Status::Ok();
  }
  Status Validate() override { return index_->Validate(); }
  Status Close() override { return index_->Close(); }
  Status RunCheckpoint() override { return index_->Checkpoint(); }
  bool supports_unclean_end() const override { return false; }
  Status MarkUnclean() override {
    return Status::Error(StatusCode::kNotSupported, "the hybrid engine has no unclean-end hook");
  }

 private:
  HybridOptions opt_;
  std::unique_ptr<HybridIndex> index_;
};

std::unique_ptr<Engine> MakeEngine(const Args& a) {
  if (a.engine == "hybrid") {
    return std::unique_ptr<Engine>(new HybridEngine(a));
  }
  return std::unique_ptr<Engine>(new BPlusEngine(a));
}

// ---------------------------------------------------------------------------
// phase = work：跑负载、写 journal、按注入点配置故障
// ---------------------------------------------------------------------------
int RunWork(const Args& a) {
  std::error_code ec;
  std::filesystem::create_directories(a.dir, ec);
  std::remove(PageFile(a).c_str());
  std::remove(WalFile(a).c_str());
  std::remove(JournalFile(a).c_str());

  std::unique_ptr<Engine> engine = MakeEngine(a);
  if (a.end_mode == "unclean" && !engine->supports_unclean_end()) {
    std::printf("--end=unclean is not supported by the %s engine\n", a.engine.c_str());
    return 1;
  }

  // 故障注入必须在任何写操作之前配置好
  FaultInjector::Reset();
  if (a.inject == "wal_append") {
    FaultInjector::SetCrashAfterWalAppend(a.n);
  } else if (a.inject == "before_flush") {
    FaultInjector::SetCrashBeforePageFlush(a.n);
  } else if (a.inject == "checkpoint_stage") {
    // 撞检查点的第 n 个阶段（1 begin / 2 flush_all / 3 end / 4 truncate）
    FaultInjector::SetCrashDuringCheckpoint(static_cast<int>(a.n));
  }

  FILE* journal = std::fopen(JournalFile(a).c_str(), "wb");
  if (journal == nullptr) {
    std::printf("cannot open the journal for writing\n");
    return 1;
  }

  const Status opened = engine->Open();
  if (!opened.ok()) {
    std::printf("engine open failed: %s\n", opened.ToString().c_str());
    return 1;
  }

  std::mt19937_64 rng(a.seed);
  const uint64_t key_space = a.ops * 2;
  uint64_t done = 0;
  for (uint64_t i = 0; i < a.ops; ++i) {
    uint64_t key = 0;
    bool do_erase = false;
    if (a.workload == "sequential") {
      // 顺序插入；每三条删一条**先前插进去**的键（删自己会全部落空，那 workload 就是空的）
      key = i + 1;
      if (i > 50 && i % 3 == 0) {
        key = i + 1 - 50;
        do_erase = true;
      }
    } else if (a.workload == "random") {
      key = 1 + (rng() % key_space);
      do_erase = (rng() % 100) < 30;
    } else {  // mixed
      key = (rng() % 2 == 0) ? (i + 1) : (1 + (rng() % key_space));
      do_erase = (rng() % 100) < 20;
    }
    Status s;
    if (do_erase) {
      s = engine->Erase(key);
      if (s.code() == StatusCode::kNotFound) {
        continue;  // 本来就不存在：状态没变，不进 journal
      }
      if (!s.ok()) {
        std::printf("erase failed: %s\n", s.ToString().c_str());
        return 1;
      }
      if (!JournalAppend(journal, 'D', key)) {
        std::printf("journal fsync failed\n");
        return 1;
      }
    } else {
      s = engine->Put(key);
      if (!s.ok()) {
        std::printf("insert failed: %s\n", s.ToString().c_str());
        return 1;
      }
      if (!JournalAppend(journal, 'I', key)) {
        std::printf("journal fsync failed\n");
        return 1;
      }
    }
    ++done;
    if (a.checkpoint_every > 0 && (i + 1) % a.checkpoint_every == 0) {
      const Status c = engine->RunCheckpoint();
      if (!c.ok()) {
        std::printf("checkpoint failed: %s\n", c.ToString().c_str());
        return 1;
      }
    }
  }
  std::fclose(journal);

  if (a.end_mode == "unclean") {
    // "断电"：页全部刷盘、日志保持不干净，然后绕过析构直接退出。
    // 用于损坏页用例 —— 它们需要一个**不做干净关闭**的现场，否则恢复会被短路。
    const Status m = engine->MarkUnclean();
    if (!m.ok()) {
      std::printf("marking the log unclean failed: %s\n", m.ToString().c_str());
      return 1;
    }
    std::printf("WORK_DONE_NO_CRASH ops=%llu unclean=1 engine=%s\n",
                static_cast<unsigned long long>(done), a.engine.c_str());
    std::fflush(stdout);
    std::_Exit(0);
  }
  const Status closed = engine->Close();
  if (!closed.ok()) {
    std::printf("clean close failed: %s\n", closed.ToString().c_str());
    return 1;
  }
  std::printf("WORK_DONE_NO_CRASH ops=%llu unclean=0 engine=%s\n",
              static_cast<unsigned long long>(done), a.engine.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// phase = corrupt：在崩溃之后的页文件里制造"撕裂写 / 内容被改坏"
// ---------------------------------------------------------------------------
// 挑的页必须**在日志里有 REDO 记录** —— 否则恢复根本没有素材去修它，
// 用例就成了空跑（这类"看起来跑了其实没测到"的形态在本项目已出现过一次，
// 见缺陷 #37）。
int RunCorrupt(const Args& a) {
  // 这里只需要一个 DiskManager（要扫日志挑页），不要构造缓冲池与恢复管理器：
  // 后者会在打开时改写日志头，属于对现场的额外扰动。
  DiskOptions dopt;
  dopt.page_file = PageFile(a);
  dopt.wal_file = WalFile(a);
  DiskManager disk(dopt);
  std::vector<PageId> pages;
  LTM_UNUSED(ForEachWalRecord(&disk, [&pages](const WalRecordHeader& h, const char*) {
    if (h.type == WalRecordType::kPageRedo && h.page_id != kMetaPageId) {
      for (PageId p : pages) {
        if (p == h.page_id) {
          return true;
        }
      }
      pages.push_back(h.page_id);
    }
    return true;
  }));
  if (pages.empty()) {
    std::printf("no data page in the log is covered by a REDO record\n");
    return 1;
  }
  const PageId victim = pages[0];
  const std::string path = PageFile(a);
  if (a.corrupt_mode == "tear") {
    if (!FaultInjector::TearPageWrite(path.c_str(), kPageSize, victim)) {
      std::printf("TearPageWrite failed (is fault injection enabled?)\n");
      return 1;
    }
  } else {
    if (!FaultInjector::CorruptPageBytes(path.c_str(), kPageSize, victim, 128, 32)) {
      std::printf("CorruptPageBytes failed (is fault injection enabled?)\n");
      return 1;
    }
  }
  std::printf("CORRUPTED page=%u mode=%s (candidates=%zu)\n", static_cast<unsigned>(victim),
              a.corrupt_mode.c_str(), pages.size());
  return 0;
}

// ---------------------------------------------------------------------------
// phase = verify：恢复 → 全量遍历 → 与 journal 回放的期望集合逐条比对
// ---------------------------------------------------------------------------
int RunVerify(const Args& a) {
  Expected expected;
  if (!JournalLoad(JournalFile(a), &expected)) {
    std::printf("cannot read the journal; nothing to verify against\n");
    return 1;
  }
  std::unique_ptr<Engine> engine = MakeEngine(a);
  const Status opened = engine->Open();  // 混合索引的 Open 内部就会跑恢复
  if (!opened.ok()) {
    std::printf("engine open after recovery failed: %s\n", opened.ToString().c_str());
    return 1;
  }

  // 逐条比对：先看期望的每一条是否可查且载荷一致
  size_t missing = 0;
  size_t mismatched = 0;
  for (const auto& kv : expected) {
    size_t len = 0;
    bool found = false;
    const Status s = engine->GetOne(kv.first, &len, &found);
    if (!s.ok()) {
      std::printf("VERIFY_FAIL get failed for key %llu: %s\n",
                  static_cast<unsigned long long>(kv.first), s.ToString().c_str());
      return 1;
    }
    if (!found) {
      ++missing;
      if (missing <= 3) {
        std::printf("  lost key %llu\n", static_cast<unsigned long long>(kv.first));
      }
      continue;
    }
    if (len != kv.second) {
      ++mismatched;
      if (mismatched <= 3) {
        std::printf("  key %llu payload mismatch: len=%zu expected=%zu\n",
                    static_cast<unsigned long long>(kv.first), len, kv.second);
      }
    }
  }
  // 再看树里有没有"多出来"的条目（未提交事务残留、幽灵副本都会在这里现形）
  std::vector<uint64_t> extra;
  const Status sc = engine->ScanAll([&expected, &extra](uint64_t key, size_t) {
    if (expected.find(key) == expected.end()) {
      extra.push_back(key);
    }
    return true;
  });
  if (!sc.ok()) {
    std::printf("VERIFY_FAIL scan failed: %s\n", sc.ToString().c_str());
    return 1;
  }
  const Status v = engine->Validate();
  if (!v.ok()) {
    std::printf("VERIFY_FAIL validate: %s\n", v.ToString().c_str());
    return 1;
  }
  if (missing != 0 || mismatched != 0 || !extra.empty()) {
    std::printf("VERIFY_FAIL missing=%zu mismatched=%zu extra=%zu (expected %zu)\n", missing,
                mismatched, extra.size(), expected.size());
    for (size_t i = 0; i < extra.size() && i < 3; ++i) {
      std::printf("  unexpected key %llu\n", static_cast<unsigned long long>(extra[i]));
    }
    return 1;
  }
  std::printf("VERIFY_OK engine=%s expected=%zu extra=0\n", a.engine.c_str(), expected.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = Parse(argc, argv);
  ltm::Logger::Instance().SetLevel(ltm::LogLevel::kError);
  // 故障注入没开时，work 段会跑完但永远不会崩、corrupt 段也改不动字节 ——
  // 那会让整张矩阵"全绿而无效"。这正是缺陷 #37 的形状，所以直接拒绝。
  if (!FaultInjector::enabled() && (a.phase == "work" || a.phase == "corrupt")) {
    std::printf("fault injection is disabled in this build; build with -DLTM_ENABLE_FAULT_INJECT=ON\n");
    return 1;
  }
  if (a.phase == "work") {
    return RunWork(a);
  }
  if (a.phase == "corrupt") {
    return RunCorrupt(a);
  }
  if (a.phase == "verify") {
    return RunVerify(a);
  }
  std::printf("unknown phase: %s\n", a.phase.c_str());
  return 1;
}

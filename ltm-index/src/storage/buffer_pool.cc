#include "storage/buffer_pool.h"

#include <unordered_set>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "storage/fault_inject.h"
#include "storage/recovery.h"
#include "storage/wal.h"
#include "util/log.h"

namespace ltm {

// 页 0 的布局见 storage/meta_page.h（MetaPage / TreeMetaSlot / MetaTreeKind）。
// 本文件是页 0 的唯一读写者；上层通过 WriteTreeSlot / WriteBufferMeta 改自己的状态。

// ---------------------------------------------------------------------------
// C1 · "本线程是否持有写事务" —— 必须是 thread_local
//
// 【为什么不能直接读 write_op.depth】`write_op` 是**写者私有**的状态（before-image
// 表、undo 动作、分配器快照），但它挂在共享的 Impl 上。而 `NotePinned` 是**每一次
// 取页**都会调的（读者也调）：它先 `if (!InWriteOp()) return;`。若这个判断读的是共享的
// `write_op.depth`，那么写事务进行期间，并发读者会看到 depth>0、以为自己也在写事务里，
// 于是去动写者的 `write_op.before` —— 与写者并发改同一个 `unordered_map`，
// 一次 rehash 就把节点释放掉，另一个线程正好在查它 → **heap-use-after-free**。
// 这正是 ASAN 在"1 写 1 读"下抓到的崩溃（SUMMARY: heap-use-after-free ...
// std::_Hash<...>::_Find_last<unsigned int>）。
//
// 语义上也该如此：它回答的是"**我**这个线程此刻在不在写事务里"，是个线程局部事实。
// ---------------------------------------------------------------------------
thread_local int g_write_op_depth = 0;

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct BufferPool::Impl {
  struct Frame {
    std::vector<char> data;
    PageId pid = kInvalidPageId;
    // M6-1：C0 下读操作共享闩，命中路径会并发地 ++/-- 这个计数，必须是原子的。
    // 其余字段（pid/dirty/valid）在 C0 下仍只由写操作（独占闩）或未命中路径改，
    // 那条路不受共享闩保护 —— 见 buffer_pool.h 里 c0_latch() 的说明。
    std::atomic<int32_t> pin_count{0};
    bool dirty = false;
    bool valid = false;  // 该帧是否正驻留某个页

    Frame() = default;
    // frames 是 std::vector<Frame>，构造时会 resize；atomic 成员删掉了隐式移动
    // 构造，必须显式补一个，否则 resize 编不过。
    Frame(Frame&& o) noexcept
        : data(std::move(o.data)), pid(o.pid), pin_count(o.pin_count.load()),
          dirty(o.dirty), valid(o.valid) {}
    Frame& operator=(Frame&&) = delete;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
  };

  Impl(const BufferPoolOptions& o, DiskManager* d, WalWriter* w, RecoveryManager* r)
      : opt(o), disk(d), wal(w), recovery(r) {
    LTM_UNUSED(wal);
    LTM_UNUSED(recovery);
  }

  BufferPoolOptions opt;
  DiskManager* disk;
  WalWriter* wal;
  RecoveryManager* recovery;

  std::vector<Frame> frames;
  std::unordered_map<PageId, uint32_t> page_to_frame;
  std::vector<uint32_t> free_frames;
  uint32_t clock_hand = 0;
  PoolStats stats;

  // ---------------------------------------------------------------------------
  // 帧表临界区（缺陷 #46 —— C0 的共享闩保护不到缺页/淘汰路径）
  //
  // 【它保护什么】`page_to_frame` / `free_frames` / `clock_hand` 三样可变结构，
  // 以及 Frame 的三个**非原子**字段（pid / valid / dirty）。`pin_count` 本身是
  // 原子的，但"查表 + 加 pin"这一对动作必须与"淘汰判定"互斥 —— 否则会出现
  // 教科书式的竞态：线程 A 在表里查到了页 P 的帧，还没来得及 `++pin_count`，
  // 线程 B 就把这个（此刻 pin_count==0 的）帧当成受害者回收、并把它交给了页 Q。
  // A 随后在属于 Q 的帧上加 pin，于是"取回的页自报 page_id=Q、请求的却是 P"。
  // 这正是 tools/c0_boundary_probe 实测到的症状，不是推理。
  //
  // 【锁粒度：命中共享、未命中/淘汰独占】命中路径只读表 + 原子加 pin，取共享；
  // 未命中/淘汰/安装要改表、改空闲链、改时钟指针，取独占并**二次查找**（别的
  // 线程可能在我们拿锁之前已经把页读进来了）。独占段跨越 `ReadPage` 与（可能
  // 发生的）脏页落盘，所以那段时间别的读线程会等 —— 这是刻意的取舍：正确性
  // 优先，而且它让"淘汰判定看到的 pin_count"是稳定的（这正是上面那条竞态的
  // 反面：判定与加 pin 必须在同一个临界区里）。
  //
  // 【锁序（C1 的全局序，写下来免得后来人摆错）】
  //     页闩(自根向下) → frame_table → 该页的 load-lock
  // 缓冲池自己**不**获取页闩（页闩由索引层在 crabbing 时持有），所以这里不会
  // 与索引层的加锁序成环。索引层持着父页闩调 FetchPage 时，顺序天然是
  // "页闩 → frame_table"，与上面的全局序一致。
  //
  // 【它保护不到什么（诚实记录）】`HeatSource`（热度表）与索引层自己的派生
  // 状态不在本锁范围内：前者由 HybridIndex 注入、自身要线程安全；后者由
  // C1 的索引层耦合负责。当前验收工具（c0_boundary_probe / soak_mixed）都用
  // heat=nullptr，故不触及前者。
  // ---------------------------------------------------------------------------
  mutable std::shared_mutex table_mutex;

  PageId free_list_head = kInvalidPageId;
  uint64_t free_pages = 0;
  PageId next_unallocated = kFirstDataPageId;
  bool meta_loaded = false;
  bool recovered_from_disk = false;

  // ---- M4-2 写事务与写屏障 ----
  struct WriteOp {
    TxnId txn = kInvalidTxnId;
    int depth = 0;
    // 本事务内每个"被钉住过"的页在**首次钉住时**的整页快照 —— UNDO 的 before-image
    std::unordered_map<PageId, std::vector<char>> before;
    // 【M6】分配器的运行时状态也要能回退。NewPage/FreePage 在写事务里改这三样，
    // 而它们住在 Impl 上、不在任何页里，所以页的 before-image 覆盖不到它们。
    // 不回退的后果不是"少还一个页号"这么轻：next_unallocated 永久前移会让
    // 恢复出来的空闲链与实际页布局对不上（不变量 I7），而这些页号再也不会
    // 被任何结构引用 —— 同时 free_list_head 还指着一个已经被解引用的链。
    bool alloc_snapshot_valid = false;
    PageId alloc_free_list_head = kInvalidPageId;
    uint64_t alloc_free_pages = 0;
    PageId alloc_next_unallocated = kFirstDataPageId;
    // 本事务里 NewPage 出来的页号。它们**没有**可用的 before-image：
    // 快照点被刻意放在 Init() 之前（否则 before == after，新页会带着
    // kInvalidLsn 落盘，见 NewPage 里的注释），所以快照里是"分配前那一帧的
    // 遗留内容"——把它还原回去等于把一页垃圾当成回滚目标。
    // 对这类页正确的回滚是"它压根不存在"：丢弃帧、并且（上面）把分配器退回去。
    std::vector<PageId> allocated_in_txn;
    // 【M6】索引层注册进来的"派生状态回滚"。缓冲池只负责在 Abort 时按后进先出执行，
    // 不关心里面是什么 —— 那部分状态归各棵树，见 buffer_pool.h 的协议说明。
    std::vector<std::function<void()>> undo_actions;
    // 每开一个**最外层**事务加一。参与方用它做去重：同一代里注册过就不再注册。
    uint64_t generation = 0;
  };
  WriteOp write_op;
  // 发现 I6 被违反后置位：拒绝再往盘上写任何页，免得把"改了一半"的内存状态落盘
  bool poisoned = false;

  // C1：用 thread_local 的写事务深度，而不是共享的 write_op.depth ——
  // 后者会被并发读者读到，导致读者误入写者私有的 before-image 表（见文件顶部的说明）。
  bool InWriteOp() const { return g_write_op_depth > 0; }
  // 钉住一页时调用：在写事务内快照它的事务开始态
  void NotePinned(PageId pid, uint32_t frame_idx);
  // 把 LSN 写进页头，并把该帧标脏。
  //
  // 【为什么按 frame_idx 而不是 pid 取帧】原先它自己去 `page_to_frame.find(pid)`，
  // 而唯一调用方 CollectDelta 是从 Unpin 来的 —— Unpin 现在持有帧表锁（缺陷 #46），
  // 再来一次 `page_to_frame.find` 就是对同一把锁的**递归**获取（shared_mutex 不可
  // 重入，直接死锁）。改成按帧号取，顺带把"帧已经不是这个页了"的检查做得更直接。
  void StampPageLsn(uint32_t frame_idx, PageId pid, Lsn lsn);
  // Unpin 时采集一页的 delta 并立刻追加进 WAL
  Status CollectDelta(PageId pid, uint32_t frame_idx);
  // 【M6】Abort 的运行期回滚：把 before-image 表里的页与分配器状态退回事务开始态。
  // 详情见实现处的长注释。
  Status RollbackWriteOp(BufferPool* self);
  // 就地把一页的帧内容还原成 before-image（不改 pin 计数）
  void RestoreFrame(uint32_t frame_idx, PageId pid, const std::vector<char>& before);
  // 三个刷盘点的统一出口：写前日志规则 + I6 检查 + poisoned 拦截 + 重算校验和。
  //
  // 【M4-3 起没有"豁免"参数】此前它有一个 enforce_i6=false 的口子，专门给建库引导
  // （页 0 刚被 Init、日志里一条记录都没有）用。那个口子本身是安全的，但它的判据
  // "数据文件是本次新建的"与"这一页确实没有 REDO 覆盖"并不等价：数据文件被删掉、
  // 日志却留着的那种状态也会落进豁免，于是 I6 会在最需要它的时候失明。
  // 现在唯一的豁免是**日志里一条记录都没有**（见下面的 last_lsn 判断）——
  // 那正好是"建库第一步"的精确判据，而且它不可能被"顺手同步"伪造。
  Status FlushFrame(uint32_t frame_idx, const char* who);

  // ---- 帧分配与淘汰 ----
  // 返回一个可直接占用的帧下标；必要时先刷盘并回收受害者。
  StatusOr<uint32_t> AcquireFrame(const EvictionPolicy* policy, HeatSource* heat,
                                  PageId target_pid);
  // 把一个候选帧彻底回收：脏页先落盘、从 page_to_frame 摘除、标记为空闲
  Status PrepareVictim(uint32_t frame_idx, HeatSource* heat);

  // ---- 元数据页 ----
  Status LoadMeta(BufferPool* self);
  Status PersistMeta(BufferPool* self);
};

// ===========================================================================
// M4-2 写事务与写屏障（不变量 I6）
//
// 【为什么 I6 需要一个"采集器"，光在 Commit 里写日志不够】
// 缓冲池允许 steal：一个逻辑事务在 Commit **之前**就可能把它改过的页交给
// PrepareVictim 写盘（实测 B+ validate 一轮驱逐 1617 次、R 树 12.7 万次）。
// 若把 delta 攒到 Commit 时才追加，那些页就是"带着尚未落盘的 REDO 落盘"的 ——
// I6 会在每一轮被违反上千次，而且违反得悄无声息。
//
// 所以采集点是 Unpin（一页不再被钉住的那一刻）。这不是将就，语义正好：
//   * "将写盘"的集合 == Unpin 时 dirty 位为真的那些页（PrepareVictim 只看 fr.dirty）；
//   * 不脏的页不会落盘，对 I6 无害，不需要 REDO；
//   * 一页只可能在钉住期间被改动，所以在**钉住时**快照 before-image 是构造上正确的
//     （若改在 MarkDirty 里快照，"先改后标"的写法会把已改内容误当成事务开始态）。
// ===========================================================================
void BufferPool::Impl::NotePinned(PageId pid, uint32_t frame_idx) {
  if (!InWriteOp()) {
    return;
  }
  if (write_op.before.find(pid) != write_op.before.end()) {
    return;
  }
  const Frame& fr = frames[frame_idx];
  write_op.before.emplace(pid, fr.data);
}

void BufferPool::Impl::StampPageLsn(uint32_t frame_idx, PageId pid, Lsn lsn) {
  Frame& fr = frames[frame_idx];
  if (fr.pid != pid) {
    // 帧已经不（再）是这个页了 —— 计数上报，绝不静默。静默的后果是"页在盘上带着
    // 旧 LSN"，屏障会因此认为它已经被覆盖。
    ++stats.lsn_stamps_missed;
    return;
  }
  Page page(fr.data.data());
  page.mutable_header()->page_lsn = lsn;
  // 【必须标脏】LSN 若落在一个"干净"帧上会被静默丢掉：PrepareVictim 只写脏帧，
  // 而 AcquireFrame 会把这个干净帧直接发给别的页 —— 这个 LSN 再也回不来了。
  fr.dirty = true;
  ++stats.lsn_stamps;
}

Status BufferPool::Impl::CollectDelta(PageId pid, uint32_t frame_idx) {
  auto it = write_op.before.find(pid);
  if (it == write_op.before.end()) {
    // 事务内被改过，却从未在事务内被钉住过 —— 按设计不可能（改动必须经守卫）。
    // 真发生就是采集漏洞，计数上报而不是静默跳过。
    ++stats.deltas_missing_before;
    return Status::Ok();
  }
  Frame& fr = frames[frame_idx];
  const std::vector<char>& before = it->second;
  if (before.size() != fr.data.size()) {
    // 尺寸都对不上只可能是"帧被复用给了别的页"，属于采集器自身的 bug。
    // 记成 missing_before 上报而不是硬算出一个无意义的区间。
    ++stats.deltas_missing_before;
    return Status::Ok();
  }
  // D3 的 memcmp 是下面这次扫描的退化情形（区间为空），所以合并成一次扫描：
  // 从两端各自推进，剩下的就是**真正变化的字节区间** [head, tail)。
  // 顺带量出"只记变化区间"到底能省多少 —— 方案里那个 20–50x 是估计值，
  // 而这个值是实测值（见 PoolStats::delta_bytes_full/changed 的注释）。
  const size_t n = before.size();
  size_t head = 0;
  while (head < n && before[head] == fr.data[head]) {
    ++head;
  }
  size_t tail = n;
  while (tail > head && before[tail - 1] == fr.data[tail - 1]) {
    --tail;
  }
  stats.delta_bytes_full += n;
  stats.delta_bytes_changed += (tail - head);
  if (head == tail) {
    // 标了脏但一个字节都没变（MarkDirty 不等于"逻辑上被改过"：Compact() 无条件
    // 调用、SplitInto 两侧都标脏）。省下的是一条 8KB 的记录，很划算。
    ++stats.deltas_skipped_identical;
    return Status::Ok();
  }
  PageImageDelta d;
  d.page_id = pid;
  d.offset = 0;
  d.before = before;
  d.after = fr.data;
  auto lsn = wal->AppendPageDelta(write_op.txn, d);
  if (!lsn.ok()) {
    return lsn.status();
  }
  ++stats.wal_deltas;
  StampPageLsn(frame_idx, pid, lsn.value());
  return Status::Ok();
}

// ===========================================================================
// M6 · Abort 的运行期回滚（C1 的前置条件）
//
// 【这条缺陷长什么样】status.md「下一步」第 4 条：`ScopedWriteOp::Abort` 过去只写
// 一条 kAbort 记录，**不回滚内存**。崩溃恢复是对的（日志里有 kAbort，Undo 阶段会
// 把它回滚掉），但运行期留下的是"日志说没有、内存里却有"的状态。
//
// 它比"运行期不一致"更重：**这棵脏树随后会被干净关闭刷到盘上**。
// 验证见 tests/concurrency/test_write_abort.cc 的 AbortedStateDoesNotSurviveReopen ——
// abort 之后 FlushAll + 重开，幻影记录是真的躺在页文件里的（干净关闭不触发恢复，
// 所以没有任何东西会去纠正它）。修复前的红输出留档在 report/abort_rollback/。
//
// 【为什么它必须是 C1 的前置条件】C0 下写被独占闩串行化，跨树失败还咬不到人；
// C1 一放开写并行，HybridIndex::Put 的"R 树写成功、B+ 树失败"就会变成常态路径。
//
// ---------------------------------------------------------------------------
// 【为什么回滚 before-image 是 I6 安全的 —— 这是本节最容易想反的一处】
// before-image 是**整页**快照（含 32B 页头），所以还原时 `page_lsn` 也一并回到
// 事务开始前的值。于是"这一页将带着一个更小的 page_lsn 落盘"，而 FlushFrame 的
// 屏障判据是 `page_lsn > flushed_lsn ⇒ 先 Sync()`（见下方 FlushFrame）。page_lsn
// 只会变小 ⇒ 这个条件只会更容易满足 ⇒ 回滚**不可能**制造出新的 I6 违反。
//
// 反过来，如果只还原载荷、把 page_lsn 留着（"反正马上要重算校验和"），那就正好
// 踩中陷阱：页的内容回到了事务前，页头却声称"我由一条尚未持久化的 REDO 覆盖"，
// 于是这一页在被人问起时永远说不清自己是谁。
//
// 【为什么不需要写 CLR】运行期 Abort 不产生"已经重放到盘上的更新"，因为它没有
// 崩溃 —— 所有改动都还在内存里。WAL 里那个事务的 REDO 记录配上 kAbort，
// 恢复时走的就是 Undo 分支，与内存中的还原结果一致。CLR 是给**恢复期**的 Undo
// 用的（那时更新确实已经重放到页上了），不该在运行期这套里再实现一遍。
// ===========================================================================
void BufferPool::Impl::RestoreFrame(uint32_t frame_idx, PageId pid, const std::vector<char>& before) {
  Frame& fr = frames[frame_idx];
  LTM_UNUSED(pid);
  if (fr.data.size() != before.size()) {
    // 尺寸对不上只可能是"这个帧已经被复用给别的页了"。事务内被改过的页在
    // Unpin 时会先落 delta，所以此刻帧被复用意味着调用顺序出了问题。
    // 不猜、不截断 —— 记进 missing_before（与 CollectDelta 的口径一致）。
    ++stats.deltas_missing_before;
    return;
  }
  if (std::memcmp(fr.data.data(), before.data(), before.size()) == 0) {
    // 一个字节都没变。这条路径很常见，不是异常：ScopedWriteOp 是 RAII，
    // "记录没找到"这类什么都没改的正常分支也会走一次 Abort。
    // 不动它 —— 平白标脏会让下一次淘汰多刷一个完全相同的页。
    // 判据与 CollectDelta 跳过 identical delta 是同一条。
    return;
  }
  std::memcpy(fr.data.data(), before.data(), before.size());
  // 必须标脏：不标的话这一次还原只活在内存里，而它恰恰是"必须落盘"的 ——
  // 盘上那份是事务的 after-image。标脏走的是与普通写同样的 PrepareVictim → FlushFrame。
  fr.dirty = true;
  ++stats.abort_pages_rolled_back;
}

Status BufferPool::Impl::RollbackWriteOp(BufferPool* self) {
  WriteOp& wo = write_op;
  if (wo.before.empty() && !wo.alloc_snapshot_valid && wo.undo_actions.empty()) {
    return Status::Ok();
  }

  // 【第零趟：本事务里新建的页要"收回"，不是"还原"】
  // 这些页没有可用的 before-image —— 快照点被刻意放在 Init() 之前（否则
  // before == after，新页会带着 kInvalidLsn 落盘，见 NewPage 的注释），
  // 所以快照里是"分配前那一帧的遗留内容"。把它还原回去 = 把一页垃圾当回滚目标，
  // 而且它的 page_lsn 往往是 kInvalidLsn，下一步 FlushAll 恰好会撞上 I6。
  // （这条不是推理出来的：改成"还原"之后实测就在 FlushAll 把池毒化了。）
  //
  // 正确的回滚是"这页从来没有存在过"：分配器在下面退回去，这些页号重新变成
  // 未分配。此时要**丢掉脏位**而不是留着 —— 留着的话淘汰/FlushAll 会把它写下去，
  // 而那正是上面那条 I6 违反。盘上那份本来就是"未分配页"该有的内容，页号再被
  // 分配出去时 NewPage 会 Init() 覆盖它。
  std::unordered_set<PageId> fresh(wo.allocated_in_txn.begin(), wo.allocated_in_txn.end());
  {
    // C1：读者正并发地访问帧表（他们走加锁路径），这里也必须进 table_mutex。
    std::unique_lock<std::shared_mutex> lk(table_mutex);
    for (PageId pid : fresh) {
      auto it = page_to_frame.find(pid);
      if (it == page_to_frame.end()) {
        continue;
      }
      Frame& fr = frames[it->second];
      if (fr.pid == pid) {
        fr.dirty = false;
        ++stats.abort_pages_discarded;
      }
    }
  }

  // 【第一趟与第二趟，顺序不能反】第一趟只碰**仍在池里**的页（纯 memcpy，不会触发
  // 淘汰）；第二趟才处理事务中被换出去的页（要 FetchPage，可能触发淘汰 → 可能刷盘）。
  //
  // 反过来的话，第二趟的淘汰有可能在"本事务还有脏帧没回滚"的时候把它们写下去 ——
  // 那些页带着事务的 after-image 和一个可能尚未持久化的 page_lsn。虽然 FlushFrame
  // 会在 page_lsn > flushed_lsn 时补一次 Sync 兜住 I6，但把"先污染再靠屏障补救"
  // 当默认路径没有道理：第一趟本来就是零成本的。
  std::vector<std::pair<PageId, const std::vector<char>*>> evicted;
  evicted.reserve(wo.before.size());
  {
    std::unique_lock<std::shared_mutex> lk(table_mutex);
    for (auto& kv : wo.before) {
      if (fresh.count(kv.first) != 0) {
        continue;  // 已由第零趟处理
      }
      auto it = page_to_frame.find(kv.first);
      if (it == page_to_frame.end()) {
        evicted.emplace_back(kv.first, &kv.second);
        continue;
      }
      RestoreFrame(it->second, kv.first, kv.second);
    }
  }

  for (auto& e : evicted) {
    // FetchPage 会把页读进来并钉住；此刻 write_op.depth 已经是 0，所以
    // NotePinned 不会重新快照（它只看 InWriteOp()），下面覆盖帧之后由守卫析构
    // 走 Unpin(pid, /*dirty=*/false) —— dirty 位已由 RestoreFrame 置好，
    // 因此也不会误进"事务外标脏"分支去涨 deltas_outside_txn。
    // 【注意】FetchPage 自带 table_mutex，所以这里不能持锁调用它。
    auto g = self->FetchPage(e.first);
    if (!g.ok()) {
      return g.status();
    }
    std::unique_lock<std::shared_mutex> lk(table_mutex);
    auto it = page_to_frame.find(e.first);
    if (it == page_to_frame.end()) {
      return Status::Error(StatusCode::kCorruption,
                           "page " + std::to_string(e.first) + " vanished mid-abort-rollback");
    }
    RestoreFrame(it->second, e.first, *e.second);
    ++stats.abort_pages_reloaded;
  }

  // ---- 分配器运行时状态 + 页 0 的内存镜像（都在帧表临界区里）----
  {
    std::unique_lock<std::shared_mutex> lk(table_mutex);
    if (wo.alloc_snapshot_valid) {
      free_list_head = wo.alloc_free_list_head;
      free_pages = wo.alloc_free_pages;
      next_unallocated = wo.alloc_next_unallocated;
      wo.alloc_snapshot_valid = false;
      ++stats.abort_alloc_restores;
    }
    // 页 0 的权威内容有两份：帧里的字节，和 self->meta_copy_ 这个被上层读的镜像。
    // 上面那一趟只回滚了帧。镜像不同步的话，EnsureMetaLoaded 之后读 meta() 看到的
    // 仍是事务后的槽位/游标，于是"内存回滚了"只对了一半 —— 而另一半恰恰是
    // 上层建树/查树时会读的那一份。
    if (self->meta_copy_ != nullptr) {
      auto pit = page_to_frame.find(kMetaPageId);
      if (pit != page_to_frame.end()) {
        Frame& mf = frames[pit->second];
        std::memcpy(self->meta_copy_.get(), Page(mf.data.data()).payload(), sizeof(MetaPage));
      }
      // 物理字段以 Impl 的运行时状态为准（它才是权威；LoadMeta 末尾也是这个方向）
      self->meta_copy_->free_list_head = free_list_head;
      self->meta_copy_->next_unallocated = next_unallocated;
      self->meta_copy_->free_pages = free_pages;
    }
  }

  // ---- 索引层注册的派生状态 ----
  // 放在最后：参与方读的是"事务开始态"，而它捕获的值与上面的页回滚无关，
  // 顺序只影响可读性。后进先出执行（与构造顺序相反），是回滚的通常约定。
  for (auto it = wo.undo_actions.rbegin(); it != wo.undo_actions.rend(); ++it) {
    (*it)();
    ++stats.abort_participant_undos;
  }
  wo.undo_actions.clear();

  return Status::Ok();
}

Status BufferPool::Impl::FlushFrame(uint32_t frame_idx, const char* who) {
  Frame& fr = frames[frame_idx];
  if (poisoned) {
    return Status::Error(StatusCode::kCorruption,
                         "the buffer pool is poisoned after an I6 violation; refusing to write page " +
                             std::to_string(fr.pid));
  }
  if (disk == nullptr) {
    return Status::Error(StatusCode::kIoError, "BufferPool has no DiskManager bound");
  }
  if (wal != nullptr) {
    Page page(fr.data.data());
    const Lsn page_lsn = page.header().page_lsn;
    const Lsn flushed = wal->flushed_lsn();
    if (page_lsn == kInvalidLsn) {
      // 一页被写过，却没有任何 REDO 覆盖它 —— 这才是真正的 I6 缺口。
      // 【为什么这条检查不会被"顺手同步"弄瞎】Sync() 只能把 flushed_lsn 往前推，
      // 它**改变不了 page_lsn 是不是 0**。所以这条判据是可证伪的。
      // 唯一正当的例外是"日志里一条记录都还没有"，即建库的第一步（页 0 被 Init 后
      // 立刻落盘）。这个判据是精确的：它不关心数据文件是不是本次新建的，
      // 只关心"有没有任何一条 REDO 可能存在"。M4-3 之前这里靠调用方传
      // enforce_i6=false 放行，而那个口子的判据（数据文件是新建的）并不等价 ——
      // 数据文件被删掉、日志却留着时 I6 会失明。
      if (wal->last_lsn() != kInvalidLsn) {
        if (opt.strict_i6_barrier) {
          ++stats.i6_violations;
          poisoned = true;
          LOG_ERROR("I6 violated: ", who, " is about to write page ", fr.pid,
                    " whose page_lsn is kInvalidLsn, but the WAL already holds records up to ",
                    wal->last_lsn(),
                    ". The page was modified without any REDO covering it. Refusing to write; "
                    "the pool is now poisoned. Do NOT 'fix' this by syncing: Sync() cannot give "
                    "the page a stamp, it only moves flushed_lsn forward.");
          return Status::Error(
              StatusCode::kCorruption,
              "I6 violated: page " + std::to_string(fr.pid) +
                  " is being written with no REDO record covering it (page_lsn=kInvalidLsn)");
        }
        ++stats.i6_unstamped_writes;
      }
    } else if (page_lsn > flushed) {
      // 写前日志规则本身（方案 §4.4 第 422 行："脏页驱逐前必须完成 WalWriter::sync()
      // -> DiskManager::write_page() -> 清脏"）。这不是"补一次保险"，而是 I6 的
      // 实现方式：事务中途把脏页淘汰出去时，它的 REDO 很可能还在缓冲里。
      ++stats.wal_sync_on_flush;
      LTM_RETURN_IF_ERROR(wal->Sync());
    }
  }
  Page page(fr.data.data());
  page.UpdateChecksum();
  LTM_RETURN_IF_ERROR(disk->WritePage(fr.pid, fr.data.data()));
  fr.dirty = false;
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 元数据页（页 0）的加载与持久化
//
// 设计要点：页 0 的布局由 storage/meta_page.h 统一定义，缓冲池是它**唯一**的
// 读写者。上层要改自己那份状态（根页号/高度/维度）必须走 WriteTreeSlot()，
// 不能自己 reinterpret 页 0。
//
// Bootstrap is limited to an empty file without WAL history, or a verified CLR
// image from undo of initial tree creation. Existing pages must pass checksum,
// physical identity and format checks before any mutation. Compute the checksum
// before flushing the initialized page, rather than waiting until unpin.
// ---------------------------------------------------------------------------
Status BufferPool::Impl::LoadMeta(BufferPool* self) {
  if (meta_loaded) {
    return Status::Ok();
  }
  // Classify before installing or modifying a frame. ReadPage pads short reads,
  // so only a physically empty file with no WAL history is a new database.
  auto file_size = disk->PageFileSize();
  if (!file_size.ok()) return file_size.status();
  if (file_size.value() > 0 && file_size.value() < kPageSize) {
    return Status::Error(StatusCode::kCorruption, "metadata page is truncated");
  }
  std::vector<char> raw(disk->page_size(), 0);
  LTM_RETURN_IF_ERROR(disk->ReadPage(kMetaPageId, raw.data()));
  const MetaPage* disk_meta = MetaOf(raw.data() + kPageHeaderSize);
  bool empty_history = (wal == nullptr ? disk->WalSize() == 0
                        : wal->last_lsn() == kInvalidLsn && disk->WalSize() <= kWalDataStart);
  if (empty_history && wal != nullptr) {
    WalLogHeader log_header{};
    LTM_RETURN_IF_ERROR(ReadWalLogHeader(disk, &log_header));
    empty_history = log_header.last_checkpoint_lsn == kInvalidLsn;
  }
  bool fresh = file_size.value() == 0 && empty_history &&
      std::all_of(raw.begin(), raw.end(), [](char byte) { return byte == 0; });
  if (!fresh) {
    Page page(raw.data());
    LTM_RETURN_IF_ERROR(page.Validate());
    if (page.header().page_id != kMetaPageId) {
      return Status::Error(StatusCode::kCorruption, "metadata page has the wrong physical page id");
    }
    // Undo of an interrupted first tree creation leaves a checksummed blank
    // shell stamped with its CLR LSN. Require the exact WAL image, not merely
    // zero magic or a nonzero LSN, before allowing bootstrap from that shell.
    const bool blank_payload = std::all_of(raw.begin() + kPageHeaderSize,
        raw.end() - kPageTrailerSize, [](char byte) { return byte == 0; });
    if (blank_payload && recovery != nullptr && page.header().page_lsn != kInvalidLsn) {
      LTM_RETURN_IF_ERROR(ForEachWalRecord(disk, [&](const WalRecordHeader& record, const char* data) {
        if (record.type != WalRecordType::kClrRedo || record.page_id != kMetaPageId ||
            record.page_off != 0 || record.data_len != kPageSize || record.lsn != page.header().page_lsn) {
          return true;
        }
        std::vector<char> image(data, data + record.data_len);
        Page restored(image.data());
        restored.mutable_header()->page_lsn = record.lsn;
        restored.UpdateChecksum();
        fresh = image == raw;
        return !fresh;
      }));
    }
    if (!fresh && (page.header().type != PageType::kMeta || page.header().level != 1 ||
                   page.header().tree_id != 0 || disk_meta->magic != kMetaPageMagic ||
                   disk_meta->format_version != kMetaPageFormatVersion)) {
      return Status::Error(StatusCode::kCorruption, "metadata page identity or format mismatch");
    }
  }
  *self->meta_copy_ = *disk_meta;
  if (fresh) {
    std::memset(self->meta_copy_.get(), 0, sizeof(MetaPage));
    self->meta_copy_->magic = kMetaPageMagic;
    self->meta_copy_->format_version = kMetaPageFormatVersion;
    self->meta_copy_->free_list_head = kInvalidPageId;
    self->meta_copy_->next_unallocated = kFirstDataPageId;
    self->meta_copy_->free_pages = 0;
    self->meta_copy_->physical_pages = 1;
  } else if (self->meta_copy_->format_version != kMetaPageFormatVersion) {
    return Status::Error(StatusCode::kCorruption,
                         "metadata page format version mismatch: " +
                             std::to_string(self->meta_copy_->format_version));
  }
  recovered_from_disk = !fresh;

  // 2) 把内存镜像写进一个真实的页守卫（页 0 常驻，第一次会分配帧）
  auto g = self->FetchChecked(kMetaPageId, /*validate_checksum=*/!fresh);
  if (!g.ok()) {
    return g.status();
  }
  PageGuard guard = std::move(g.value());
  if (!fresh) {
    LTM_RETURN_IF_ERROR(guard.page().Validate());
  }
  if (fresh) {
    // 只有"全新建库"才重建页头。已存在的页必须保留它盘上的页头 —— 尤其是
    // page_lsn：每次打开都对页 0 调 Init()（整页 memset）会让它的 LSN 永远停在
    // kInvalidLsn，而页 0 恰恰是最需要被写屏障覆盖的页（它记录根页号/高度，
    // 且每个写操作末尾都会改它）。保留它，M4-2 的断言才不会对最危险的页恰好失明。
    //
    // 【M4-3 补充】"新建库"这条分支也要保留 page_lsn，不只是已存在的页：
    // 崩溃恢复可能把一个**全零**的页 0 留在盘上，同时给它盖上一条 CLR 的 LSN
    // （未提交的建树事务被回滚，页 0 回到建库前的样子）。此时 magic 当然不存在，
    // 走的就是这条 fresh 分支；如果 Init() 把 LSN 抹成 0，那么紧接着的落盘就会
    // 撞上 I6 检查 —— 而那个 LSN 恰恰是恢复刚写下的、唯一能解释这一页的记录。
    Page probe = guard.page();
    const Lsn keep_lsn = probe.header().page_lsn;
    guard.MarkDirty();  // 先标脏再改：下面的 Init() 会整页 memset
    Page page = guard.page();
    page.Init(kMetaPageId, PageType::kMeta, 0, 1);
    page.mutable_header()->page_lsn = keep_lsn;
  }
  // 类型与层级每次都校正（防御陈旧页头），但**不碰 page_lsn**
  PageHeader* mh = guard.mutable_header();
  mh->type = PageType::kMeta;
  mh->level = 1;
  std::memcpy(guard.mutable_payload(), self->meta_copy_.get(), sizeof(MetaPage));
  // 重算校验和是写操作，必须走 mutable_page()；它顺带标脏，所以这里不再重复 MarkDirty
  guard.mutable_page().UpdateChecksum();
  // 【M4-3 起这里不再放行】此前调用方用 enforce_i6=!fresh 把"建库引导"这件事告诉
  // FlushFrame，于是"数据文件是新建的"变成了一个可以绕过 I6 的口子。现在唯一的豁免
  // 判据在 FlushFrame 内部、且是精确的那一条：日志里一条记录都没有。
  auto fit = page_to_frame.find(kMetaPageId);
  if (fit == page_to_frame.end()) {
    return Status::Error(StatusCode::kCorruption,
                         "the metadata page disappeared from the pool during LoadMeta");
  }
  const Status flushed = FlushFrame(fit->second, "LoadMeta bootstrap");
  guard.Release();
  LTM_RETURN_IF_ERROR(flushed);
  // 内存镜像里的物理页字段要与运行时状态一致
  self->meta_copy_->free_list_head = free_list_head;
  self->meta_copy_->next_unallocated = next_unallocated;
  self->meta_copy_->free_pages = free_pages;
  meta_loaded = true;
  LOG_INFO("BufferPool: metadata page ", (fresh ? "created and persisted" : "loaded from disk"), " (free pages ",
           self->meta_copy_->free_pages, ", allocation cursor ", self->meta_copy_->next_unallocated, ")");
  return Status::Ok();
}

// 把内存镜像写回页 0。**不再立刻落盘** —— 原因见下面的注释。
//
// 页 0 的持久性需求是"提交后可见"，而不是"事务中途可见"。原实现在此处无条件
// FlushPage(0)，而 IndexTree::Insert / IndexTree::Erase 每次成功操作的末尾都会调
// PersistMeta。实测（B+ validate，20000 次操作）：writes 21164 页而驱逐只有 902 次，
// 差额的约 20000 次全是页 0 的 4KB 随机写。
//
// 去掉它不会丢数据：干净关闭走 ~BufferPool -> FlushAll()（写脏帧 + SyncPageFile）；
// 崩溃场景本来就是 M4-3 恢复要覆盖的范围。
// 更重要的是：页 0 从此刻起与其他页走**同一条落盘路径**，M4-2 的写屏障只需守一个出口。
Status BufferPool::Impl::PersistMeta(BufferPool* self) {
  auto g = self->FetchPage(kMetaPageId);
  if (!g.ok()) {
    return g.status();
  }
  PageGuard guard = std::move(g.value());
  // 物理页字段从 Impl 的运行时状态同步进镜像
  self->meta_copy_->free_list_head = free_list_head;
  self->meta_copy_->next_unallocated = next_unallocated;
  self->meta_copy_->free_pages = free_pages;
  std::memcpy(guard.mutable_payload(), self->meta_copy_.get(), sizeof(MetaPage));
  // 重算校验和是写操作，必须走 mutable_page()；它顺带标脏，所以这里不再重复 MarkDirty
  guard.mutable_page().UpdateChecksum();
  guard.Release();
  return Status::Ok();
}

Status BufferPool::Impl::PrepareVictim(uint32_t frame_idx, HeatSource* heat) {
  Frame& v = frames[frame_idx];
  if (v.valid) {
    if (v.dirty) {
      // 写盘统一走 FlushFrame：它是 I6 屏障的唯一出口（重算校验和也在里面，
      // 漏掉的话盘上会留下"内容与 trailer 里的 CRC 不一致"的页，
      // 下次读回来时 FetchPage 直接报 kChecksumMismatch）。
      LTM_RETURN_IF_ERROR(FlushFrame(frame_idx, "evicting a dirty page"));
      ++stats.dirty_evictions;
    }
    ++stats.evictions;
    page_to_frame.erase(v.pid);
    if (heat != nullptr) {
      heat->MarkOutOfPool(v.pid);
    }
  }
  v.valid = false;
  v.pid = kInvalidPageId;
  v.dirty = false;
  return Status::Ok();
}

StatusOr<uint32_t> BufferPool::Impl::AcquireFrame(const EvictionPolicy* policy,
                                                  HeatSource* heat, PageId target_pid) {
  if (!free_frames.empty()) {
    const uint32_t idx = free_frames.back();
    free_frames.pop_back();
    return idx;
  }
  // 需要淘汰。
  //
  // 【必须同时满足的两个条件】候选帧不仅要 pin_count == 0，还必须是
  // "当前没有被任何页号占用"（valid == false 或已从 page_to_frame 摘除）。
  // 只看 pin_count 是不够的：一个页 Unpin 之后 pin_count 归零，但它仍然
  // 对应着 page_to_frame 里的一个页号；若把这个帧分配给新页，
  // 两个页号就会共享同一块帧内存、互相覆盖 —— 表现为"某个页的哨兵键/键序
  // 莫名其妙被破坏"，而且只在缓冲池需要淘汰时出现。
  // 可淘汰 == 没有被钉住、且不是元数据页（页 0 常驻）。
  //
  // 注意不能要求 !valid：一个页 Unpin 之后 pin_count 归零，但它仍是有效的驻留页，
  // 正是最该被淘汰的候选。若要求 !valid，就只有"从未用过"的帧可被选中，
  // 缓冲池一旦写满就再也发不出新帧（实测表现为 kOutOfSpace）。
  // 帧被选中后由 PrepareVictim() 负责刷盘并从 page_to_frame 摘除。
  auto cleanable = [this, target_pid](uint32_t i) {
    return frames[i].pin_count == 0 && frames[i].pid != kMetaPageId &&
           frames[i].pid != target_pid;
  };
  // 交给策略打分所需的帧信息（真实页头 + 真实热度）
  auto info_fn = [this, heat](uint32_t i, PageHeader* header, uint8_t* h, bool* pinned) {
    std::memcpy(header, frames[i].data.data(), sizeof(PageHeader));
    *h = (heat != nullptr) ? heat->Heat(frames[i].pid) : 0;
    *pinned = frames[i].pin_count > 0;
  };
  if (policy != nullptr) {
    ClockEvictor evictor;
    uint32_t victim = 0;
    if (!evictor.PickVictim(static_cast<uint32_t>(frames.size()), &clock_hand, cleanable, info_fn,
                            *policy, &victim)) {
      return Status::Error(StatusCode::kOutOfSpace,
                           "the buffer pool is full with no evictable frame (all pinned or still referenced)");
    }
    LTM_RETURN_IF_ERROR(PrepareVictim(victim, heat));
    return victim;
  }
  // 无策略：优先干净且未占用的帧，其次任意未占用的帧
  uint32_t victim = 0;
  bool found = false;
  for (size_t pass = 0; pass < 2 && !found; ++pass) {
    for (size_t k = 0; k < frames.size(); ++k) {
      const uint32_t idx =
          (clock_hand + static_cast<uint32_t>(k)) % static_cast<uint32_t>(frames.size());
      if (!cleanable(idx)) {
        continue;
      }
      if (pass == 0 && frames[idx].dirty) {
        continue;
      }
      victim = idx;
      found = true;
      clock_hand = (idx + 1) % static_cast<uint32_t>(frames.size());
      break;
    }
  }
  if (!found) {
    return Status::Error(StatusCode::kOutOfSpace,
                         "the buffer pool is full with no evictable frame (all pinned or still referenced)");
  }
  LTM_RETURN_IF_ERROR(PrepareVictim(victim, heat));
  return victim;
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
BufferPool::BufferPool(const BufferPoolOptions& options, DiskManager* disk, WalWriter* wal,
                       RecoveryManager* recovery, HeatSource* heat, const EvictionPolicy* policy)
    : impl_(new Impl(options, disk, wal, recovery)),
      meta_copy_(new MetaPage{}),  // 必须在这里就绪：LoadMeta 会写入它
      // C1 · 页闩表。顺序检查**常开**：Release/RelWithDebInfo 下只计数不 abort
      // （见 LatchManager::DefaultAbortOnViolation），Debug 下逆序直接 abort。
      // 判据 stats().order_violations 因此在本机构建的每一种配置下都可用。
      latch_mgr_(new LatchManager(/*enable_order_check=*/true)),
      heat_(heat),
      policy_(policy) {
  Impl& im = *impl_;
  if (im.opt.pool_frames == 0) {
    LOG_ERROR("BufferPool: pool_frames must not be 0");
    return;
  }
  const uint32_t page_size = (disk != nullptr) ? disk->page_size() : kPageSize;
  if (page_size != kPageSize) {
    LOG_ERROR("BufferPool: this implementation only supports page_size == ", kPageSize, ", got ", page_size);
    return;
  }
  im.frames.resize(im.opt.pool_frames);
  im.page_to_frame.reserve(im.opt.pool_frames * 2);
  im.free_frames.reserve(im.opt.pool_frames);
  for (uint32_t i = 0; i < im.opt.pool_frames; ++i) {
    im.frames[i].data.assign(page_size, 0);
    im.free_frames.push_back(im.opt.pool_frames - 1 - i);  // 使 0 号帧最先被分配
  }
  LOG_INFO("BufferPool ready: frames=", im.opt.pool_frames, " page_size=", page_size,
           " policy=", (policy_ != nullptr ? "injected" : "none"),
           " heat=", (heat_ != nullptr ? "injected" : "none"));
}

BufferPool::~BufferPool() {
  // 【顺序不可交换】先 FlushAll（把全部脏页落盘 + fsync 页文件），再置 clean_shutdown。
  // 反过来的话，"干净关闭"这个标记会在数据真正落盘之前就成立 —— 那正是恢复最不该
  // 相信的一种状态：下次打开会跳过 Redo，而数据其实还在内存里。
  const bool had_open_write_op = impl_->write_op.depth > 0;
  const Status s = FlushAll();
  if (!s.ok()) {
    LOG_ERROR("BufferPool: FlushAll failed during destruction: ", s.ToString());
  }
  // 只有在"这一轮确实没有半途而废的写事务"时才敢声明干净关闭。
  // 有未提交的写事务 → 与崩溃现场等价，必须让下次打开去做 Undo。
  if (impl_->recovery != nullptr && !had_open_write_op && !impl_->poisoned) {
    const Status m = impl_->recovery->MarkCleanShutdown();
    if (!m.ok()) {
      LOG_ERROR("BufferPool: cannot mark the log as cleanly shut down: ", m.ToString());
    }
  } else if (impl_->recovery != nullptr) {
    LOG_WARN("BufferPool: destroyed with ",
             (impl_->poisoned ? "a poisoned pool" : "a write transaction still open"),
             "; the log is deliberately left marked as unclean so that the next open runs recovery");
  }
}

char* BufferPool::FrameData(uint32_t frame_index) { return impl_->frames[frame_index].data.data(); }

const char* BufferPool::FrameData(uint32_t frame_index) const {
  return impl_->frames[frame_index].data.data();
}

// ---------------------------------------------------------------------------
// 页访问
// ---------------------------------------------------------------------------
StatusOr<PageGuard> BufferPool::FetchPage(PageId pid, LatchMode mode) { return FetchChecked(pid, true, mode); }

StatusOr<PageGuard> BufferPool::FetchUnchecked(PageId pid, LatchMode mode) {
  return FetchChecked(pid, false, mode);
}

StatusOr<PageGuard> BufferPool::FetchChecked(PageId pid, bool validate_checksum, LatchMode mode) {
  Impl& im = *impl_;
  if (im.disk == nullptr) {
    return Status::Error(StatusCode::kIoError, "BufferPool has no DiskManager bound");
  }
  if (pid >= im.disk->num_pages()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "FetchPage page number out of range: " + std::to_string(pid));
  }

  uint32_t frame_idx = 0;
  bool found = false;

  // ---- 快路径：共享锁下"查表 + 加 pin"（缺陷 #46）----
  // 这两步必须在同一个临界区里完成：查到帧之后、加 pin 之前若被别的线程淘汰，
  // 那个帧就会被交给另一个页。淘汰只在**独占**段发生，所以共享段与之互斥。
  {
    std::shared_lock<std::shared_mutex> lk(im.table_mutex);
    auto it = im.page_to_frame.find(pid);
    if (it != im.page_to_frame.end()) {
      frame_idx = it->second;
      Impl::Frame& fr = im.frames[frame_idx];
      ++fr.pin_count;
      ++im.stats.hits;
      im.NotePinned(pid, frame_idx);  // M4-2：写事务内首次钉住时快照事务开始态
      found = true;
    }
  }

  if (!found) {
    // ---- 慢路径：独占锁下二次查找 + 淘汰 + 读盘 + 安装 ----
    std::unique_lock<std::shared_mutex> lk(im.table_mutex);
    auto it = im.page_to_frame.find(pid);
    if (it != im.page_to_frame.end()) {
      // 二次查找：在我们拿到独占锁之前，别的线程可能已经把它读进来了。
      frame_idx = it->second;
      Impl::Frame& fr = im.frames[frame_idx];
      ++fr.pin_count;
      ++im.stats.hits;
      im.NotePinned(pid, frame_idx);
    } else {
      ++im.stats.misses;
      auto frame_or = im.AcquireFrame(policy_, heat_, pid);
      if (!frame_or.ok()) {
        return frame_or.status();
      }
      frame_idx = frame_or.value();
      Impl::Frame& fr = im.frames[frame_idx];
      // 拿到帧后必须确认它没有被别的页占用：AcquireFrame 可能返回一个"未钉住但
      // 仍属于某个页"的帧（该页尚未被淘汰），若直接覆盖，两个页号就会共享帧内存。
      if (fr.valid) {
        LTM_RETURN_IF_ERROR(im.PrepareVictim(frame_idx, heat_));
      }

      LTM_RETURN_IF_ERROR(im.disk->ReadPage(pid, fr.data.data()));
      fr.pid = pid;
      fr.dirty = false;
      fr.valid = true;
      fr.pin_count = 1;
      im.page_to_frame[pid] = frame_idx;
      im.NotePinned(pid, frame_idx);

      Page page(fr.data.data());
      if (validate_checksum) {
        const Status v = page.Validate();
        if (!v.ok()) {
          // 未初始化页（全零）与损坏页都在这里被拦下；正常路径只有 NewPage 之后才会读
          im.page_to_frame.erase(pid);
          fr.valid = false;
          fr.pin_count = 0;
          im.free_frames.push_back(frame_idx);
          return Status::Error(v.code(), "FetchPage validation failed pid=" + std::to_string(pid) +
                                             ": " + v.message());
        }
      }
    }
  }

  // ---- C1 · 页闩（必须在 table_mutex **之外**取）----
  //
  // 【锁序】table_mutex 到这里已经放掉了，所以"页闩 → frame_table"这条序不会
  // 出现两者同时持有，天然不成环（缓冲池也从不持着 table_mutex 去取页闩）。
  // level 从页头读：它在页的生命周期内不变（页只会被回收复用，不会换层），
  // 所以"先取页、后取闩"之间读到的是一个稳定值。闩拿到之后才真正读内容。
  PageGuard guard(this, frame_idx, pid);
  if (mode != LatchMode::kNone) {
    const uint8_t level = Page(im.frames[frame_idx].data.data()).header().level;
    auto latch_or = latch_mgr_->Acquire(pid, mode, level);
    if (!latch_or.ok()) {
      guard.Release();
      return latch_or.status();
    }
    guard.attach_latch(std::move(latch_or.value()));
  }
  if (heat_ != nullptr) {
    heat_->Touch(pid, /*is_prefetch=*/false);
  }
  return guard;
}

StatusOr<PageGuard> BufferPool::NewPage(PageType type, TreeId tree_id, uint8_t level,
                                        PageId* out_pid) {
  Impl& im = *impl_;
  if (im.disk == nullptr) {
    return Status::Error(StatusCode::kIoError, "BufferPool has no DiskManager bound");
  }
  LTM_RETURN_IF_ERROR(im.LoadMeta(this));

  PageId pid = kInvalidPageId;
  // 【C1：NewPage 也要在帧表临界区里】C0 时代写被独占闩串行化，下面这些对
  // free_list_head / free_pages / next_unallocated / page_to_frame / free_frames /
  // clock_hand 的直接访问不需要锁。C1 撤掉 C0 之后，**读者正并发地取页**（他们走
  // 的是 FetchChecked 的加锁路径），这些无锁访问会把 unordered_map 写坏 ——
  // 实测症状就是 soak_mixed 的读+写混合跑出 Segfault。
  // 所以下面每一段帧表/分配器访问都进 table_mutex；`FetchPage` 自带锁，故只能
  // 在**不持锁**的时候调用它。
  //
  // 【复用空闲页时必须复用同一个帧】下面 FetchPage(free_list_head) 已经把这个页读进
  // 池里了。如果这里再向 AcquireFrame 要一个别的帧，同一个页号就会同时占两块帧内存：
  // page_to_frame 指向新帧，旧帧变成"孤儿"——它既不在映射里、又仍带着旧内容。之后
  // 那个孤儿帧被淘汰时，PrepareVictim 会按 v.pid 把**陈旧内容**写到该页的磁盘位置，
  // 并顺手 erase 掉新帧的映射；于是该页 ReadPage 回来就是旧的（甚至是被释放时那一版：
  // type=kFree、只有 1 个槽），表现为"父页指向一个早已释放的页"这类静默损坏。
  uint32_t frame_idx = 0;
  bool reused = false;

  {
    std::unique_lock<std::shared_mutex> lk(im.table_mutex);
    if (im.free_list_head != kInvalidPageId) {
      pid = im.free_list_head;
      reused = true;
    } else {
      if (im.next_unallocated >= im.disk->num_pages()) {
        return Status::Error(StatusCode::kOutOfSpace,
                             "the data file has reached its page limit: " + std::to_string(im.disk->num_pages()));
      }
      pid = im.next_unallocated++;
      ++im.stats.pages_created;
    }
  }

  if (reused) {
    // 读旧页头，拿链上的下一个页号。FetchPage 自带锁（查表 + pin），所以这里
    // **不能**持 table_mutex（会递归）。
    auto g = FetchPage(pid);  // 空闲页不在键空间里，读者到不了，故不带页闩
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    const PageId next = guard.header().next_page;

    std::unique_lock<std::shared_mutex> lk(im.table_mutex);
    im.free_list_head = next;
    if (im.free_pages > 0) {
      --im.free_pages;
    }
    ++im.stats.pages_allocated;
    auto it = im.page_to_frame.find(pid);
    if (it == im.page_to_frame.end()) {
      return Status::Error(StatusCode::kCorruption,
                           "the reused free page is not in the pool after being read: " + std::to_string(pid));
    }
    frame_idx = it->second;
    Impl::Frame& fr = im.frames[frame_idx];
    if (fr.pin_count != 1) {
      return Status::Error(StatusCode::kCorruption,
                           "the reused free page has an unexpected pin count: " + std::to_string(pid));
    }
    // 【pin 的所有权转让给新页】走 Detach 而不是 Release：这一格 pin 就是新页的 pin。
    // 用 Release 会让 pin 先掉到 0，在"掉到 0"与"下面重新置 1"之间这个帧可能被
    // 并发读者触发的淘汰回收、再交给别的页（C1 下这个窗口是真实的）。
    guard.Detach();
    im.NotePinned(pid, frame_idx);
    Page page(fr.data.data());
    page.Init(pid, type, tree_id, level);
    fr.pid = pid;
    fr.dirty = true;  // 新页必须落盘，否则重开时读到全零页
    fr.valid = true;
    fr.pin_count = 1;
    im.page_to_frame[pid] = frame_idx;
  } else {
    std::unique_lock<std::shared_mutex> lk(im.table_mutex);
    auto frame_or = im.AcquireFrame(policy_, heat_, pid);
    if (!frame_or.ok()) {
      return frame_or.status();
    }
    frame_idx = frame_or.value();
    Impl::Frame& fr = im.frames[frame_idx];
    if (fr.valid && fr.pid != pid) {
      LTM_RETURN_IF_ERROR(im.PrepareVictim(frame_idx, heat_));
    }
    // 必须在 Init 之前快照 before-image：Init 会整页 memset，之后再快照就等于把
    // "已经被清零的内容"当成事务开始态，若调用方只改很少的字节，before == after
    // 会把这条 delta 直接跳过（D3），新页就会带着 kInvalidLsn 落盘。
    // 新页在 Unpin 之前一直是被钉住的，不可能被淘汰，所以这个窗口没有 I6 缺口。
    im.NotePinned(pid, frame_idx);
    Page page(fr.data.data());
    page.Init(pid, type, tree_id, level);
    fr.pid = pid;
    fr.dirty = true;  // 新页必须落盘，否则重开时读到全零页
    fr.valid = true;
    fr.pin_count = 1;
    im.page_to_frame[pid] = frame_idx;
  }
  // 记进写事务：Abort 时这些页要"收回"而不是"还原"（理由见 WriteOp 的注释）
  if (im.InWriteOp()) {
    im.write_op.allocated_in_txn.push_back(pid);
  }
  if (heat_ != nullptr) {
    heat_->Touch(pid, /*is_prefetch=*/false);
  }

  LTM_RETURN_IF_ERROR(im.PersistMeta(this));
  if (reused) {
    LOG_DEBUG("NewPage reuses the free page pid=", pid);
  }
  if (out_pid != nullptr) {
    *out_pid = pid;
  }
  return PageGuard(this, frame_idx, pid);
}

Status BufferPool::FreePage(PageId pid) {
  Impl& im = *impl_;
  if (pid == kMetaPageId) {
    return InvalidArgument("freeing the metadata page is not allowed");
  }
  // C1：改页内容（type=kFree）要持独占页闩 —— 否则可能与正在读这一页的读者并发。
  // 调用方保证这一页已经从树里摘除（不再被引用），所以这里的闩是"最后一个读者
  // 走了我才改"的语义。
  auto g = FetchPage(pid, LatchMode::kExclusive);
  if (!g.ok()) {
    return g.status();
  }
  PageGuard guard = std::move(g.value());
  // C1：free_list_head / free_pages 是帧表临界区里的共享可变状态（NewPage 与读者
  // 的取页路径都在用），读写都要进 table_mutex。
  PageId old_head = kInvalidPageId;
  {
    std::shared_lock<std::shared_mutex> lk(im.table_mutex);
    old_head = im.free_list_head;
  }
  PageHeader* h = guard.mutable_header();
  h->type = PageType::kFree;
  h->next_page = old_head;
  h->prev_page = kInvalidPageId;
  guard.mutable_page().UpdateChecksum();  // 重算校验和是写操作，必须走 mutable_page()
  {
    std::unique_lock<std::shared_mutex> lk(im.table_mutex);
    im.free_list_head = pid;
    ++im.free_pages;
    ++im.stats.pages_freed;
  }
  if (heat_ != nullptr) {
    heat_->ForgetPage(pid);
  }
  return im.PersistMeta(this);
}

Status BufferPool::Unpin(PageId pid, bool dirty) {
  Impl& im = *impl_;
  if (!dirty) {
    // 【只读守卫：共享锁】查表 + 减 pin。与淘汰段（独占）互斥，所以不会出现
    // "查表到减 pin 之间帧被换掉"的竞态。读守卫不会标脏，故不需要写路径。
    std::shared_lock<std::shared_mutex> lk(im.table_mutex);
    auto it = im.page_to_frame.find(pid);
    if (it == im.page_to_frame.end()) {
      return Status::Error(StatusCode::kCorruption,
                           "Unpin on a page that is not in the pool: " + std::to_string(pid));
    }
    Impl::Frame& fr = im.frames[it->second];
    if (fr.pin_count <= 0) {
      return Status::Error(StatusCode::kCorruption, "Unpin count exceeds the pin count: " + std::to_string(pid));
    }
    --fr.pin_count;
    return Status::Ok();
  }

  // 【脏守卫：独占锁】写路径（C0 下被独占闩串行化），但同一把锁的纪律要保持：
  // CollectDelta 会读 write_op 并把 delta 追加进 WAL，UpdateChecksum 会改帧内容。
  std::unique_lock<std::shared_mutex> lk(im.table_mutex);
  auto it = im.page_to_frame.find(pid);
  if (it == im.page_to_frame.end()) {
    return Status::Error(StatusCode::kCorruption,
                         "Unpin on a page that is not in the pool: " + std::to_string(pid));
  }
  Impl::Frame& fr = im.frames[it->second];
  if (fr.pin_count <= 0) {
    return Status::Error(StatusCode::kCorruption, "Unpin count exceeds the pin count: " + std::to_string(pid));
  }
  --fr.pin_count;
  fr.dirty = true;
  // M4-2：这一页即将落盘（可能就是下一次淘汰时），先把它的 REDO 追加进 WAL。
  // 采集点必须是 Unpin 而不是 Commit —— 理由见本文件顶部那段 M4-2 说明。
  if (im.InWriteOp()) {
    LTM_RETURN_IF_ERROR(im.CollectDelta(pid, it->second));
  } else if (im.wal != nullptr) {
    // 事务之外标脏：例如 IndexTree::Open 建根页。这些页没有 REDO 覆盖，
    // 计数上报让缺口可见，而不是静默（见 D10 的边界说明）。
    ++im.stats.deltas_outside_txn;
  }
  // 校验和必须在 **LSN 写入之后**重算：CRC 覆盖 [0, 4080)，其中包含 page_lsn
  // （页头 offset 24）。顺序反了就会把"改动前内容的 CRC"留在盘上，
  // 下次打开直接报 kChecksumMismatch。
  Page page(fr.data.data());
  page.UpdateChecksum();
  return Status::Ok();
}

Status BufferPool::FlushPage(PageId pid) {
  Impl& im = *impl_;
  std::unique_lock<std::shared_mutex> lk(im.table_mutex);
  auto it = im.page_to_frame.find(pid);
  if (it == im.page_to_frame.end()) {
    return Status::Error(StatusCode::kCorruption,
                         "FlushPage on a page that is not in the pool: " + std::to_string(pid));
  }
  return im.FlushFrame(it->second, "FlushPage");
}

Status BufferPool::FlushAll() {
  Impl& im = *impl_;
  if (im.poisoned) {
    // 池已被 I6 违反毒化：内存里可能有"改了一半"的页，宁可什么都不写，
    // 也不能让它们以"看起来完整"的样子落到盘上。
    return Status::Error(StatusCode::kCorruption,
                         "FlushAll refused: the buffer pool was poisoned by an I6 violation");
  }
  // 独占帧表锁：要遍历 page_to_frame 并逐帧落盘，遍历期间不能有人插/删表项。
  std::unique_lock<std::shared_mutex> lk(im.table_mutex);
  for (auto& kv : im.page_to_frame) {
    Impl::Frame& fr = im.frames[kv.second];
    if (!fr.dirty) {
      continue;
    }
    LTM_RETURN_IF_ERROR(im.FlushFrame(kv.second, "FlushAll"));
  }
  if (im.disk != nullptr) {
    return im.disk->SyncPageFile();
  }
  return Status::Ok();
}

const BufferPool::PoolStats& BufferPool::stats() const { return impl_->stats; }

BufferPool::PoolStats BufferPool::SnapshotStats(DiskManager::IoStats* io) const {
  std::lock_guard<std::recursive_mutex> writer(write_mutex_);
  std::shared_lock<std::shared_mutex> table(impl_->table_mutex);
  if (io != nullptr) *io = impl_->disk != nullptr ? impl_->disk->stats() : DiskManager::IoStats{};
  return impl_->stats;
}

double BufferPool::hit_rate() const {
  const uint64_t total = impl_->stats.hits + impl_->stats.misses;
  if (total == 0) {
    return 0.0;
  }
  return static_cast<double>(impl_->stats.hits) / static_cast<double>(total);
}

uint32_t BufferPool::pool_frames() const { return impl_->opt.pool_frames; }

uint32_t BufferPool::resident_pages() const {
  std::shared_lock<std::shared_mutex> lk(impl_->table_mutex);
  return static_cast<uint32_t>(impl_->page_to_frame.size());
}

uint32_t BufferPool::dirty_pages() const {
  std::shared_lock<std::shared_mutex> lk(impl_->table_mutex);
  uint32_t n = 0;
  for (const auto& kv : impl_->page_to_frame) {
    if (impl_->frames[kv.second].dirty) {
      ++n;
    }
  }
  return n;
}

// ---------------------------------------------------------------------------
// 元数据页的公开访问器（上层改索引状态必须走这里）
// ---------------------------------------------------------------------------
Status BufferPool::WriteTreeSlot(uint32_t slot, const TreeMetaSlot& value) {
  if (slot >= kMaxTreeSlots) {
    return InvalidArgument("WriteTreeSlot slot out of range");
  }
  LTM_RETURN_IF_ERROR(impl_->LoadMeta(this));
  meta_copy_->trees[slot] = value;
  return impl_->PersistMeta(this);
}

Status BufferPool::EnsureMetaLoaded() { return impl_->LoadMeta(this); }

Status BufferPool::WriteBufferMeta() {
  LTM_RETURN_IF_ERROR(impl_->LoadMeta(this));
  return impl_->PersistMeta(this);
}
bool BufferPool::ShouldFlushDirty() const {
  if (policy_ == nullptr) {
    return false;
  }
  return policy_->ShouldFlushDirty(dirty_pages());
}

// ---------------------------------------------------------------------------
// WAL 转发（M4-1 接真实实现）
// ---------------------------------------------------------------------------
StatusOr<TxnId> BufferPool::BeginOp() {
  if (impl_->wal == nullptr) {
    // 无日志模式：返回 0 号事务，调用方照常走 Append/Commit（都是空操作）
    return static_cast<TxnId>(0);
  }
  return impl_->wal->BeginOp();
}

StatusOr<Lsn> BufferPool::AppendPageDelta(TxnId txn, const PageImageDelta& delta) {
  if (impl_->wal == nullptr) {
    // 无日志模式：调用方拿到的 LSN 是 kInvalidLsn，页头也就不会被标上 LSN，
    // 写屏障的检查对此天然放行（见 Impl::FlushFrame）。
    LTM_UNUSED(txn);
    LTM_UNUSED(delta);
    return kInvalidLsn;
  }
  return impl_->wal->AppendPageDelta(txn, delta);
}

Status BufferPool::Commit(TxnId txn) {
  if (impl_->wal == nullptr) {
    LTM_UNUSED(txn);
    return Status::Ok();
  }
  return impl_->wal->Commit(txn);
}

Status BufferPool::Abort(TxnId txn) {
  if (impl_->wal == nullptr) {
    LTM_UNUSED(txn);
    return Status::Ok();
  }
  return impl_->wal->Abort(txn);
}

Lsn BufferPool::flushed_lsn() const { return (impl_->wal != nullptr) ? impl_->wal->flushed_lsn() : kInvalidLsn; }

Lsn BufferPool::last_lsn() const { return (impl_->wal != nullptr) ? impl_->wal->last_lsn() : kInvalidLsn; }

// ---------------------------------------------------------------------------
// M4-2 写事务
//
// 事务住在这里的理由见 buffer_pool.h 的说明段：M5 的双树 Put 需要**一个**事务，
// 而 BufferPool 是唯一能看见全部帧、因而唯一能在 Unpin 时采集 delta 的对象。
// 可重入，只有最外层 Commit 才真正提交。
// ---------------------------------------------------------------------------
Status BufferPool::BeginWriteOp() {
  Impl& im = *impl_;
  // 【C1：先取写锁，再看深度 —— 顺序不能反】
  // C0 时代写被独占闩串行化，"先无锁读 depth、再决定是嵌套还是开新事务"是安全的。
  // C1 下有两个写线程：若先无锁读 `write_op.depth`，两个线程会同时看到 depth>0
  // （或同时看到 0）而都走"嵌套"分支 —— 于是**两个线程同时动写者的 before-image 表**，
  // 一次 rehash 就把节点释放掉，另一个线程正好在用它 → heap-use-after-free。
  // 这正是 ASAN 在 "4 读 2 写" 下抓到的第二个崩溃（_Insert_new_node_before）。
  // `write_mutex_` 是 recursive_mutex，同线程的嵌套 BeginWriteOp 会再次加锁、计数平衡。
  write_mutex_.lock();
  ++im.write_op.depth;
  ++g_write_op_depth;
  if (im.write_op.depth > 1) {
    return Status::Ok();  // 嵌套（RTree::Insert 内部的 Erase+Insert、双树 Put）
  }
  auto txn = BeginOp();
  if (!txn.ok()) {
    --im.write_op.depth;
    --g_write_op_depth;
    write_mutex_.unlock();
    return txn.status();
  }
  im.write_op.txn = txn.value();
  im.write_op.before.clear();
  im.write_op.allocated_in_txn.clear();
  im.write_op.undo_actions.clear();
  ++im.write_op.generation;  // 新的一代：参与方据此重新注册
  // 【M6】分配器的快照点：与 before 表同一个时刻（事务开始），
  // 于是"页 + 分配器"两半状态还原到的是同一个一致点。
  im.write_op.alloc_snapshot_valid = true;
  im.write_op.alloc_free_list_head = im.free_list_head;
  im.write_op.alloc_free_pages = im.free_pages;
  im.write_op.alloc_next_unallocated = im.next_unallocated;
  return Status::Ok();
}

Status BufferPool::CommitWriteOp() {
  Impl& im = *impl_;
  if (im.write_op.depth <= 0) {
    return Status::Error(StatusCode::kCorruption, "CommitWriteOp without a matching BeginWriteOp");
  }
  --im.write_op.depth;
  --g_write_op_depth;
  if (im.write_op.depth > 0) {
    write_mutex_.unlock();  // 内层退出：把这一层锁还掉（与 Begin 的加锁配对）
    return Status::Ok();    // 最外层才提交
  }
  const TxnId txn = im.write_op.txn;
  im.write_op.txn = kInvalidTxnId;
  im.write_op.before.clear();
  im.write_op.allocated_in_txn.clear();
  im.write_op.undo_actions.clear();
  // 提交路径用不到分配器快照了；清掉它，免得下一次 BeginWriteOp 之前有人读到陈旧值。
  im.write_op.alloc_snapshot_valid = false;
  const Status s = Commit(txn);
  // 事务状态已清干净，可以放写锁了（放锁之后别的写者才能进）。
  write_mutex_.unlock();
  return s;
}

Status BufferPool::AbortWriteOp() {
  Impl& im = *impl_;
  if (im.write_op.depth <= 0) {
    return Status::Error(StatusCode::kCorruption, "AbortWriteOp without a matching BeginWriteOp");
  }
  --im.write_op.depth;
  --g_write_op_depth;
  if (im.write_op.depth > 0) {
    write_mutex_.unlock();  // 内层退出（与 Begin 的加锁配对）
    return Status::Ok();
  }
  const TxnId txn = im.write_op.txn;
  im.write_op.txn = kInvalidTxnId;
  // 【顺序】先把内存状态退回去，再写 kAbort 记录。
  //
  // 反过来的话会有一个很窄但真实的重叠窗口：kAbort 已经落进日志（"这个事务没了"），
  // 而内存里的页还是它的 after-image。此刻若另一个线程读这棵树，读到的是不该存在
  // 的记录 —— 而日志已经替它出具了"不存在"的证明。C1 下读不再取 C0 共享闩，这个
  // 窗口更关不住，所以顺序更要守住（回滚期间 depth 已是 0，不会再碰事务状态）。
  const Status rolled_back = im.RollbackWriteOp(this);
  im.write_op.before.clear();
  im.write_op.allocated_in_txn.clear();
  // 注意 undo_actions 不在这里清 —— RollbackWriteOp 已经执行并清空了它。
  const Status aborted = Abort(txn);
  write_mutex_.unlock();
  return rolled_back.ok() ? aborted : rolled_back;
}

bool BufferPool::write_op_active() const { return impl_->write_op.depth > 0; }

uint64_t BufferPool::write_op_generation() const { return impl_->write_op.generation; }

void BufferPool::RegisterWriteUndo(std::function<void()> fn) {
  Impl& im = *impl_;
  if (!im.InWriteOp()) {
    // 事务之外没有可回滚的东西：这一份"事务开始态"永远不会被用到。
    // 静默丢弃是对的（参与方总是先开事务再注册），但有个前提 —— 参与方必须
    // 在**加入事务之后**注册。见 IndexTree::Impl::RegisterRollback 的调用点。
    return;
  }
  im.write_op.undo_actions.push_back(std::move(fn));
}

// ---------------------------------------------------------------------------
// PageGuard
// ---------------------------------------------------------------------------
PageGuard::PageGuard(BufferPool* pool, uint32_t frame_index, PageId pid)
    : pool_(pool), frame_(frame_index), pid_(pid) {}

PageGuard::PageGuard(PageGuard&& other) noexcept
    : pool_(other.pool_),
      frame_(other.frame_),
      pid_(other.pid_),
      dirty_(other.dirty_),
      latch_(std::move(other.latch_)) {
  other.pool_ = nullptr;
  other.frame_ = 0;
  other.pid_ = kInvalidPageId;
  other.dirty_ = false;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this != &other) {
    Release();
    pool_ = other.pool_;
    frame_ = other.frame_;
    pid_ = other.pid_;
    dirty_ = other.dirty_;
    latch_ = std::move(other.latch_);
    other.pool_ = nullptr;
    other.frame_ = 0;
    other.pid_ = kInvalidPageId;
    other.dirty_ = false;
  }
  return *this;
}

PageGuard::~PageGuard() { Release(); }

void PageGuard::Detach() {
  // 放弃守卫但不动 pin 计数（转让给调用方）。闩也一并放弃：调用点（NewPage 复用
  // 空闲页）拿到的守卫本来就没有闩，这里只是把语义写全。
  latch_.Release();
  pool_ = nullptr;
  dirty_ = false;
  pid_ = kInvalidPageId;
}

void PageGuard::Release() {
  if (pool_ == nullptr) {
    return;
  }
  // 【C1：先放闩，再 unpin】放闩之后没人会再改这页的内容，unpin 只是归还帧。
  // 顺序反过来的话，帧可能在闩还持有时被别人换走 —— `page()` 指向的就不再是
  // 这一页了（而调用方可能正拿它做校验）。
  latch_.Release();
  const Status s = pool_->Unpin(pid_, dirty_);
  if (!s.ok()) {
    LOG_ERROR("PageGuard: Unpin failed on release: ", s.ToString());
  }
  pool_ = nullptr;
  dirty_ = false;
  pid_ = kInvalidPageId;
}

// 返回 const Page 的理由见 buffer_pool.h 的声明处（D9：可写的 Page 只能来自
// mutable_page()，而它一定会标脏）。
const Page PageGuard::page() const { return Page(pool_->FrameData(frame_)); }

PageHeader* PageGuard::mutable_header() {
  MarkDirty();
  Page p = page();
  return p.mutable_header();
}

char* PageGuard::mutable_payload() {
  MarkDirty();
  Page p = page();
  return p.mutable_payload();
}

void PageGuard::MarkDirty() { dirty_ = true; }

}  // namespace ltm

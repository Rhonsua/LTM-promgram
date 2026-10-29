// src/index/tree_ops.cc
// 跨页通用算法（结构维护半边）的唯一实现 —— 方案 D3「通用」的落地处。
//
// 本文件里的每一行都**不得**知道任何一棵树的具体页内布局：它只通过
// TreePageOps 的挂钩访问条目。判别标准很简单：
//   「改这一行时，是否需要先知道叶条目的二进制结构？」
//   是 → 它属于页算子（bplus_tree.cc / rtree.cc）；否 → 它属于这里。
//
// 共享范围与边界见 tree_ops.h 顶部的【诚实边界】。一句话：
// 插入下钻、分裂传播、借键合并、下溢循环、根升降、元数据、Validate 骨架、
// Destroy 在这里；查找半边（记录定位 + 范围/空间/KNN）在各树自己那里。
//
// 缺陷史提醒（本文件是把 bplus_tree.cc 的逻辑原样搬来的，搬家时逐条核过）：
//   #15 分隔条目要刷"所有"指向该子页的槽，不是某一槽
//   #18 借键方向搞反会把键序弄反 —— 方向由 BorrowFrom 的 dir 参数承载
//   #21 借键/合并必须循环到不下溢为止，单轮不够
//   #23/#24 忘记标脏 / 未重算校验和 —— 所有挂钩调用点都先 mutable_page()
//   #26 LoadMeta 必须先 EnsureMetaLoaded，否则重开会静默建空树并覆盖根页
//   #28 Destroy 不跳空槽会 Fetch(kInvalidPageId) 触发自报家门断言
#include "index/tree_ops.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <unordered_set>
#include <utility>
#include <vector>

#include "index/page_utils.h"
#include "util/log.h"

namespace ltm {

using pageutil::LiveSlots;
using pageutil::SlotIsEmpty;
using pageutil::SlotsOf;

// ---------------------------------------------------------------------------
// ScopedWriteOp（语义与"为什么是 RAII"见 tree_ops.h 的声明处）
// ---------------------------------------------------------------------------
ScopedWriteOp::ScopedWriteOp(BufferPool* pool) : pool_(pool) {
  if (pool_ != nullptr) {
    begin_ = pool_->BeginWriteOp();
  }
}

ScopedWriteOp::~ScopedWriteOp() {
  if (pool_ != nullptr && begin_.ok() && !finished_) {
    // 返回值故意丢弃：本析构常常运行在栈展开路径上（此刻已经有一个错误在传播），
    // 再报一个错的唯一效果是把真正的病因盖掉。Abort 自身失败会记进日志。
    LTM_UNUSED(pool_->AbortWriteOp());
  }
}

Status ScopedWriteOp::Commit() {
  if (pool_ == nullptr) {
    return Status::Ok();
  }
  if (!begin_.ok()) {
    // 开启事务就失败了（如无 DiskManager 的 WalWriter）。原样上报，
    // 绝不降级成"没有日志也照样改页"——那正是 I6 失效的形态。
    return begin_;
  }
  const Status s = pool_->CommitWriteOp();
  if (s.ok()) {
    finished_ = true;
  }
  return s;
}

void ScopedWriteOp::Abort() {
  if (pool_ != nullptr && begin_.ok() && !finished_) {
    LTM_UNUSED(pool_->AbortWriteOp());
    finished_ = true;
  }
}

// ===========================================================================
// TreePageOps 的默认实现
//
// 原则（见头文件 v3 挂钩区）：只有**中性**语义才给默认值 —— 即"这个默认值对
// 任何一棵树都成立"，而不是"它恰好是第一棵树的行为"。语义挂钩一律纯虚。
// ===========================================================================
TreeEntry TreePageOps::BuildSeparator(const PageHeader& child_header,
                                      const char* child_payload) const {
  TreeEntry e;
  e.key = FirstKey(child_header, child_payload);
  e.child = child_header.page_id;
  return e;
}

Status TreePageOps::InsertChildAt(PageHeader* header, char* payload, SlotId at,
                                  const TreeEntry& entry) {
  ElemKey key = entry.key;
  // 最左子页哨兵：0 号槽的键不承载路由语义，写死哨兵值。
  // 对没有哨兵的树（多维树）这一条自然不生效，退化成普通插入。
  if (at == 0 && HasLeftmostSentinel()) {
    key = kMinElemKey;
  }
  return InsertEntry(header, payload, at, key, entry.child, entry.value);
}

PageId TreePageOps::ChooseHalf(const TreeEntry& entry, PageId left, const PageHeader& left_header,
                               const char* left_payload, PageId right,
                               const PageHeader& right_header, const char* right_payload) const {
  LTM_UNUSED(left_payload);
  LTM_UNUSED(right_payload);
  // EntryBytes 已经把槽目录项算进去了（见契约注释），所以这里不再加 sizeof(Slot)。
  const uint32_t need = EntryBytes(entry);
  if (left_header.free_space >= need && left != kInvalidPageId) {
    return left;
  }
  if (right_header.free_space >= need && right != kInvalidPageId) {
    return right;
  }
  return kInvalidPageId;
}

uint16_t TreePageOps::UnderflowThreshold() const {
  const uint16_t cap = Capacity();
  const uint16_t half = static_cast<uint16_t>(cap / 2);
  return half < 2 ? 2 : half;
}

bool TreePageOps::IsTombstone(const PageHeader& header, const char* payload, SlotId slot) const {
  if (slot >= header.num_slots) {
    return true;
  }
  return SlotIsEmpty(SlotsOf(payload)[slot]);
}

void TreePageOps::AccumulateOverlap(const PageHeader& header, const char* payload, double* sum,
                                    uint64_t* count) const {
  LTM_UNUSED(header);
  LTM_UNUSED(payload);
  LTM_UNUSED(sum);
  LTM_UNUSED(count);
}

StatusOr<std::vector<SlotId>> TreePageOps::CandidateChildSlots(const PageHeader& header,
                                                               const char* payload,
                                                               const IndexEntry& target,
                                                               SlotChoice purpose) const {
  std::vector<SlotId> out;
  if (header.num_slots == 0) {
    return out;
  }
  const SlotId s = ChooseChildSlot(header, payload, target, purpose);
  if (s < header.num_slots) {
    out.push_back(s);
  }
  return out;
}

// ===========================================================================
// IndexTree::Impl —— 通用跨页算法
// ===========================================================================
// C1：本线程是否正在走某棵树的写路径。见 WritePathScope 的说明 —— 它必须是
// thread_local：写线程与读线程共用同一个 Impl，用成员会让读者误取独占闩。
thread_local bool g_write_path = false;

struct IndexTree::Impl {
  BufferPool* pool = nullptr;
  TreePageOps* page_ops = nullptr;
  TreeBuildOptions options;
  TreeId tree_id = 0;
  PageId root = kInvalidPageId;
  PageId leftmost_leaf = kInvalidPageId;
  uint8_t height = 0;
  uint64_t num_entries = 0;
  TreeStats stats;
  bool opened = false;
  PageStatsAccumulator acc;
  // 本树最近一次向写事务注册派生状态回滚时所处的事务代号。见 RegisterRollback()。
  uint64_t rollback_gen = 0;

  // ---- C1 · 写路径标记 ----
  //
  // 【为什么用一个标记，而不是给每个 Fetch 传 mode】写路径的取页点有二十多处，
  // 逐个标注一定会漏；漏一处就是"这一页被并发读者读着的时候改"，静默撕裂。
  // 写入口用 RAII 把"当前在写路径上"置位，`Fetch` 据此把默认的共享闩升为独占闩。
  //
  // 【它必须是 thread_local，不能是 Impl 的成员】写入口与读者跑在**不同线程**上、
  // 却共用同一个 Impl。若它是成员，写线程把它置位之后，同树的读线程会看到 true
  // 并给读路径取**独占**闩 —— 实测在 1 写 1 读下就 Segfault。
  // 它是"本线程此刻在不在写路径上"的属性，所以是 thread_local。
  struct WritePathScope {
    bool prev;
    explicit WritePathScope(Impl*) : prev(g_write_path) { g_write_path = true; }
    ~WritePathScope() { g_write_path = prev; }
    WritePathScope(const WritePathScope&) = delete;
    WritePathScope& operator=(const WritePathScope&) = delete;
  };

  // ---- C1 · 结构元数据闩 ----
  //
  // root / height / leftmost_leaf **不在任何页里**：读者在 DescendToLeaf 开头读它们，
  // 写者在根分裂/降级时改它们。写者是单写者，但读者是并发的，所以要给"快照"与
  // "改根"互斥。它只在根变化的那一瞬间被独占（很少发生），读者取共享时彼此不阻塞 ——
  // 这正是 C1 相对 C0 的关键：读者不再被写操作整段挡住。
  //
  // 【它为什么不能省】读者若读到"新 root + 旧 height"（写者改到一半），下钻层数就错了；
  // 若读到已被降级释放的旧 root，会去读一个已回收的页。两者都必须靠这把闩挡住。
  std::shared_mutex structure_mutex;

  // 把本树的**事务开始态**注册给当前写事务（M6 · Abort 的运行期回滚）。
  //
  // 【为什么这棵树要自己管这件事】root / leftmost_leaf / height / num_entries 是
  // 跨页算法持有的派生状态，它们**不在任何页里**：页 0 的树槽位只在
  // Open/Close/Checkpoint 时同步（见 PersistMeta 的调用点），不是每次插入都写。
  // 所以缓冲池的 before-image 再完整也够不着它们。不回滚的后果不是"数字不好看"：
  // Validate 会直接报 "total entries in pages 400 != metadata count 600"，
  // 而且此后每一次 RangeScan/统计都建立在一个不存在的计数上。
  //
  // 【必须在改动任何东西之前调用】捕获的是"此刻"的值，而此刻正是本调用加入事务、
  // 尚未动手的时刻 —— 这就是事务开始态。放在改动之后注册等于把 after-image 当
  // before-image，回滚会回滚到一半的中间态。
  //
  // 【去重靠代号】一次 Upsert 会进出这棵树好几次（Erase + Insert），每次都会走到
  // 这里；用 generation 拦住重复注册，否则同一份开始态会被压进 undo 列表好几遍。
  void RegisterRollback() {
    if (pool == nullptr || !pool->write_op_active()) {
      return;
    }
    const uint64_t gen = pool->write_op_generation();
    if (gen == rollback_gen) {
      return;
    }
    rollback_gen = gen;
    const PageId r = root;
    const PageId l = leftmost_leaf;
    const uint8_t h = height;
    const uint64_t n = num_entries;
    pool->RegisterWriteUndo([this, r, l, h, n]() {
      // Rollback publishes the same structural state as root promotion/demotion.
      // Even an unchanged root must synchronize with concurrent descent snapshots.
      std::unique_lock<std::shared_mutex> lock(structure_mutex);
      root = r;
      leftmost_leaf = l;
      height = h;
      num_entries = n;
    });
  }

  uint16_t capacity() const { return page_ops != nullptr ? page_ops->Capacity() : 0; }
  uint32_t slot() const { return options.caller_tree_slot; }

  // 取页。见下方实现注释：这一步同时是"页必须自报家门"的不变量检查点。
  //
  // 【C1 的 mode】默认 kShared（读）。**写路径一律传 kExclusive**：写者要改页内容，
  // 必须把读者挡在外面；读者持共享闩，两者互斥。写路径是单写者（D4），所以"写者之间"
  // 不需要页闩来排序，页闩在这里只承担"写与读互斥"。
  StatusOr<PageGuard> Fetch(PageId pid, LatchMode mode = LatchMode::kShared);
  Status PersistMeta();
  Status LoadMeta();

  // 单路径下钻（插入路径内部使用；也供一维树的查询复用）
  StatusOr<PageGuard> DescendToLeaf(const IndexEntry& target, SlotChoice purpose);

  // 插入的递归主体。entry 是逻辑记录（路由判据），elem 是编码后的页内条目。
  // leaf_out 非空时写出"记录最终落在哪个叶页"（下钻决定页，分裂可能换页）。
  Status InsertRec(PageId pid, const IndexEntry& entry, const TreeEntry& elem, bool* split_out,
                   PageId* new_pid_out, PageId* leaf_out);
  // 删除的递归主体。*erased_out 报告"这棵子树里到底有没有删掉东西"，
  // 多维树的多个候选孩子靠它决定要不要继续试下一个。
  Status EraseRec(PageId pid, const IndexEntry& entry, bool is_root, bool* underflow_out,
                  bool* erased_out);

  Status SyncSeparatorForChild(PageId parent_pid, PageId child_pid);
  Status Rebalance(PageId parent_pid, SlotId child_slot);
  Status RebalanceOnce(PageId parent_pid, SlotId* child_slot_io, bool* progress_out);
  // 借键的一侧：把 donor 的条目搬进 child，循环到 child 不再下溢 / donor 无余量
  Status BorrowLoop(PageId parent_pid, PageId child_pid, PageId donor_pid, BorrowDirection dir,
                    uint16_t min_fill, bool* progress_out);
  // 叶页删除一个槽（含压实与条目计数），并报告该叶页是否因此下溢
  Status EraseLeafSlotAt(PageId leaf_pid, SlotId slot, bool* underflow_out);

  // 「空间先满」协议的后半步：分裂该叶页，再把 elem 插进合适的一半。
  // 前置条件：guard 是叶页、InsertLeafEntry 已返回 kPageFull、CheckEntryFits 已过。
  // leaf_out 非空时写出 elem 最终落在哪一半（pid 或新页）。
  Status SplitLeafAndReinsert(PageId pid, PageGuard& guard, const TreeEntry& elem,
                              bool* split_out, PageId* new_pid_out, PageId* leaf_out);
};

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
StatusOr<PageGuard> IndexTree::Impl::Fetch(PageId pid, LatchMode mode) {
  // C1：写路径上默认升为独占闩（见 write_path 的说明）。显式传 kExclusive 的调用点
  // 不受影响；显式传 kNone 也不会被改写（缓冲池内部取页走的是 pool->FetchPage，
  // 不经过这里）。
  const LatchMode eff =
      (mode == LatchMode::kShared && g_write_path) ? LatchMode::kExclusive : mode;
  auto g = pool->FetchPage(pid, eff);
  if (!g.ok()) {
    return g.status();
  }
  // 不变量：取回来的页必须自报家门。这一条能抓住"两个页号共用一块帧内存"
  // （帧被复用后旧守卫的 Page 视图仍然在写）这类最难定位的静默损坏：
  // 那种情况下帧里装的是别的页的内容，header.page_id 与请求的页号不一致。
  if (g.value().header().page_id != pid) {
    return Status::Error(StatusCode::kCorruption,
                         "the fetched page claims page_id=" + std::to_string(g.value().header().page_id) +
                             ", but the requested page is " + std::to_string(pid));
  }
  // 只在真的不同时才写。必须先用 const 的 header() 比较：mutable_header() 会先
  // MarkDirty()，无条件写等于"每次 Fetch 都标脏"，连 Search / RangeScan / Validate
  // 这些只读路径也不例外。实测后果：两棵树的驱逐统计都是 100% 脏
  // （902/902、202416/202416），只读一遍也要把 4KB 页写回盘。
  // M4-2 的 WAL 采集挂在"即将落盘"的页上，这里不修的话每读一页都会灌一条 delta。
  if (g.value().header().tree_id != tree_id) {
    g.value().mutable_header()->tree_id = tree_id;
  }
  return std::move(g.value());
}

// 元数据只存在页 0 的对应槽位里（storage/meta_page.h）
Status IndexTree::Impl::PersistMeta() {
  TreeMetaSlot s{};
  s.in_use = 1;
  s.kind = static_cast<uint8_t>(page_ops->MetaKind());
  s.height = height;
  s.order_capacity = capacity();
  s.dim = options.mbr_dim;
  s.root = root;
  s.leftmost_leaf = leftmost_leaf;
  s.num_entries = num_entries;
  return pool->WriteTreeSlot(slot(), s);
}

Status IndexTree::Impl::LoadMeta() {
  if (slot() >= kMaxTreeSlots) {
    return InvalidArgument("tree slot out of range");
  }
  // 先把元数据页从磁盘加载进内存镜像。漏掉这一步的话，重开数据库时读到的是
  // "尚未加载"的全零镜像 —— 槽位 in_use == 0 会被解释成"树不存在"，
  // 于是 Open 新建一棵空树并覆盖掉原数据的根页（缺陷 #26：重开后 0 条记录）。
  LTM_RETURN_IF_ERROR(pool->EnsureMetaLoaded());
  const TreeMetaSlot& s = pool->meta().trees[slot()];
  if (s.in_use == 0) {
    return NotFound("the metadata tree slot is empty (a new tree must be created)");
  }
  if (s.kind != static_cast<uint8_t>(page_ops->MetaKind())) {
    return Status::Error(StatusCode::kCorruption,
                         std::string("the tree kind in the metadata slot is not ") + page_ops->name());
  }
  root = s.root;
  height = s.height;
  leftmost_leaf = s.leftmost_leaf;
  num_entries = s.num_entries;
  return Status::Ok();
}

IndexTree::IndexTree() : impl_(new Impl()) {}
IndexTree::~IndexTree() = default;

Status IndexTree::Open(BufferPool* pool, TreePageOps* page_ops, const TreeBuildOptions& options,
                       TreeId tree_id) {
  if (pool == nullptr || page_ops == nullptr) {
    return InvalidArgument("IndexTree::Open needs a BufferPool and TreePageOps");
  }
  Impl& im = *impl_;
  im.pool = pool;
  im.page_ops = page_ops;
  im.options = options;
  im.tree_id = tree_id;

  // 【Open 也要包事务】建一棵新树会改两个页：页 0（元数据槽位）与根页。
  // 不包的话它们就是"事务外标脏"——delta 采集不到（stats.deltas_outside_txn 会涨），
  // 恢复时根页无从重建，而根页丢失等于整棵树丢失。D1 的补充定义要求在这里表态，
  // 表的就是"包一个事务"，而不是让它漂在事务外。
  ScopedWriteOp op(pool);

  const Status loaded = im.LoadMeta();
  if (loaded.ok()) {
    im.opened = true;
    LOG_INFO("index tree opened an existing structure: kind=", page_ops->name(), " root=", im.root,
             " height=", static_cast<int>(im.height), " entries=", im.num_entries);
    return op.Commit();
  }
  if (loaded.code() != StatusCode::kNotFound) {
    return loaded;
  }

  PageId root_pid = kInvalidPageId;
  auto g = pool->NewPage(page_ops->PageTypeForLevel(0), tree_id, /*level=*/0, &root_pid);
  if (!g.ok()) {
    return g.status();
  }
  PageGuard guard = std::move(g.value());
  LTM_RETURN_IF_ERROR(page_ops->InitPage(guard.mutable_header(), guard.mutable_payload(), true));
  guard.mutable_header()->flags |= kPageFlagRoot;
  guard.Release();

  im.root = root_pid;
  im.leftmost_leaf = root_pid;
  im.height = 0;
  im.num_entries = 0;
  im.opened = true;
  LTM_RETURN_IF_ERROR(im.PersistMeta());
  LOG_INFO("index tree created: kind=", page_ops->name(), " root=", root_pid,
           " capacity=", im.capacity());
  return op.Commit();
}

bool IndexTree::opened() const { return impl_->opened; }
uint16_t IndexTree::capacity() const { return impl_->capacity(); }
BufferPool* IndexTree::pool() const { return impl_->pool; }
TreePageOps* IndexTree::page_ops() const { return impl_->page_ops; }
TreeId IndexTree::tree_id() const { return impl_->tree_id; }
PageId IndexTree::root() const { return impl_->root; }
uint8_t IndexTree::height() const { return impl_->height; }
uint64_t IndexTree::num_entries() const { return impl_->num_entries; }
const TreeBuildOptions& IndexTree::options() const { return impl_->options; }
PageId IndexTree::leftmost_leaf() const { return impl_->leftmost_leaf; }
StatusOr<PageGuard> IndexTree::Fetch(PageId pid) { return impl_->Fetch(pid); }

Status IndexTree::CheckEntryFits(const TreeEntry& entry) const {
  Impl& im = *impl_;
  const uint32_t bytes = im.page_ops->EntryBytes(entry);
  if (bytes > kPagePayloadSize) {
    return InvalidArgument("the entry needs " + std::to_string(bytes) + " bytes, exceeding the single-page payload " +
                           std::to_string(kPagePayloadSize) + " bytes (overflow-page support required, not implemented)");
  }
  // 页算子声明了字节预算时，单条不得超过"每页均分给 capacity_ 条的份额"。
  // 否则变长条目会让页在达到 capacity_ 条之前先被字节撑满，
  // 而 IsOverflow（槽数口径）永远不触发 —— 分裂路径形同虚设，页会写坏。
  const uint32_t budget = im.page_ops->EntryByteBudget();
  if (budget != 0 && bytes > budget) {
    return InvalidArgument("the entry needs " + std::to_string(bytes) + " bytes, exceeding the per-page budget " +
                           std::to_string(budget) + " bytes (reduce capacity or shorten the payload)");
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 下钻
// ---------------------------------------------------------------------------
StatusOr<PageGuard> IndexTree::Impl::DescendToLeaf(const IndexEntry& target, SlotChoice purpose) {
  PageId cur = kInvalidPageId;
  uint8_t h = 0;
  {
    // C1：root 与 height 必须作为**一致的一对**快照 —— 写者在根分裂/降级时把两者
    // 一起改，读者若读到"新 root + 旧 height"下钻层数就错了（见 structure_mutex 的说明）。
    std::shared_lock<std::shared_mutex> sl(structure_mutex);
    cur = root;
    h = height;
  }
  if (cur == kInvalidPageId) {
    return Status::Error(StatusCode::kCorruption, "the tree is not initialized (root is invalid)");
  }
  auto rg = Fetch(cur);
  if (!rg.ok()) {
    return rg.status();
  }
  PageGuard guard = std::move(rg.value());
  for (uint8_t level = h; level > 0; --level) {
    if (guard.header().level != level) {
      return Status::Error(StatusCode::kCorruption,
                           "page level disagrees with the tree height (page " + std::to_string(cur) + ")");
    }
    const SlotId child_slot =
        page_ops->ChooseChildSlot(guard.header(), guard.payload(), target, purpose);
    const PageId child = page_ops->GetChild(guard.header(), guard.payload(), child_slot);
    if (child == kInvalidPageId) {
      return Status::Error(StatusCode::kCorruption, "an internal page returned an illegal child");
    }
    // ---- C1 · latch coupling（crabbing），且**不额外占帧** ----
    //
    // 方案 §4.6 要的是"fetch(父) → 拿子页闩 → 放父页闩"。直接照做会让每读者同时钉住
    // "父 + 子"两帧：8 读者 × 树高就把 16 帧的池子钉满，`AcquireFrame` 报 kOutOfSpace
    // （实测，见 report/c1/c1_groundwork.txt）。这里把"闩"与"pin"解耦：
    //   * **留闩**：写者要改父页必须拿父页独占闩 —— 在读者拿到子页闩之前父页路由不会
    //     被改，child 也就不会被合并/回收（回收必须摘父页的槽）。
    //   * **放 pin**：闩是页号级的、与帧是否驻留无关；放掉 pin 后帧可被复用，
    //     于是"持父页闩"期间每读者只钉 1 帧（子页那帧）。
    // 拿住父页闩直到子页闩到手，正是"读者选定 child"与"读者拿到 child 闩"之间那个
    // 窗口的封口。
    PageLatch parent_latch = guard.take_latch();  // 闩交出来，守卫只剩 pin
    guard.Release();                              // 放父页 pin（帧可被复用）
    auto cg = Fetch(child);                       // 父页闩仍持有 → child 的路由此刻有效
    parent_latch.Release();                       // 子页闩到手（或取页失败），放父页闩
    if (!cg.ok()) {
      return cg.status();
    }
    guard = std::move(cg.value());
    cur = child;
  }
  if (guard.header().level != 0) {
    return Status::Error(StatusCode::kCorruption, "descent ended on something that is not a leaf page");
  }
  return guard;
}

StatusOr<PageGuard> IndexTree::DescendToLeaf(const IndexEntry& target, SlotChoice purpose) {
  return impl_->DescendToLeaf(target, purpose);
}

// ---------------------------------------------------------------------------
// 父页分隔条目（I1 一维 / I9 多维的唯一维护点）
// ---------------------------------------------------------------------------
StatusOr<TreeEntry> IndexTree::SeparatorOf(PageId child_pid) {
  Impl& im = *impl_;
  auto cg = im.Fetch(child_pid);
  if (!cg.ok()) {
    return cg.status();
  }
  return im.page_ops->BuildSeparator(cg.value().header(), cg.value().payload());
}

// 用子页当前状态刷新父页里**所有**指向它的分隔条目。
//
// 为什么是"所有"而不是"某一槽"：
//   * B+ 内部页的 0/1 号槽是一对冗余引用（0 号持哨兵键、1 号持同一子页的最小键），
//     只刷其中一槽，另一槽立刻变陈旧，Validate 的 I1 就报错（缺陷 #15）。
//   * 多维树里一次借键/合并会让两侧的 MBR 并集同时变化，两槽都得跟着改。
// 起始下标由 HasLeftmostSentinel() 决定：B+ 跳过 0 号哨兵槽（它的键恒为
// kMinElemKey，不承载路由语义）；多维树 0 号槽是真条目，必须参与刷新。
Status IndexTree::Impl::SyncSeparatorForChild(PageId parent_pid, PageId child_pid) {
  auto pg = Fetch(parent_pid);
  if (!pg.ok()) {
    return pg.status();
  }
  PageGuard parent = std::move(pg.value());
  const uint16_t n = parent.page().num_slots();
  const uint16_t start = page_ops->HasLeftmostSentinel() ? 1 : 0;
  if (n <= start) {
    return Status::Ok();
  }
  auto cg = Fetch(child_pid);
  if (!cg.ok()) {
    return cg.status();
  }
  PageGuard child = std::move(cg.value());
  // 空页即将被合并/回收，此时给它写一个分隔条目只会污染父页。
  if (LiveSlots(child.header(), child.payload()) == 0) {
    return Status::Ok();
  }
  const TreeEntry sep = page_ops->BuildSeparator(child.header(), child.payload());
  for (uint16_t s = start; s < n; ++s) {
    if (page_ops->IsTombstone(parent.header(), parent.payload(), s)) {
      continue;
    }
    if (page_ops->GetChild(parent.header(), parent.payload(), s) != child_pid) {
      continue;
    }
    // 判等后再写：无条件改写会把本来干净的父页标脏，改变缓冲池的驱逐与写盘
    // 次数，M2 记录的性能基线就复现不出来了（这一步是行为保真，不是优化）。
    if (page_ops->SeparatorEquals(parent.header(), parent.payload(), s, sep)) {
      continue;
    }
    LTM_RETURN_IF_ERROR(
        page_ops->WriteSeparator(parent.mutable_header(), parent.mutable_payload(), s, sep));
  }
  return Status::Ok();
}

Status IndexTree::SyncSeparatorForChild(PageId parent_pid, PageId child_pid) {
  return impl_->SyncSeparatorForChild(parent_pid, child_pid);
}

Status IndexTree::EraseLeafSlot(PageId leaf_pid, SlotId slot, bool* underflow_out) {
  return impl_->EraseLeafSlotAt(leaf_pid, slot, underflow_out);
}

Status IndexTree::Impl::EraseLeafSlotAt(PageId leaf_pid, SlotId slot, bool* underflow_out) {
  if (underflow_out != nullptr) {
    *underflow_out = false;
  }
  auto g = Fetch(leaf_pid);
  if (!g.ok()) {
    return g.status();
  }
  PageGuard guard = std::move(g.value());
  if (guard.header().level != 0) {
    return Status::Error(StatusCode::kCorruption, "EraseLeafSlot got a non-leaf page");
  }
  Page p = guard.mutable_page();
  LTM_RETURN_IF_ERROR(p.EraseEntry(slot));
  // EraseEntry 只把槽标记为墓碑（num_slots 不变，空间记入 frag_bytes）。
  // 若不立即压实，"存活条目数"与 num_slots 就会分叉：
  //   * IsUnderflow 按 num_slots 判断 → 删空的叶页永远不触发借键/合并，
  //     树无法降级（EraseDownToEmpty 用例会停在 height=2）；
  //   * Validate 的条目总数、叶链键序检查会把墓碑当成键 0 而误报损坏。
  // 页内条目数最多为 capacity，O(capacity) 的压实可以随每次删除做。
  p.Compact();
  if (num_entries > 0) {
    --num_entries;
  }
  if (underflow_out != nullptr && page_ops->IsUnderflow(guard.header(), guard.payload())) {
    *underflow_out = true;
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 写入：插入
// ---------------------------------------------------------------------------
Status IndexTree::Impl::InsertRec(PageId pid, const IndexEntry& entry, const TreeEntry& elem,
                                  bool* split_out, PageId* new_pid_out, PageId* leaf_out) {
  *split_out = false;
  auto g = Fetch(pid);
  if (!g.ok()) {
    return g.status();
  }
  PageGuard guard = std::move(g.value());

  if (guard.header().level == 0) {
    // 记录落在这个叶页上（走了分裂分支时由 SplitLeafAndReinsert 改写成实际那一半）
    if (leaf_out != nullptr) {
      *leaf_out = pid;
    }
    // 叶页插入整体交给页算子：去重判据、覆盖处理、压实重试都是页内语义
    // （一维树按键覆盖，多维树按 (键, MBR) 双匹配），通用层不得参与。
    auto r = page_ops->InsertLeafEntry(guard.mutable_header(), guard.mutable_payload(), elem);
    if (r.ok()) {
      // 覆盖式插入不能增加条目计数：元数据计数必须恒等于"叶页存活条目总数"
      // （Validate 会逐条比对），早期版本在这里无条件 ++，重复插入会把计数撑大。
      if (r.value() == LeafInsertResult::kInserted) {
        ++num_entries;
      }
    } else if (r.status().code() != StatusCode::kPageFull) {
      return r.status();
    } else {
      // ---- 「空间先满」协议：插入 → kPageFull → 分裂 → 选半 → 重插 ----
      //
      // 走到这里说明：页在压实之后仍然放不下（kPageFull），但 CheckEntryFits
      // 已经保证这条条目单独放进**空页**是放得下的。因此这不是"entry too large"，
      // 而是"页满了该分裂"—— 直接报错会让变长条目的树在页还没到 capacity_
      // 条时就卡死（定长条目的 B+ 树永远走不到：capacity_ 远小于页字节容量）。
      return SplitLeafAndReinsert(pid, guard, elem, split_out, new_pid_out, leaf_out);
    }
  } else {
    const SlotId child_slot =
        page_ops->ChooseChildSlot(guard.header(), guard.payload(), entry, SlotChoice::kInsert);
    const PageId child = page_ops->GetChild(guard.header(), guard.payload(), child_slot);
    if (child == kInvalidPageId) {
      return Status::Error(StatusCode::kCorruption, "internal page child is invalid");
    }
    guard.Release();  // 单线程下钻，不需要 latch coupling

    bool child_split = false;
    PageId new_child = kInvalidPageId;
    LTM_RETURN_IF_ERROR(InsertRec(child, entry, elem, &child_split, &new_child, leaf_out));

    // 重新取本页守卫（帧可能已被淘汰复用）
    auto g2 = Fetch(pid);
    if (!g2.ok()) {
      return g2.status();
    }
    guard = std::move(g2.value());

    if (child_split) {
      // 子页分裂出的新页紧跟在 child_slot 之后，因此插到 child_slot + 1。
      // 分隔条目由新页现状推导（单一来源），不接收 SplitInto 顺手返回的副本 ——
      // 双真相一旦漂移就是 I1/I9 被静默破坏。
      auto nl = Fetch(new_child);
      if (!nl.ok()) {
        return nl.status();
      }
      const TreeEntry sep =
          page_ops->BuildSeparator(nl.value().header(), nl.value().payload());
      nl.value().Release();
      const SlotId at = static_cast<SlotId>(child_slot + 1);
      Status st =
          page_ops->InsertChildAt(guard.mutable_header(), guard.mutable_payload(), at, sep);
      if (st.code() == StatusCode::kPageFull) {
        guard.mutable_page().Compact();
        st = page_ops->InsertChildAt(guard.mutable_header(), guard.mutable_payload(), at, sep);
      }
      LTM_RETURN_IF_ERROR(st);
    }

    if (page_ops->SeparatorMayChangeOnInsert()) {
      // 多维树：本次插入让 child（以及分裂出的新页）的 MBR 并集变大，
      // 父页里指向它们的槽必须跟着刷新 —— 逐层都要，分裂与非分裂都要，
      // 分裂后两半都要，且在下钻重插**之后**（此刻子页才是最终状态）。
      guard.Release();
      LTM_RETURN_IF_ERROR(SyncSeparatorForChild(pid, child));
      if (child_split) {
        LTM_RETURN_IF_ERROR(SyncSeparatorForChild(pid, new_child));
      }
      auto g3 = Fetch(pid);
      if (!g3.ok()) {
        return g3.status();
      }
      guard = std::move(g3.value());
    }
  }

  if (page_ops->IsOverflow(guard.header(), guard.payload())) {
    const uint8_t level = guard.header().level;
    PageId new_pid = kInvalidPageId;
    auto ng = pool->NewPage(page_ops->PageTypeForLevel(level), tree_id, level, &new_pid);
    if (!ng.ok()) {
      return ng.status();
    }
    PageGuard new_guard = std::move(ng.value());
    // 必须走 mutable_page()：它会把这个守卫标脏。用 page() 取视图再改内容，
    // 缓冲池不知道页被改过，淘汰时就可能既不写盘也不重算校验和（缺陷 #23/#24）。
    Page np = new_guard.mutable_page();
    LTM_RETURN_IF_ERROR(page_ops->SplitInto(guard.mutable_header(), guard.mutable_payload(), &np));
    ++stats.num_splits;
    if (level == 0) {
      // 叶链修补：old -> new -> old_next（old_next 的 prev 在这里修正）
      const PageId old_next = new_guard.header().next_page;
      if (old_next != kInvalidPageId) {
        auto og = Fetch(old_next);
        if (og.ok()) {
          og.value().mutable_header()->prev_page = new_pid;
        }
      }
    }
    *split_out = true;
    *new_pid_out = new_pid;
  }
  return Status::Ok();
}

// 「空间先满」协议的后半步：页被字节撑满但槽数还没到 capacity_，
// 于是 IsOverflow 永远不触发、上面的分裂路径形同虚设。这里的处理是：分裂，
// 再由页算子决定 "条目该进哪一半"（多维树按最小面积增量，通用默认按谁放得下）。
//
// 分裂后必然放得下：进入本函数时该页存活 N < capacity_ 条（IsOverflow 在每次
// 插入末尾都会把达到 capacity_ 的页拆掉），分裂后每半 ≤ ⌈N/2⌉，再插一条
// 也不会到 capacity_（capacity_ 下限为 4）。
Status IndexTree::Impl::SplitLeafAndReinsert(PageId pid, PageGuard& guard, const TreeEntry& elem,
                                            bool* split_out, PageId* new_pid_out, PageId* leaf_out) {
  if (guard.header().level != 0) {
    return Status::Error(StatusCode::kCorruption, "SplitLeafAndReinsert got a non-leaf page");
  }
  if (guard.page().num_slots() < 3) {
    // 只剩 1~2 条却仍放不下：只能是这一条本身太大（CheckEntryFits 用空页估算，
    // 与"页里已经有东西"的实际可用空间不同）。明确报错，绝不静默损坏。
    return Status::Error(StatusCode::kPageFull,
                         "the leaf page holds only " + std::to_string(guard.page().num_slots()) +
                             " entries and still cannot fit this entry: entry too large, overflow-page support required (not implemented)");
  }

  // 【分裂的合法性前置检查：分不出两半都合格时，宁可报错也不能拆】
  //
  // 分裂点由页算子定（B+ 取中点）。若该页的存活条目数不足 2×下溢阈值，中点分裂
  // 必然让至少一半低于填充下限 —— 树会当场变成 Validate 眼里的损坏状态。
  //
  // 这条路径是「字节先满」的兜底（槽数没到 capacity_，但页字节装不下），
  // 只有变长条目的树（R）在正常负载下会走到；B+ 走到这里说明 capacity_ 选得
  // 超过了页字节容量 —— 那是配置错误，必须吵，不能静默降级。
  //
  // 判据用页算子自己的阈值口径（UnderflowThreshold），通用层不另算一套：
  // 两处各算一次就会出现"通用层认为合法、页算子认为下溢"。
  const uint16_t threshold = page_ops->UnderflowThreshold();
  const uint32_t live = pageutil::LiveSlots(guard.header(), guard.payload());
  if (live < 2u * static_cast<uint32_t>(threshold)) {
    return Status::Error(
        StatusCode::kPageFull,
        "page " + std::to_string(pid) + " live " + std::to_string(live) + " entries, underflow threshold " +
            std::to_string(threshold) + ": a midpoint split would leave both halves below the threshold (violates I3); " +
            "split refused. The usual cause is a capacity larger than the page's byte capacity, or a single entry that is too large relative to the page");
  }

  PageId new_pid = kInvalidPageId;
  auto ng = pool->NewPage(page_ops->PageTypeForLevel(0), tree_id, /*level=*/0, &new_pid);
  if (!ng.ok()) {
    return ng.status();
  }
  PageGuard new_guard = std::move(ng.value());
  Page np = new_guard.mutable_page();
  LTM_RETURN_IF_ERROR(page_ops->SplitInto(guard.mutable_header(), guard.mutable_payload(), &np));
  ++stats.num_splits;

  // 叶链修补：old -> new -> old_next（old_next 的 prev 在这里修正）
  {
    const PageId old_next = new_guard.header().next_page;
    if (old_next != kInvalidPageId) {
      auto og = Fetch(old_next);
      if (og.ok()) {
        og.value().mutable_header()->prev_page = new_pid;
      }
    }
  }

  const PageId target = page_ops->ChooseHalf(elem, pid, guard.header(), guard.payload(), new_pid,
                                             new_guard.header(), new_guard.payload());
  if (target == kInvalidPageId) {
    return Status::Error(StatusCode::kPageFull,
                         "neither half can hold the entry after the split: entry too large, overflow-page support required (not implemented)");
  }

  // target 可能就是 pid 本身：此时 = 取回同一个帧（pin 计数 +1），改的仍是同一块内存。
  auto tg = Fetch(target);
  if (!tg.ok()) {
    return tg.status();
  }
  PageGuard target_guard = std::move(tg.value());
  auto r = page_ops->InsertLeafEntry(target_guard.mutable_header(), target_guard.mutable_payload(),
                                     elem);
  if (!r.ok()) {
    return Status::Error(r.status().code(),
                         std::string("re-inserting after the split still failed: ") + r.status().message());
  }
  if (r.value() == LeafInsertResult::kInserted) {
    ++num_entries;
  }
  if (leaf_out != nullptr) {
    *leaf_out = target;  // 记录落在被选中的那一半（可能就是原页）
  }
  target_guard.Release();
  guard.Release();

  *split_out = true;
  *new_pid_out = new_pid;
  return Status::Ok();
}

Status IndexTree::Insert(const IndexEntry& entry) { return InsertReturningLeaf(entry, nullptr); }

Status IndexTree::InsertReturningLeaf(const IndexEntry& entry, PageId* out_leaf) {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "Insert called before Open");
  }
  if (out_leaf != nullptr) {
    *out_leaf = kInvalidPageId;
  }
  // C1：整段插入都在写路径上 —— 这之后的所有 Fetch 默认拿独占页闩。
  Impl::WritePathScope wp(&im);
  // 一次 Insert = 一个事务。所有提前返回由析构兜底 Abort，成功路径显式 Commit。
  ScopedWriteOp op(im.pool);
  im.RegisterRollback();  // 必须在改动任何东西之前（见该函数的说明）
  TreeEntry elem;
  LTM_RETURN_IF_ERROR(im.page_ops->EncodeEntry(entry, &elem));
  LTM_RETURN_IF_ERROR(CheckEntryFits(elem));

  bool split = false;
  PageId new_pid = kInvalidPageId;
  LTM_RETURN_IF_ERROR(im.InsertRec(im.root, entry, elem, &split, &new_pid, out_leaf));

  if (!split) {
    LTM_RETURN_IF_ERROR(im.PersistMeta());
    return op.Commit();
  }

  // 新根的两个子页分隔条目：**两个**都由子页现状推导（单一来源）。
  // 旧根那一条也必须这样推导，而不是"反正有最左哨兵、键会被改写、随便给个空条目"——
  // 对没有哨兵的树（多维树）0 号槽是真条目，它的 MBR 必须落地，否则 I9 当场被破坏。
  // B+ 不受影响：InsertChildAt 在 at==0 且 HasLeftmostSentinel() 时会把键改写为哨兵。
  TreeEntry sep_old;
  TreeEntry sep_new;
  {
    auto og = im.Fetch(im.root);
    if (!og.ok()) {
      return og.status();
    }
    sep_old = im.page_ops->BuildSeparator(og.value().header(), og.value().payload());
  }
  {
    auto np = im.Fetch(new_pid);
    if (!np.ok()) {
      return np.status();
    }
    sep_new = im.page_ops->BuildSeparator(np.value().header(), np.value().payload());
  }

  // 根分裂：新根 = <旧根的分隔条目, 旧根> + <sep_new, 新页>；
  // 旧根与新页同层（原高度），新根 = 原高度 + 1
  const uint8_t new_height = static_cast<uint8_t>(im.height + 1);
  PageId new_root = kInvalidPageId;
  auto g = im.pool->NewPage(im.page_ops->PageTypeForLevel(new_height), im.tree_id, new_height,
                            &new_root);
  if (!g.ok()) {
    return g.status();
  }
  {
    PageGuard guard = std::move(g.value());
    PageHeader* h = guard.mutable_header();
    char* payload = guard.mutable_payload();
    LTM_RETURN_IF_ERROR(im.page_ops->InitPage(h, payload, false));
    h->flags |= kPageFlagRoot;
    // 0 号槽的键（与 MBR）由页算子决定：有最左哨兵的树写哨兵值，多维树写自己的
    // 分隔条目。两者都来自 BuildSeparator(旧根)，不存在第二处推导。
    LTM_RETURN_IF_ERROR(im.page_ops->InsertChildAt(h, payload, 0, sep_old));
    LTM_RETURN_IF_ERROR(im.page_ops->InsertChildAt(h, payload, 1, sep_new));
  }
  {
    // 重新 Fetch 旧根（可能已被淘汰），把它降为"原高度"那一层
    auto old_root = im.Fetch(im.root);
    if (!old_root.ok()) {
      return old_root.status();
    }
    PageHeader* oh = old_root.value().mutable_header();
    oh->flags &= static_cast<uint8_t>(~kPageFlagRoot);
    oh->level = static_cast<uint8_t>(new_height - 1);
  }

  {
    // C1：root 与 height 必须一起改（读者把它们当成一致的一对来快照，见 structure_mutex）。
    std::unique_lock<std::shared_mutex> sl(im.structure_mutex);
    im.root = new_root;
    im.height = new_height;
  }
  LTM_RETURN_IF_ERROR(im.PersistMeta());
  LOG_INFO("index tree grew: height=", static_cast<int>(im.height), " root=", im.root);
  return op.Commit();
}

// ---------------------------------------------------------------------------
// 写入：删除
// ---------------------------------------------------------------------------
Status IndexTree::Impl::EraseRec(PageId pid, const IndexEntry& entry, bool is_root,
                                 bool* underflow_out, bool* erased_out) {
  *underflow_out = false;
  *erased_out = false;
  auto g = Fetch(pid);
  if (!g.ok()) {
    return g.status();
  }
  PageGuard guard = std::move(g.value());

  if (guard.header().level == 0) {
    // 叶内定位是页内语义：一维树按键，多维树按 (键, MBR) 双匹配。
    // 不能假定叶内有序 —— 多维树的叶内条目按分裂结果排列，与路由标签无关。
    const SlotId slot = page_ops->FindEntry(guard.header(), guard.payload(), entry);
    guard.Release();
    if (slot == kInvalidSlot) {
      return Status::Ok();  // 不在这棵子树里；由调用方决定要不要试下一个候选
    }
    bool uf = false;
    LTM_RETURN_IF_ERROR(EraseLeafSlotAt(pid, slot, &uf));
    *erased_out = true;
    if (!is_root && uf) {
      *underflow_out = true;
    }
    return Status::Ok();
  }

  // 候选孩子：一维树恰好一个；多维树的 MBR 可能重叠，同一条记录可能在多个
  // 候选里，必须逐个尝试（单路径下钻会静默漏删）。
  auto cand = page_ops->CandidateChildSlots(guard.header(), guard.payload(), entry,
                                            SlotChoice::kSearch);
  if (!cand.ok()) {
    return cand.status();
  }
  std::vector<std::pair<SlotId, PageId>> candidates;
  for (SlotId s : cand.value()) {
    if (s >= guard.page().num_slots()) {
      continue;
    }
    const PageId c = page_ops->GetChild(guard.header(), guard.payload(), s);
    if (c != kInvalidPageId) {
      candidates.emplace_back(s, c);
    }
  }
  guard.Release();
  if (candidates.empty()) {
    // 「这棵子树里没有」**不是错误**，只是"不在这里"。返回 Ok 并让 erased_out
    // 保持 false，由调用方决定是试下一个候选还是最终报 kNotFound。
    //
    // 早期版本这里返回 kNotFound，配合下面递归处的 LTM_RETURN_IF_ERROR，
    // 就成了一条静默的**提前退出**：R 树的 MBR 重叠天然会产生假阳性候选，
    // 只要先探到的那条路走到底没有候选，整次删除就被误报成"记录不存在"，
    // 而真正装着记录的那个兄弟候选根本没被访问。症状极具迷惑性 ——
    // Validate 全绿、ScanAll 与真值表逐条一致、计数也对，只有 Erase 说不存在。
    return Status::Ok();
  }

  for (const std::pair<SlotId, PageId>& cand_item : candidates) {
    const SlotId child_slot = cand_item.first;
    const PageId child = cand_item.second;
    bool child_underflow = false;
    bool erased = false;
    const Status rs = EraseRec(child, entry, false, &child_underflow, &erased);
    if (rs.code() == StatusCode::kNotFound) {
      // 子树内部的"死胡同"同上：必须继续试兄弟候选，不能中断。
      continue;
    }
    LTM_RETURN_IF_ERROR(rs);
    if (!erased) {
      continue;
    }
    // 【就地置位，不能挪到函数末尾】下面的"子页没有下溢"分支会提前 return，
    // 若把 *erased_out = true 放在末尾，一次**已经成功**的删除会被当成
    // "记录不存在"上报给上层，整条祖先链继续上报 kNotFound —— 而记录其实
    // 已经从叶页消失。症状极具迷惑性：Search 命中、ScanAll 与真值表完全一致、
    // num_entries 也对，只有 Erase 报"记录不存在"。
    *erased_out = true;

    // 不变量 I1/I9：父页分隔条目恒等于子页现状。本次删除若删掉的正是子页
    // （或子页左侧路径上）的最小键，父条目就会变陈旧，必须跟着刷新。
    //
    // 注意这里**不区分是否下溢**都要刷新：随后的借键/合并并不保证覆盖到这个槽
    // （例如"向右兄弟借键"只会刷新供体那一槽，而目标槽的键在上一步删除时就已经
    // 陈旧了）。刷新必须在子页还在的时候做，所以放在 Rebalance 之前。
    LTM_RETURN_IF_ERROR(SyncSeparatorForChild(pid, child));
    if (!child_underflow) {
      return Status::Ok();
    }
    LTM_RETURN_IF_ERROR(Rebalance(pid, child_slot));
    // Rebalance 的合并分支会从本页删掉一个槽，本页可能因此自己下溢；
    // 必须把这一点报给上层，否则整条祖先链都不会再做借键/合并，
    // 树会停在一个"内部页填充不足"的状态（I3 直接报错）。
    if (!is_root) {
      auto self = Fetch(pid);
      if (!self.ok()) {
        return self.status();
      }
      if (page_ops->IsUnderflow(self.value().header(), self.value().payload())) {
        *underflow_out = true;
      }
    }
    return Status::Ok();
  }
  return Status::Ok();
}

Status IndexTree::Erase(const IndexEntry& entry) {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "Erase called before Open");
  }
  // C1：整段删除都在写路径上（Fetch 默认拿独占页闩）。
  Impl::WritePathScope wp(&im);
  ScopedWriteOp op(im.pool);
  im.RegisterRollback();  // 同上：改动前注册
  bool underflow = false;
  bool erased = false;
  LTM_RETURN_IF_ERROR(im.EraseRec(im.root, entry, /*is_root=*/true, &underflow, &erased));
  if (!erased) {
    return NotFound("record not found: key=" + std::to_string(entry.key) +
                    (entry.dim() > 0 ? " (matched exactly on the MBR as well)" : ""));
  }

  // 根降级：根只剩一个子页时把它提为新根
  if (im.height > 0) {
    auto g = im.Fetch(im.root);
    if (!g.ok()) {
      return g.status();
    }
    if (g.value().page().num_slots() <= 1 && g.value().page().header().level > 0) {
      const PageId only_child =
          im.page_ops->GetChild(g.value().header(), g.value().payload(), 0);
      const PageId old_root = im.root;
      g.value().mutable_header()->flags &= static_cast<uint8_t>(~kPageFlagRoot);
      g.value().Release();
      if (only_child == kInvalidPageId) {
        return Status::Error(StatusCode::kCorruption, "invalid child page while demoting the root");
      }
      {
        // C1：root 与 height 一起改（与根分裂同一口径，见 structure_mutex）。
        std::unique_lock<std::shared_mutex> sl(im.structure_mutex);
        im.root = only_child;
        --im.height;
      }
      LTM_RETURN_IF_ERROR(im.PersistMeta());
      LTM_RETURN_IF_ERROR(im.pool->FreePage(old_root));
      auto ng = im.Fetch(im.root);
      if (ng.ok()) {
        ng.value().mutable_header()->flags |= kPageFlagRoot;
      }
      LOG_INFO("index tree root demoted: height=", static_cast<int>(im.height), " root=", im.root);
    }
  }
  LTM_RETURN_IF_ERROR(im.PersistMeta());
  return op.Commit();
}

// ---------------------------------------------------------------------------
// 下溢处理
// ---------------------------------------------------------------------------
// 让"父页第 child_slot 槽指向的子页"不再下溢（不变量 I3）。
//
// 【为什么要循环】单轮处理只做一件事：借一条、或者合并一次。但两种情况下单轮都不够：
//   * 下溢页可能远低于阈值（合并会一次从父页抽走一个槽，实测容量 32 时出现过只剩
//     1 条槽的内部页），只借一条根本补不满；
//   * 两个都很小的页合并之后，结果仍可能低于阈值。
// 所以反复调用单轮处理，直到子页满足填充率、或它已经没有兄弟可以借/并（此时把
// 下溢交给上层 —— 父页自己会不会下溢由 EraseRec 的 underflow_out 继续上报）。
Status IndexTree::Impl::Rebalance(PageId parent_pid, SlotId child_slot) {
  for (int round = 0; round < 4096; ++round) {
    bool progress = false;
    LTM_RETURN_IF_ERROR(RebalanceOnce(parent_pid, &child_slot, &progress));
    auto pg = Fetch(parent_pid);
    if (!pg.ok()) {
      return pg.status();
    }
    if (child_slot >= pg.value().page().num_slots()) {
      return Status::Ok();  // 槽已在本轮合并中消失
    }
    const PageId child_pid =
        page_ops->GetChild(pg.value().header(), pg.value().payload(), child_slot);
    pg.value().Release();
    auto cg = Fetch(child_pid);
    if (!cg.ok()) {
      return cg.status();
    }
    const bool underflow = page_ops->IsUnderflow(cg.value().header(), cg.value().payload());
    cg.value().Release();
    if (!underflow) {
      return Status::Ok();
    }
    if (!progress) {
      return Status::Ok();  // 既借不动也并不了：交给上层
    }
  }
  return Status::Error(StatusCode::kCorruption, "Rebalance exceeded its round limit (structural corruption suspected)");
}

Status IndexTree::Rebalance(PageId parent_pid, SlotId child_slot) {
  return impl_->Rebalance(parent_pid, child_slot);
}

// 借键的一侧：把 donor_pid 的一条条目搬进 child_pid，循环到 child 不再下溢
// 或 donor 没有余量为止。返回 true 表示至少搬动过一条。
//
// 方向由 BorrowDirection 承载（缺陷 #18：方向搞反会把键序弄反）。
// 四种情形（左/右 × 叶/内部）的布局手术全部下沉在页算子 BorrowFrom 里 ——
// 尤其 B+ 内部页借键要重建"最左子页哨兵对"，那是页算子才知道的槽约定。
Status IndexTree::Impl::BorrowLoop(PageId parent_pid, PageId child_pid, PageId donor_pid,
                                   BorrowDirection dir, uint16_t min_fill, bool* progress_out) {
  *progress_out = false;
  for (;;) {
    auto cg = Fetch(child_pid);
    auto dg = Fetch(donor_pid);
    if (!cg.ok() || !dg.ok()) {
      return Status::Ok();  // 读不到页面就放弃借键，交给合并路径
    }
    PageGuard child = std::move(cg.value());
    PageGuard donor = std::move(dg.value());
    if (!page_ops->IsUnderflow(child.header(), child.payload())) {
      return Status::Ok();  // 已经补足
    }
    if (child.header().level != donor.header().level) {
      return Status::Ok();
    }
    // 供体条件：**借走一条之后供体自己**不能变成下溢（num_slots > 阈值）。
    // 早期版本只要求"供体当前不下溢"，于是容量 8、阈值 4 时供体 4 → 3，
    // 借键把供体弄成下溢页，Validate 的 I3 立刻报"填充不足"（缺陷 #17）。
    if (donor.page().num_slots() <= min_fill) {
      return Status::Ok();  // 供体已无余量
    }
    // 父页中原本路由到 donor 的那个分隔条目：多维树内部页借键时要拿它当转移键，
    // 一维树叶页与多维树都忽略它。由 donor 现状推导（单一来源）。
    const TreeEntry parent_sep =
        page_ops->BuildSeparator(donor.header(), donor.payload());
    TreeEntry moved;
    Page cp = child.mutable_page();
    Page dp = donor.mutable_page();
    if (!page_ops->BorrowFrom(cp.mutable_header(), cp.mutable_payload(), &dp, dir, parent_sep,
                              &moved)) {
      return Status::Ok();  // 供体那一条不可借（如内部页哨兵槽）
    }
    ++stats.num_borrows;
    *progress_out = true;
    // 【两侧都要同步】一维树里只有一侧的分隔条目会变（借左兄弟 → 接收方最小键变小；
    // 借右兄弟 → 供体最小键变大），但多维树里借键让**两个方向的 MBR 并集同时变化**。
    // 统一无条件同步两侧；不需要改的那一侧会被 SeparatorEquals 判等跳过，
    // 因此对一维树既不多余、也不改变行为。
    LTM_RETURN_IF_ERROR(SyncSeparatorForChild(parent_pid, child_pid));
    LTM_RETURN_IF_ERROR(SyncSeparatorForChild(parent_pid, donor_pid));
  }
}

// 单轮处理：借一条（优先左兄弟）或合并一次。
// *child_slot_io 在合并后会更新为"合并后仍然存活的那个页"所在的槽。
Status IndexTree::Impl::RebalanceOnce(PageId parent_pid, SlotId* child_slot_io,
                                      bool* progress_out) {
  *progress_out = false;
  const SlotId child_slot = *child_slot_io;
  auto pg = Fetch(parent_pid);
  if (!pg.ok()) {
    return pg.status();
  }
  PageGuard parent = std::move(pg.value());
  const uint16_t n = parent.page().num_slots();
  if (n == 0) {
    return Status::Error(StatusCode::kCorruption, "the parent page has no entries");
  }
  if (child_slot >= n) {
    return Status::Ok();
  }
  const SlotId left_slot = (child_slot == 0) ? kInvalidSlot : static_cast<SlotId>(child_slot - 1);
  const SlotId right_slot = (child_slot + 1 < n) ? static_cast<SlotId>(child_slot + 1)
                                                 : kInvalidSlot;
  const PageId child_pid = page_ops->GetChild(parent.header(), parent.payload(), child_slot);
  const uint16_t min_fill = page_ops->UnderflowThreshold();

  // 内部页的 0/1 号槽是一对冗余引用（都指向最左子页），所以"相邻槽"未必是兄弟：
  // 当 child_slot == 1 且 0 号槽指向同一个子页时，那个槽不是左兄弟。
  // 若不排除，借键会"借自己"（把本页最大条目复制一份再删原件），
  // 合并会"把自己并进自己"（MergeFrom 自追加，实测叶页键被整块复制一遍：
  // keys={2897,...,2932, 2897,...,2932}，随后父页键序校验直接报错）。
  const bool has_left =
      left_slot != kInvalidSlot &&
      page_ops->GetChild(parent.header(), parent.payload(), left_slot) != child_pid;
  const bool has_right =
      right_slot != kInvalidSlot &&
      page_ops->GetChild(parent.header(), parent.payload(), right_slot) != child_pid;

  // 1) 向左兄弟借**末**条目（借来的键比 child 现有全部键都小，插到 child 开头）
  if (has_left) {
    const PageId left_pid = page_ops->GetChild(parent.header(), parent.payload(), left_slot);
    bool progressed = false;
    LTM_RETURN_IF_ERROR(BorrowLoop(parent_pid, child_pid, left_pid, BorrowDirection::kFromLeft,
                                   min_fill, &progressed));
    if (progressed) {
      *progress_out = true;
    }
    auto done = Fetch(child_pid);
    if (done.ok() && !page_ops->IsUnderflow(done.value().header(), done.value().payload())) {
      return Status::Ok();
    }
  }

  // 2) 向右兄弟借**首**条目（借来的键比 child 现有全部键都大，追加到末尾）
  if (has_right) {
    const PageId right_pid = page_ops->GetChild(parent.header(), parent.payload(), right_slot);
    bool progressed = false;
    LTM_RETURN_IF_ERROR(BorrowLoop(parent_pid, child_pid, right_pid, BorrowDirection::kFromRight,
                                   min_fill, &progressed));
    if (progressed) {
      *progress_out = true;
    }
    auto done = Fetch(child_pid);
    if (done.ok() && !page_ops->IsUnderflow(done.value().header(), done.value().payload())) {
      return Status::Ok();
    }
  }

  // 3) 合并：优先把右并入左
  SlotId left_of_pair;
  SlotId right_of_pair;
  if (has_left) {
    left_of_pair = left_slot;
    right_of_pair = child_slot;
  } else if (has_right) {
    left_of_pair = child_slot;
    right_of_pair = right_slot;
  } else {
    return Status::Ok();  // 父页只有这一个子页，交给上层做根降级
  }

  const PageId left_pid = page_ops->GetChild(parent.header(), parent.payload(), left_of_pair);
  const PageId right_pid = page_ops->GetChild(parent.header(), parent.payload(), right_of_pair);
  // 父页中位于两页之间、原本路由到 donor（右页）的分隔条目键。
  // B+ 内部页合并必须把它下压成新条目的键（父页少一个键、子页多一个键，键数守恒）；
  // 多维树内部页是"N 条目对 N 子页"、没有哨兵，该条目直接删除、不下压。
  // 怎么用由页算子决定，通用层只负责传 —— 所以这里不得假设键数守恒。
  const ElemKey sep = page_ops->GetKey(parent.header(), parent.payload(), right_of_pair);

  auto lg = Fetch(left_pid);
  if (!lg.ok()) {
    return lg.status();
  }
  PageGuard left = std::move(lg.value());
  auto rg = Fetch(right_pid);
  if (!rg.ok()) {
    return rg.status();
  }
  PageGuard right = std::move(rg.value());

  // 合并要把右页条目写进左页：必须用 mutable_page() 标脏，
  // 否则这批修改不会被写盘（重开丢数据），校验和也不会重算。
  Page lp = left.mutable_page();
  const Status merged =
      page_ops->MergeFrom(lp.mutable_header(), lp.mutable_payload(), right.page(), sep);
  if (!merged.ok()) {
    return Status::Error(merged.code(),
                         std::string("merge failed (a smaller capacity may be needed): ") + merged.message());
  }
  ++stats.num_merges;

  if (left.page().header().level == 0) {
    // 【摘链必须按**叶链**做，不能按父页槽位】
    // 这里的 left/right 是父页上相邻的两槽，但"父页槽位相邻" **不等于**
    // "叶链相邻"。B+ 两者恰好一致（都是有键序的），多维树不是 ——
    // 内部页借键会把一个子页从一棵父页的槽位搬到另一棵父页的槽位，
    // 该子页的叶链位置却纹丝不动。此时若照搬 `left->next = right->next`，
    // 就会把 left 与 right 之间那一整段叶页从链上摘掉：
    //   ... → left → [被跳过的一段] → right → after ...
    // 变成 ... → left → after，而 [被跳过的一段] 末端仍指向 right（已释放、
    // 可能已被重新分配），链就此缠成一团。症状是 ScanAll 突然少掉几百条，
    // 而 Validate 全绿（I9 与链序无关），Erase/Search 也照常工作。
    //
    // 正确做法：用 right 自己记录的 prev_page 找它在链上的真前驱（O(1)，
    // 每个 split/merge 都在维护它）。B+ 恒走快路径，行为与改动前逐位一致。
    const PageId after = right.page().header().next_page;
    const PageId chain_prev = right.page().header().prev_page;
    if (chain_prev == left_pid) {
      left.mutable_header()->next_page = after;  // 快路径：left 本来就是链上前驱
    } else if (chain_prev == kInvalidPageId) {
      leftmost_leaf = after;  // right 是链头，摘掉后由 after 接任
    } else {
      auto cg = Fetch(chain_prev);
      if (!cg.ok()) {
        return cg.status();
      }
      cg.value().mutable_header()->next_page = after;
    }
    if (after != kInvalidPageId) {
      auto ag = Fetch(after);
      if (!ag.ok()) {
        return ag.status();
      }
      ag.value().mutable_header()->prev_page = chain_prev;
    }
    if (leftmost_leaf == right_pid) {
      leftmost_leaf = left_pid;  // 防御：链头记录与 prev_page 不一致时兜底
    }
  }

  // 顺序不可颠倒：先更新父页、再释放右页（不变量 I7）
  parent.mutable_page().EraseEntry(right_of_pair);
  parent.mutable_page().Compact();
  left.Release();
  right.Release();
  // 合并后左页的分隔条目 = 左页现状（单一来源）。
  // 它可能同时改变了父页里指向左页的**每一槽**（B+ 的哨兵对），所以走全槽刷新。
  LTM_RETURN_IF_ERROR(SyncSeparatorForChild(parent_pid, left_pid));
  LTM_RETURN_IF_ERROR(pool->FreePage(right_pid));

  // 合并可能仍然没把页补到阈值以上（两个很小的页并起来依旧很小），
  // 也可能父页因此少了一个槽 —— 两种情况都交给外层循环继续处理，
  // 并把"存活下来的那个页"所在的槽回填给调用方。
  *progress_out = true;
  *child_slot_io = left_of_pair;
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------
Status IndexTree::Validate() {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "Validate called before Open");
  }
  if (im.root == kInvalidPageId) {
    return Status::Error(StatusCode::kCorruption, "root is invalid");
  }

  struct Item {
    PageId pid;
    uint8_t expected_level;
    PageId parent;  // 便于区分"同父页的合法重复引用"与"跨父页的环"
  };
  std::vector<Item> stack;
  std::unordered_map<PageId, PageId> parent_of;
  std::vector<PageId> order;
  std::unordered_set<PageId> seen;
  uint64_t pages = 0;
  uint64_t leaf_pages = 0;
  uint64_t entries = 0;
  im.acc = PageStatsAccumulator{};

  stack.push_back(Item{im.root, im.height, kInvalidPageId});
  size_t cursor = 0;
  while (cursor < stack.size()) {
    const Item it = stack[cursor++];
    if (!seen.insert(it.pid).second) {
      // 同一页被再次访问可能是合法重复，也可能是结构损坏，必须区分：
      //   * 合法（仅限有最左哨兵的树）：**同一个父页**的 0 号槽（最左子页哨兵）与
      //     1 号槽可以指向同一个子页 —— 哨兵的键无意义，而 1 号槽持该子页的最小键，
      //     两个槽引用同一子页是正常形态，不是环。
      //   * 损坏：不同父页引用同一子页（真环/串链）；
      //     多维树没有哨兵，任何重复引用都是损坏。
      if (im.page_ops->HasLeftmostSentinel()) {
        auto prev = parent_of.find(it.pid);
        if (prev != parent_of.end() && prev->second == it.parent) {
          continue;
        }
      }
      return Status::Error(StatusCode::kCorruption,
                           "page " + std::to_string(it.pid) + " is referenced by multiple parents (cycle or chain)");
    }
    parent_of[it.pid] = it.parent;
    auto g = im.Fetch(it.pid);
    if (!g.ok()) {
      return g.status();
    }
    PageGuard guard = std::move(g.value());
    ++pages;
    if (guard.header().level != it.expected_level) {
      return Status::Error(StatusCode::kCorruption,
                           "page " + std::to_string(it.pid) + " level=" +
                               std::to_string(guard.header().level) + " != expected " +
                               std::to_string(it.expected_level));
    }
    LTM_RETURN_IF_ERROR(im.page_ops->ValidatePage(guard.header(), guard.payload()));
    im.page_ops->AccumulateOverlap(guard.header(), guard.payload(), &im.acc.overlap_sum,
                                   &im.acc.overlap_count);
    order.push_back(it.pid);

    if (guard.header().level == 0) {
      ++leaf_pages;
      // 按存活槽计数：墓碑不是条目（缺陷 #28 的同类问题：把墓碑当条目会把
      // 计数与键序检查一起带偏）。用 IsTombstone 而不是自己判 s.len == 0，
      // 让"什么算墓碑"由页算子定义。
      for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
        if (!im.page_ops->IsTombstone(guard.header(), guard.payload(), i)) {
          ++entries;
        }
      }
      continue;
    }
    // 内部页至少要有一个真实条目（有哨兵的树是 2 槽：哨兵 + 1）。
    if (guard.page().num_slots() < 2) {
      return Status::Error(StatusCode::kCorruption,
                           "internal page " + std::to_string(it.pid) + " slot count < 2 (should have been demoted)");
    }
    for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
      if (im.page_ops->IsTombstone(guard.header(), guard.payload(), i)) {
        continue;
      }
      const PageId c = im.page_ops->GetChild(guard.header(), guard.payload(), i);
      if (c == kInvalidPageId) {
        return Status::Error(StatusCode::kCorruption, "an internal page has an illegal child");
      }
      auto cg = im.Fetch(c);
      if (!cg.ok()) {
        return cg.status();
      }
      PageGuard child = std::move(cg.value());
      LTM_RETURN_IF_ERROR(
          im.page_ops->CheckParentKey(guard.header(), guard.payload(), i, child.page()));
      stack.push_back(Item{c, static_cast<uint8_t>(guard.header().level - 1), it.pid});
    }
  }

  // 叶链：必须覆盖全部叶页、双向一致；键严格升序（仅对**叶内有序**的树断言）
  if (im.page_ops->HasLeafChain() && im.leftmost_leaf != kInvalidPageId) {
    const bool ordered = im.page_ops->KeysAreOrdered();
    ElemKey prev_key = kMinElemKey;
    bool has_prev = false;
    PageId cur = im.leftmost_leaf;
    PageId last = kInvalidPageId;
    uint64_t walked = 0;
    while (cur != kInvalidPageId) {
      auto g = im.Fetch(cur);
      if (!g.ok()) {
        return g.status();
      }
      PageGuard guard = std::move(g.value());
      if (guard.header().level != 0) {
        return Status::Error(StatusCode::kCorruption, "a non-leaf page appeared on the leaf chain");
      }
      if (guard.header().prev_page != last) {
        return Status::Error(StatusCode::kCorruption,
                             "leaf chain prev pointer is inconsistent (page " + std::to_string(cur) + ")");
      }
      if (ordered) {
        for (uint16_t i = 0; i < guard.page().num_slots(); ++i) {
          // 墓碑槽不是条目（GetKey 会把它读成 kMinElemKey），必须跳过，
          // 否则页中间的墓碑会让"键严格升序"检查误报损坏。
          if (im.page_ops->IsTombstone(guard.header(), guard.payload(), i)) {
            continue;
          }
          const ElemKey k = im.page_ops->GetKey(guard.header(), guard.payload(), i);
          if (has_prev && k <= prev_key) {
            return Status::Error(StatusCode::kCorruption,
                                 "leaf chain key order is broken: " + std::to_string(prev_key) + " -> " +
                                     std::to_string(k));
          }
          prev_key = k;
          has_prev = true;
        }
      }
      last = cur;
      cur = guard.header().next_page;
      ++walked;
    }
    if (walked != leaf_pages) {
      return Status::Error(StatusCode::kCorruption,
                           "leaf chain page count " + std::to_string(walked) + " != the number of leaf pages in the tree " +
                               std::to_string(leaf_pages));
    }
  }

  // 条目总数与元数据计数必须一致。**这一条不属于叶链检查** —— 没有叶链的树
  // 也必须有此不变量，早期把它嵌在叶链分支里，于是无边树的计数错误被静默跳过。
  if (entries != im.num_entries) {
    return Status::Error(StatusCode::kCorruption,
                         "total entries in pages " + std::to_string(entries) + " != metadata count " +
                             std::to_string(im.num_entries));
  }

  // 非根页不得下溢。阈值口径由页算子自己定义（IsUnderflow），
  // 通用层**不**重算一遍 —— 两处各算一次就会出现"通用层认为没下溢、
  // 页算子认为下溢"，表现为 Rebalance 不进展 + Validate 误报损坏。
  for (PageId pid : order) {
    if (pid == im.root) {
      continue;
    }
    auto g = im.Fetch(pid);
    if (!g.ok()) {
      return g.status();
    }
    if (im.page_ops->IsUnderflow(g.value().header(), g.value().payload())) {
      // 诊断信息必须够用：只说"存活 15 条"无法区分是叶页还是内部页、是分裂分歪了
      // 还是删除没触发再平衡 —— level/槽数/叶链邻居三个字段就能定位到具体成因。
      const PageHeader& h = g.value().header();
      return Status::Error(StatusCode::kCorruption,
                           "page " + std::to_string(pid) + " underfull: level=" +
                               std::to_string(static_cast<int>(h.level)) + " slot " +
                               std::to_string(h.num_slots) + " live " +
                               std::to_string(pageutil::LiveSlots(h, g.value().payload())) +
                               " entries, threshold " + std::to_string(im.page_ops->UnderflowThreshold()) +
                               " (leaf chain prev=" + std::to_string(h.prev_page) + " next=" +
                               std::to_string(h.next_page) + ", root=" + std::to_string(im.root) +
                               " height=" + std::to_string(im.height) + ")");
    }
  }

  auto rg = im.Fetch(im.root);
  if (!rg.ok()) {
    return rg.status();
  }
  if (rg.value().page().header().level != im.height) {
    return Status::Error(StatusCode::kCorruption, "the root page level disagrees with the metadata height");
  }

  im.stats.height = im.height;
  im.stats.num_pages = pages;
  im.stats.num_entries = im.num_entries;
  im.stats.root_page = im.root;
  im.stats.order_or_capacity = im.capacity();
  im.stats.dim = im.options.mbr_dim;
  im.stats.avg_fill_rate =
      (pages > 0) ? static_cast<double>(entries) / (static_cast<double>(pages) * im.capacity())
                  : 0.0;
  im.stats.avg_overlap_area = (im.acc.overlap_count > 0)
                                  ? im.acc.overlap_sum / static_cast<double>(im.acc.overlap_count)
                                  : 0.0;
  return Status::Ok();
}

TreeStats IndexTree::stats() const {
  Impl& im = *impl_;
  TreeStats s = im.stats;
  s.height = im.height;
  s.num_entries = im.num_entries;
  s.root_page = im.root;
  s.order_or_capacity = im.capacity();
  s.dim = im.options.mbr_dim;
  if (s.num_pages == 0) {
    s.num_pages = (im.num_entries == 0) ? 1 : 0;
  }
  return s;
}

Status IndexTree::Destroy() {
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "Destroy called before Open");
  }
  // Destroy 也是结构修改：FreePage 会改页头与页 0 的空闲链，同样要有 REDO 覆盖，
  // 否则恢复出来的空闲链与实际页布局对不上（页泄漏或双分配，不变量 I7）。
  ScopedWriteOp op(im.pool);
  im.RegisterRollback();  // 同上：改动前注册
  std::vector<PageId> stack;
  if (im.root != kInvalidPageId) {
    stack.push_back(im.root);
  }
  std::unordered_set<PageId> seen;
  std::vector<PageId> order;
  while (!stack.empty()) {
    const PageId pid = stack.back();
    stack.pop_back();
    if (pid == kInvalidPageId || !seen.insert(pid).second) {
      continue;
    }
    order.push_back(pid);
    auto g = im.Fetch(pid);
    if (!g.ok()) {
      return g.status();
    }
    if (g.value().page().header().level > 0) {
      for (uint16_t i = 0; i < g.value().page().num_slots(); ++i) {
        // 空槽（墓碑）必须跳过：GetChild 对空槽返回 kInvalidPageId，
        // 若把它入栈，下一轮 Fetch(kInvalidPageId) 会触发"页必须自报家门"断言
        // 而把一个正常的销毁过程报成损坏（缺陷 #28）。
        if (im.page_ops->IsTombstone(g.value().header(), g.value().payload(), i)) {
          continue;
        }
        const PageId c = im.page_ops->GetChild(g.value().header(), g.value().payload(), i);
        if (c != kInvalidPageId) {
          stack.push_back(c);
        }
      }
    }
  }
  for (PageId pid : order) {
    LTM_RETURN_IF_ERROR(im.pool->FreePage(pid));
  }
  im.root = kInvalidPageId;
  im.leftmost_leaf = kInvalidPageId;
  im.height = 0;
  im.num_entries = 0;
  im.stats = TreeStats{};
  im.acc = PageStatsAccumulator{};
  LTM_RETURN_IF_ERROR(im.PersistMeta());
  return op.Commit();
}

}  // namespace ltm

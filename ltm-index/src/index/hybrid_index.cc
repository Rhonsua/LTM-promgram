#include "index/hybrid_index.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "index/bplus_tree.h"
#include "index/rtree.h"
#include "util/log.h"

namespace ltm {

// ===========================================================================
// M5：混合索引
//
// 三条设计决定（都在 docs/status.md 的 M5 一节有实测依据）：
//
// 1. **两棵树一个事务**。Put/Erase 用 `ScopedWriteOp` 罩住两棵树的写入 ——
//    ScopedWriteOp 可重入，而 RTree::Insert 内部还会开一层（它的 upsert 要先删后插），
//    三层都 join 进同一个 txn_id，只有最外层 Commit 才写 kCommit 记录。
//    这正是 M4-2 把事务放进 BufferPool（而不是放进某棵树）的原因。
//
// 2. **B+ 叶条目里存 R 树叶页号，但它只是提示**。Get/Erase 先按提示页去读，
//    校验不过（页不是本树的 R 树叶页、或页里没有这条键）就退化为叶链全扫。
//    R 树的借键与合并会把记录搬到兄弟页并释放原页，原页号随后可能被分配给
//    别的树 —— 把提示当权威会读到别的树的内容。提示的有效率有计数器。
//
// 3. **一维查询走 B+，空间/KNN 走 R**。两棵树的键空间不同：B+ 的键是逻辑主键，
//    R 树的路由键是 MBR 的 Z 序编码，因此"按键范围"只能由 B+ 回答；
//    反过来 R 树的 ScanAll 不按键有序，用它做一维范围查询要全扫。
//    所以 Query 按 kind 路由，而不是让一棵树勉强回答另一棵树的问题。
// ===========================================================================

namespace {

struct HeldApi { const void* index; bool exclusive; };
thread_local std::vector<HeldApi> held_apis;

// A whole logical operation sees both trees between committed writes. Nested
// read-only callback calls reuse the outer guard; read-to-write upgrades fail.
class ApiGuard {
 public:
  ApiGuard(std::shared_mutex& mutex, const void* index, bool exclusive)
      : writer_(mutex, std::defer_lock), reader_(mutex, std::defer_lock) {
    for (const auto& held : held_apis) {
      if (held.index == index) {
        ok_ = !exclusive || held.exclusive;
        return;
      }
    }
    if (exclusive) writer_.lock(); else reader_.lock();
    held_apis.push_back({index, exclusive});
    registered_ = true;
  }
  ~ApiGuard() { if (registered_) held_apis.pop_back(); }
  bool ok() const { return ok_; }
  ApiGuard(const ApiGuard&) = delete;
  ApiGuard& operator=(const ApiGuard&) = delete;
 private:
  std::unique_lock<std::shared_mutex> writer_;
  std::shared_lock<std::shared_mutex> reader_;
  bool registered_ = false;
  bool ok_ = true;
};

Status ReadUpgradeError() {
  return InvalidArgument("a read callback cannot mutate, close, checkpoint or validate its index");
}

// 把 Config 里的迁移参数显式搬到迁移层。
// BufferPoolOptions 里也有一份冷热参数（存储层自身的缺省值），组装时以
// MigrationPolicyOptions 为准，避免"两处各改一半"。
MigrationPolicyOptions MigrationOptionsFrom(const Config& cfg) { return cfg.migration; }

// ---------------------------------------------------------------------------
// B+ 侧记录的编码：`dim | payload_len | lo[dim] | hi[dim] | payload`
//
// 【为什么 MBR 也放在 B+ 侧，而不是"只存 R 树叶页号、用的时候再去 R 树取"】
// 方案 §4.9 的原设计是后者。实测（`tests/index/test_hybrid_diff.cc` 的 E 用例与
// `ltm_demo` 的 hybrid 段）表明那条路走不通：**R 树的分裂会把一半条目搬到新页**，
// 于是"插入时就记下的叶页号"在负载稍大之后大部分都失效 —— 2000 条纯插入之后，
// 提示的命中率只有 ~48%；失效一次就要做一次 O(R 树叶页数) 的叶链全扫。
// 结果是 Get/Erase 有一半概率退化成全扫，"混合索引定位是 O(log n)"这句话就不成立。
//
// 把 MBR 直接放进 B+ 侧的记录里，Get/Erase 就**完全不需要访问 R 树**：
//   * Get    = B+ 定位一次，读回来的记录自带 MBR；
//   * Erase  = B+ 定位一次拿到精确 MBR，再用它去删 R 树（R 树的叶内匹配是
//              (键, MBR) 双匹配，本来就必须要 MBR）；
//   * R 树只回答空间问题（相交/包含/KNN），它那边的副本照样有 MBR。
// 代价是 B+ 侧的载荷多 4+16*dim 字节 —— 换掉的是"一半概率全扫"。
//
// B+ 叶条目里的 `rtree_leaf_page` 因此降级为**纯提示**：只给 I8 交叉校验用
// （提示有效时 O(1)，失效时退化为叶链全扫，结果仍然正确）。
// ---------------------------------------------------------------------------
std::vector<char> EncodeBPlusPayload(const Mbr& mbr, const std::vector<char>& payload,
                                     uint16_t dim) {
  const size_t header = 4 + sizeof(double) * 2 * dim;
  std::vector<char> out(header + payload.size(), 0);
  const uint16_t plen = static_cast<uint16_t>(payload.size());
  std::memcpy(out.data(), &dim, sizeof(dim));
  std::memcpy(out.data() + 2, &plen, sizeof(plen));
  for (uint16_t d = 0; d < dim; ++d) {
    const double lo = mbr.lo[d];
    const double hi = mbr.hi[d];
    std::memcpy(out.data() + 4 + sizeof(double) * d, &lo, sizeof(double));
    std::memcpy(out.data() + 4 + sizeof(double) * (dim + d), &hi, sizeof(double));
  }
  if (!payload.empty()) {
    std::memcpy(out.data() + header, payload.data(), payload.size());
  }
  return out;
}

Status DecodeBPlusPayload(const std::vector<char>& blob, uint16_t dim, Mbr* mbr,
                          std::vector<char>* payload) {
  const size_t header = 4 + sizeof(double) * 2 * dim;
  if (blob.size() < header) {
    return Status::Error(StatusCode::kCorruption,
                         "the hybrid B+ payload is " + std::to_string(blob.size()) +
                             " bytes, shorter than the " + std::to_string(header) +
                             "-byte record header");
  }
  uint16_t stored_dim = 0;
  uint16_t plen = 0;
  std::memcpy(&stored_dim, blob.data(), sizeof(stored_dim));
  std::memcpy(&plen, blob.data() + 2, sizeof(plen));
  if (stored_dim != dim || header + plen != blob.size()) {
    return Status::Error(StatusCode::kCorruption,
                         "the hybrid B+ payload does not describe a " + std::to_string(dim) +
                             "-dimensional record of the expected length");
  }
  *mbr = Mbr::Invalid(dim);
  mbr->lo.resize(dim);
  mbr->hi.resize(dim);
  for (uint16_t d = 0; d < dim; ++d) {
    double lo = 0.0;
    double hi = 0.0;
    std::memcpy(&lo, blob.data() + 4 + sizeof(double) * d, sizeof(double));
    std::memcpy(&hi, blob.data() + 4 + sizeof(double) * (dim + d), sizeof(double));
    mbr->lo[d] = lo;
    mbr->hi[d] = hi;
  }
  payload->assign(blob.begin() + static_cast<std::ptrdiff_t>(header), blob.end());
  return Status::Ok();
}

}  // namespace

struct HybridIndex::Impl {
  mutable std::shared_mutex api_mutex;
  std::atomic<uint64_t> queries{0};
  HybridOptions options;
  bool opened = false;
  IndexStats stats;
  // 页算子必须活得比树长（树只持有指针）
  std::unique_ptr<BPlusPageOps> bplus_ops;
  std::unique_ptr<RTreePageOps> rtree_ops;
  // 混合索引需要两棵树的**具体**能力（R 树的"插入并报告叶页"、按键定位），
  // 这些不属于 TreeOps 契约 —— 通用契约只覆盖结构维护半边与统一查询，
  // 定位是各树自己的算法（tree_ops.h 的【诚实边界】）。名字校验后 static_cast。
  BPlusTree* bplus = nullptr;
  RTree* rtree = nullptr;
};

HybridIndex::HybridIndex() : impl_(new Impl()) {}

HybridIndex::~HybridIndex() {
  // 成员按声明逆序析构：rtree_ → bplus_ → checkpoint_ → pool_ → ... → disk_。
  // pool_ 先于 recovery_ 析构是刻意的：缓冲池的析构要在日志还活着的时候写
  // clean_shutdown（见 storage/buffer_pool.cc）。
}

IndexEntry HybridIndex::OneDim(KeyId key) {
  IndexEntry e;
  e.key = key;  // mbr 保持 dim == 0：B+ 树只接受一维记录
  return e;
}

IndexEntry HybridIndex::Materialize(const RecordRef& ref) {
  IndexEntry e;
  e.key = ref.key;
  e.mbr = ref.mbr;
  e.rtree_leaf = ref.rtree_leaf;
  if (ref.payload != nullptr && ref.payload_len > 0) {
    e.payload.assign(ref.payload, ref.payload + ref.payload_len);
  }
  return e;
}

Status HybridIndex::Open(const HybridOptions& options) {
  ApiGuard api(impl_->api_mutex, impl_.get(), true);
  if (!api.ok()) return ReadUpgradeError();
  if (impl_->opened) {
    return InvalidArgument("HybridIndex is already open");
  }
  HybridOptions o = options;
  // 缺省值：B+ 容量 32、R 树每页 64 条 / 2 维（与两棵树的验收配置一致）
  if (o.bplus.order_or_capacity == 0) {
    o.bplus.order_or_capacity = 32;
  }
  if (o.rtree.order_or_capacity == 0) {
    o.rtree.order_or_capacity = 64;
  }
  if (o.rtree.mbr_dim == 0) {
    o.rtree.mbr_dim = 2;
  }
  o.bplus.mbr_dim = 0;  // B+ 只接受一维记录
  // 两棵树的元数据槽位必须不同，否则后打开的树会覆盖前者的根页号/高度
  o.bplus.caller_tree_slot = options.bplus_slot;
  o.rtree.caller_tree_slot = options.rtree_slot;
  if (o.bplus_tree_id == o.rtree_tree_id) {
    return InvalidArgument("the two trees must use different tree ids (page self-description)");
  }
  if (o.bplus_slot == o.rtree_slot) {
    return InvalidArgument("the two trees must use different metadata slots");
  }
  if (o.rtree.mbr_dim > 8) {
    return InvalidArgument("mbr_dim > 8 is not supported by this implementation");
  }
  // A previous Close or failed Open can leave owned storage here. Destroy
  // borrowers before their dependencies; pool destruction still uses WAL/disk.
  if (disk_ != nullptr) {
    rtree_.reset();
    bplus_.reset();
    impl_->rtree = nullptr;
    impl_->bplus = nullptr;
    impl_->rtree_ops.reset();
    impl_->bplus_ops.reset();
    checkpoint_.reset();
    pool_.reset();
    migration_policy_.reset();
    hot_cold_.reset();
    recovery_.reset();
    disk_.reset();
  }
  impl_->queries.store(0, std::memory_order_relaxed);
  impl_->stats.writes = 0;
  impl_->stats.i8_pairs_checked = 0;
  impl_->options = o;

  // ---- 1. 存储内核。顺序：DiskManager → RecoveryManager → BufferPool → Recover ----
  disk_.reset(new DiskManager(o.config.disk));
  recovery_.reset(new RecoveryManager(o.config.wal, disk_.get()));
  if (recovery_->wal() == nullptr) {
    return Status::Error(StatusCode::kIoError, "the recovery manager could not create a WalWriter");
  }
  hot_cold_.reset(new HotColdTracker(MigrationOptionsFrom(o.config)));
  migration_policy_.reset(new MigrationPolicy(MigrationOptionsFrom(o.config)));
  pool_.reset(new BufferPool(o.config.pool, disk_.get(), recovery_->wal(), recovery_.get(),
                             hot_cold_.get(), migration_policy_.get()));
  // 【必须在任何索引打开之前】恢复直接读写页文件、不经过缓冲池；
  // 若先 Open 了树，缓冲池里就会有恢复前的旧页。
  LTM_RETURN_IF_ERROR(recovery_->Recover(pool_.get()));
  checkpoint_.reset(
      new CheckpointManager(o.config.wal, disk_.get(), recovery_.get(), pool_.get()));

  // ---- 2. 按名字建树（D3 的可扩展点；新增变体不动存储层）----
  IndexRegistry::Instance().RegisterBuiltins();
  auto bp = IndexRegistry::Instance().Create("bplus", o.bplus);
  if (!bp.ok()) {
    return bp.status();
  }
  bplus_ = std::move(bp.value());
  auto rt = IndexRegistry::Instance().Create("rtree", o.rtree);
  if (!rt.ok()) {
    return rt.status();
  }
  rtree_ = std::move(rt.value());

  // ---- 3. 页算子 + Open ----
  impl_->bplus_ops.reset(new BPlusPageOps(o.bplus.order_or_capacity));
  impl_->rtree_ops.reset(new RTreePageOps(o.rtree.mbr_dim, ParseSplitPolicy(o.rtree.split_policy),
                                          o.rtree.order_or_capacity));
  LTM_RETURN_IF_ERROR(
      bplus_->Open(pool_.get(), impl_->bplus_ops.get(), o.bplus, o.bplus_tree_id));
  LTM_RETURN_IF_ERROR(
      rtree_->Open(pool_.get(), impl_->rtree_ops.get(), o.rtree, o.rtree_tree_id));
  if (std::string(bplus_->name()) != "bplus" || std::string(rtree_->name()) != "rtree") {
    return Status::Error(StatusCode::kCorruption, "the registry returned unexpected tree kinds");
  }
  impl_->bplus = static_cast<BPlusTree*>(bplus_.get());
  impl_->rtree = static_cast<RTree*>(rtree_.get());
  impl_->opened = true;

  LOG_INFO("HybridIndex open: bplus(cap=", o.bplus.order_or_capacity, ", slot=", o.bplus_slot,
           ") + rtree(cap=", o.rtree.order_or_capacity, ", dim=", o.rtree.mbr_dim, ", slot=",
           o.rtree_slot, "), recovered=", (recovery_->report().recovery_needed ? "yes" : "no"));
  return Status::Ok();
}

Status HybridIndex::Close() {
  ApiGuard api(impl_->api_mutex, impl_.get(), true);
  if (!api.ok()) return ReadUpgradeError();
  if (!impl_->opened) {
    return Status::Ok();
  }
  // 【顺序】先把脏页全部落盘，再标干净关闭 —— 反过来的话"干净关闭"会在数据真正
  // 落盘之前就成立，下次打开会跳过恢复，而数据其实还在内存里。
  LTM_RETURN_IF_ERROR(pool_->FlushAll());
  LTM_RETURN_IF_ERROR(recovery_->MarkCleanShutdown());
  impl_->opened = false;
  LOG_INFO("HybridIndex closed: flushed all dirty pages and marked the log as cleanly shut down");
  return Status::Ok();
}

bool HybridIndex::is_open() const {
  ApiGuard api(impl_->api_mutex, impl_.get(), false);
  return impl_->opened;
}

// ---------------------------------------------------------------------------
// 写：一次逻辑事务，双树原子提交
// ---------------------------------------------------------------------------
Status HybridIndex::Put(const IndexEntry& entry) {
  ApiGuard api(impl_->api_mutex, impl_.get(), true);
  if (!api.ok()) return ReadUpgradeError();
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "HybridIndex::Put called before Open");
  }
  const uint16_t dim = im.rtree_ops->dim();
  if (entry.dim() != dim) {
    return Status::Error(StatusCode::kDimensionMismatch,
                         "record dim " + std::to_string(entry.dim()) + " != index dim " +
                             std::to_string(dim));
  }
  LTM_RETURN_IF_ERROR(entry.mbr.Validate());

  // 一个事务罩住两棵树。内层（RTree::Insert 自己的 upsert 括号、以及两棵树的
  // IndexTree::Insert）都会 join 进它，只有这里的 Commit 写 kCommit 记录并 fsync。
  ScopedWriteOp op(pool_.get());

  // 1) 先写 R 树（空间副本）。返回的叶页号只作为 I8 交叉校验的定位提示。
  auto leaf = im.rtree->InsertReturningLeaf(entry);
  if (!leaf.ok()) {
    return leaf.status();
  }
  // 2) B+ 侧写**完整记录**：键 + MBR + 载荷。Get/Erase 因此完全不必访问 R 树
  //    （理由见上面 EncodeBPlusPayload 的长注释）。
  IndexEntry be = OneDim(entry.key);
  be.payload = EncodeBPlusPayload(entry.mbr, entry.payload, dim);
  be.rtree_leaf = leaf.value();
  LTM_RETURN_IF_ERROR(im.bplus->Insert(be));

  LTM_RETURN_IF_ERROR(op.Commit());
  ++im.stats.writes;
  return Status::Ok();
}

Status HybridIndex::Erase(KeyId key) {
  // Include the read of the old MBR in the exclusive operation boundary.
  ApiGuard api(impl_->api_mutex, impl_.get(), true);
  if (!api.ok()) return ReadUpgradeError();
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "HybridIndex::Erase called before Open");
  }
  // 1) 从 B+ 读出这条记录（键 + MBR + 载荷）—— 一次定位，不需要碰 R 树
  IndexEntry found;
  bool present = false;
  LTM_RETURN_IF_ERROR(ReadBPlusKey(key, &found, &present));
  if (!present) {
    return NotFound("no such key: " + std::to_string(key));
  }
  // 2) 用记录自带的精确 MBR 删 R 树。R 树的叶内匹配是 (键, MBR) 双匹配，
  //    只有键对不上 MBR 会被判成"不是同一条记录"，所以 MBR 是必须的 ——
  //    而它就在刚读出来的这条记录里。
  IndexEntry spatial = found;  // 已经是物化的（key + mbr + 载荷），可以直接用
  ScopedWriteOp op(pool_.get());
  const Status erased = im.rtree->Erase(spatial);
  if (erased.code() == StatusCode::kNotFound) {
    // B+ 有、R 没有 ⇒ I8 已经被破坏。这是**结构性**错误，必须吵，
    // 不能当"反正删掉就行" —— 那会把不一致静默地抹平。
    return Status::Error(StatusCode::kCorruption,
                         "I8 violated: key " + std::to_string(key) +
                             " exists in the B+ tree but not in the R tree");
  }
  LTM_RETURN_IF_ERROR(erased);
  LTM_RETURN_IF_ERROR(im.bplus->Erase(OneDim(key)));
  LTM_RETURN_IF_ERROR(op.Commit());
  ++im.stats.writes;
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 读
// ---------------------------------------------------------------------------
// 从 B+ 读一条记录（键 + MBR + 载荷 + R 树叶页号提示）。
//
// 单点区间复用 B+ 的扫描与记录解码路径。Search 与 RangeScan 均已持有自有载荷；
// RangeScan 按页复制命中记录、释放页闩后执行回调，载荷寿命不依赖页帧驻留。
Status HybridIndex::ReadBPlusKey(KeyId key, IndexEntry* out, bool* present) {
  Impl& im = *impl_;
  *present = false;
  // 【必须写全限定名】本类的成员函数 Query 会遮蔽同名类型 ltm::Query：
  // 在类作用域内写 `Query q;` 会被解析成"成员函数名"，报出误导性的 C2146。
  ltm::Query q;
  q.kind = QueryKind::kRange1D;
  q.key_lo = key;
  q.key_hi = key;
  std::vector<char> blob;
  PageId hint = kInvalidPageId;
  Status s = im.bplus->RangeScan(q, [&](const RecordRef& r) {
    if (r.key != key) {
      return true;
    }
    if (r.payload != nullptr && r.payload_len > 0) {
      blob.assign(r.payload, r.payload + r.payload_len);
    } else {
      blob.clear();
    }
    hint = r.rtree_leaf;
    *present = true;
    return false;  // 找到一个就够了
  });
  LTM_RETURN_IF_ERROR(s);
  if (!*present) {
    return Status::Ok();
  }
  out->key = key;
  out->rtree_leaf = hint;
  return DecodeBPlusPayload(blob, im.rtree_ops->dim(), &out->mbr, &out->payload);
}

StatusOr<IndexEntry> HybridIndex::Get(KeyId key) {
  ApiGuard api(impl_->api_mutex, impl_.get(), false);
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "HybridIndex::Get called before Open");
  }
  // 一次 B+ 定位就够：MBR 与载荷都在 B+ 侧的那条记录里（见 EncodeBPlusPayload）。
  // 这里**不访问 R 树** —— 这正是把 MBR 内联进 B+ 载荷换来的东西。
  IndexEntry found;
  bool present = false;
  LTM_RETURN_IF_ERROR(ReadBPlusKey(key, &found, &present));
  if (!present) {
    return NotFound("no such key: " + std::to_string(key));
  }
  im.queries.fetch_add(1, std::memory_order_relaxed);
  return found;
}

Status HybridIndex::Query(const ltm::Query& query,
                          const std::function<bool(const IndexEntry&)>& emit) {
  ApiGuard api(impl_->api_mutex, impl_.get(), false);
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "HybridIndex::Query called before Open");
  }
  im.queries.fetch_add(1, std::memory_order_relaxed);

  // 路由：一维问题交给 B+，空间问题交给 R。
  // 【不要试图让一棵树回答另一棵树的问题】B+ 的键是逻辑主键，R 树的路由键是
  // MBR 的 Z 序编码；用 R 树回答"按键范围"要全扫，用 B+ 回答"空间相交"根本
  // 表达不了（它不存 MBR）。
  switch (query.kind) {
    case QueryKind::kPoint: {
      auto one = Get(query.key_lo);
      if (one.status().code() == StatusCode::kNotFound) {
        return Status::Ok();
      }
      LTM_RETURN_IF_ERROR(one.status());
      if (!emit(one.value())) {
        return Status::Ok();
      }
      return Status::Ok();
    }
    case QueryKind::kRange1D: {
      // 一维区间：B+ 沿叶链扫（按键有序）。命中的记录自带 MBR 与载荷，
      // **不需要**再去 R 树补齐 —— 这也是把 MBR 内联进 B+ 载荷换来的。
      Status err;
      const uint16_t dim = im.rtree_ops->dim();
      Status s = im.bplus->RangeScan(query, [&](const RecordRef& r) {
        IndexEntry e;
        e.key = r.key;
        e.rtree_leaf = r.rtree_leaf;
        std::vector<char> blob;
        if (r.payload != nullptr && r.payload_len > 0) {
          blob.assign(r.payload, r.payload + r.payload_len);
        }
        err = DecodeBPlusPayload(blob, dim, &e.mbr, &e.payload);
        if (!err.ok()) {
          return false;
        }
        return emit(e);
      });
      if (!s.ok()) {
        return s;
      }
      return err;
    }
    case QueryKind::kRangeSpatial:
    case QueryKind::kIntersect: {
      // 空间查询：R 树自己展开（它知道怎么用 MBR 剪枝）。
      // 命中的 RecordRef 里已经带全了 key/MBR/载荷，**不需要**再回查 R 树 ——
      // 只有一维那一路才缺 MBR。这里物化一次即可，别顺手多做一次定位。
      return im.rtree->SpatialQuery(query, [&](const RecordRef& r) {
        return emit(Materialize(r));
      });
    }
    case QueryKind::kKnn: {
      // Traversal counters are call-local; only the final snapshot is published.
      return im.rtree->Knn(query, [&](const RecordRef& r, double dist) {
        LTM_UNUSED(dist);
        return emit(Materialize(r));
      });
    }
  }
  return InvalidArgument("unknown query kind");
}

Status HybridIndex::Scan(const ltm::Query& query,
                         const std::function<bool(const IndexEntry&)>& emit) {
  return Query(query, emit);
}

Status HybridIndex::Knn(const ltm::Query& query, uint32_t k, std::vector<IndexEntry>* out) {
  if (out == nullptr) {
    return InvalidArgument("Knn needs an output vector");
  }
  out->clear();
  ltm::Query q = query;  // 同上：全限定名，否则被成员函数名 Query 遮蔽
  q.kind = QueryKind::kKnn;
  q.k = k;
  LTM_RETURN_IF_ERROR(Query(q, [out](const IndexEntry& e) {
    out->push_back(e);
    return true;
  }));
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// 运维
// ---------------------------------------------------------------------------
Status HybridIndex::Checkpoint() {
  ApiGuard api(impl_->api_mutex, impl_.get(), true);
  if (!api.ok()) return ReadUpgradeError();
  if (!impl_->opened) {
    return Status::Error(StatusCode::kCorruption, "HybridIndex::Checkpoint called before Open");
  }
  std::lock_guard<std::recursive_mutex> stable(pool_->write_mutex());
  return checkpoint_->RunCheckpoint();
}

Status HybridIndex::DecayHeat() {
  ApiGuard api(impl_->api_mutex, impl_.get(), false);
  if (!impl_->opened) return InvalidArgument("DecayHeat called before Open");
  hot_cold_->DecayAll();
  return Status::Ok();
}

Status HybridIndex::Validate() {
  ApiGuard api(impl_->api_mutex, impl_.get(), true);
  if (!api.ok()) return ReadUpgradeError();
  Impl& im = *impl_;
  if (!im.opened) {
    return Status::Error(StatusCode::kCorruption, "HybridIndex::Validate called before Open");
  }
  // I8 spans both trees: keep writers out until the full comparison finishes.
  std::lock_guard<std::recursive_mutex> stable(pool_->write_mutex());
  // 1) 两棵树各自的结构不变量
  LTM_RETURN_IF_ERROR(im.bplus->Validate());
  LTM_RETURN_IF_ERROR(im.rtree->Validate());

  // 2) I8：两棵树必须指向同一份记录集 —— 逐条交叉校验，不是抽样。
  //    两个方向都要查：B+ 里有的 R 里必须有，R 里有的 B+ 里也必须有。
  //    只查一个方向会漏掉"R 树里多出一条幽灵记录"这种不一致。
  //
  //    这里用 `RTree::Locate`（按键定位）而不是空间查询：R 树的插入去向由 MBR
  //    决定，逻辑键不是路由键，所以"这条键在不在树里"只能用叶链定位回答。
  //    B+ 叶条目里存的 R 树叶页号作为提示传进去，命中时 O(1)、失效时退化为全扫。
  uint64_t checked = 0;
  uint64_t bad_key = 0;
  bool missing = false;
  bool disagree = false;
  const uint16_t dim = im.rtree_ops->dim();
  Status s = im.bplus->ScanAll([&](const RecordRef& r) {
    if (missing || disagree) {
      return true;  // 已经记下了第一个不一致，继续扫完但不重复记录
    }
    IndexEntry mine;
    mine.key = r.key;
    std::vector<char> blob;
    if (r.payload != nullptr && r.payload_len > 0) {
      blob.assign(r.payload, r.payload + r.payload_len);
    }
    if (!DecodeBPlusPayload(blob, dim, &mine.mbr, &mine.payload).ok()) {
      bad_key = r.key;
      disagree = true;
      return true;
    }
    auto located = im.rtree->Locate(r.key, r.rtree_leaf);
    if (!located.ok()) {
      bad_key = r.key;
      missing = true;
      return true;
    }
    // MBR 与载荷都必须一致：两棵树存的是同一条逻辑记录
    const IndexEntry& theirs = located.value();
    bool same = (mine.payload == theirs.payload) && (mine.mbr.lo.dim() == theirs.mbr.lo.dim());
    for (uint16_t d = 0; d < dim && same; ++d) {
      if (mine.mbr.lo[d] != theirs.mbr.lo[d] || mine.mbr.hi[d] != theirs.mbr.hi[d]) {
        same = false;
      }
    }
    if (!same) {
      bad_key = r.key;
      disagree = true;
      return true;
    }
    ++checked;
    return true;
  });
  if (!s.ok()) {
    return Status::Error(StatusCode::kCorruption,
                         "I8 violated while scanning the B+ tree: " + s.ToString());
  }
  if (missing) {
    return Status::Error(StatusCode::kCorruption,
                         "I8 violated: key " + std::to_string(bad_key) +
                             " exists in the B+ tree but not in the R tree");
  }
  if (disagree) {
    return Status::Error(StatusCode::kCorruption,
                         "I8 violated: the two trees disagree about key " + std::to_string(bad_key) +
                             " (the MBR or the payload differs)");
  }
  // P0 introduced this two-phase check to avoid R-leaf -> B+ root lock order.
  // P1 scan callbacks now release page latches, but retaining the two phases
  // keeps the reverse I8 check independent of callback latch lifetime.
  std::vector<KeyId> rtree_keys;
  Status s2 = im.rtree->ScanAll([&](const RecordRef& r) {
    rtree_keys.push_back(r.key);
    return true;
  });
  if (!s2.ok()) {
    return Status::Error(StatusCode::kCorruption, "I8 violated while scanning the R tree: " + s2.ToString());
  }
  const uint64_t rtree_count = rtree_keys.size();
  bool rtree_orphan = false;
  uint64_t orphan_key = 0;
  for (KeyId key : rtree_keys) {
    IndexEntry probe = OneDim(key);
    bool present = false;
    Status rs = ReadBPlusKey(key, &probe, &present);
    if (!rs.ok() || !present) {
      rtree_orphan = true;
      orphan_key = key;
      break;
    }
  }
  if (rtree_orphan) {
    return Status::Error(StatusCode::kCorruption,
                         "I8 violated: key " + std::to_string(orphan_key) +
                             " exists in the R tree but not in the B+ tree");
  }
  if (checked != rtree_count) {
    return Status::Error(StatusCode::kCorruption,
                         "I8 violated: the two trees hold different numbers of records (" +
                             std::to_string(checked) + " vs " + std::to_string(rtree_count) + ")");
  }
  im.stats.i8_pairs_checked = checked;
  return Status::Ok();
}

IndexStats HybridIndex::stats() const {
  ApiGuard api(impl_->api_mutex, impl_.get(), false);
  Impl& im = *impl_;
  IndexStats s;
  if (!im.opened) {
    return s;
  }
  s.bplus = im.bplus->stats();
  s.rtree = im.rtree->stats();
  s.pool = pool_->SnapshotStats(&s.io);
  const uint64_t hits = s.pool.hits.load();
  const uint64_t total = hits + s.pool.misses.load();
  s.pool_hit_rate = total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
  s.heat = hot_cold_->stats();
  s.queries = im.queries.load(std::memory_order_relaxed);
  s.writes = im.stats.writes;
  const RTree::LocateStats& ls = im.rtree->locate_stats();
  s.locate_hint_hits = ls.hint_hits;
  s.locate_hint_misses = ls.hint_misses;
  s.locate_chain_scans = ls.chain_scans;
  s.i8_pairs_checked = im.stats.i8_pairs_checked;
  return s;
}

}  // namespace ltm

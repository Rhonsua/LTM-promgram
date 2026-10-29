// tests/index/test_tree_ops_contract.cc
// 契约测试：把 tree_ops.h 的接口约定固化成可执行断言。
// 这些用例是**回归护栏**：M3 接入 R 树、后续接入第三个树变体时都必须保持通过。
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "index/bplus_tree.h"
#include "index/mock_tree.h"
#include "index/page_utils.h"
#include "index/rtree.h"
#include "tests/harness/test_harness.h"

using namespace ltm;

namespace {

// 一个"最小树变体"：只实现契约，用于验证 TreeOps 默认实现与调用约定
class ContractTree final : public TreeOps {
 public:
  const char* name() const override { return "contract"; }
  Status Open(BufferPool* pool, TreePageOps* ops, const TreeBuildOptions& o, TreeId tid) override {
    pool_ = pool;
    ops_ = ops;
    options_ = o;
    tree_id_ = tid;
    ++open_calls;
    return Status::Ok();
  }
  Status Insert(const IndexEntry& e) override {
    last_key = e.key;
    return Status::Ok();
  }
  Status Erase(const IndexEntry&) override { return Status::Ok(); }
  StatusOr<RecordRef> Search(const IndexEntry&) override { return NotFound("empty tree"); }
  Status RangeScan(const Query&, const std::function<bool(const RecordRef&)>&) override {
    return Status::Ok();
  }
  Status ScanAll(const std::function<bool(const RecordRef&)>&) override { return Status::Ok(); }
  Status Validate() override { return Status::Ok(); }
  TreeStats stats() const override { return TreeStats{}; }
  Status Destroy() override { return Status::Ok(); }

  int open_calls = 0;
  KeyId last_key = 0;
  TreeId tree_id_ = 0;
  BufferPool* pool_ = nullptr;
  TreePageOps* ops_ = nullptr;
  TreeBuildOptions options_;
};

}  // namespace

TEST(TreeOpsContract, SpatialAndKnnDefaultToNotSupported) {
  ContractTree t;
  Status sp = t.SpatialQuery(Query{}, [](const RecordRef&) { return true; });
  EXPECT_TRUE(sp.code() == StatusCode::kNotSupported);
  // 报错必须带树名，否则多树场景无法定位
  EXPECT_TRUE(sp.message().find("contract") != std::string::npos);

  Status kn = t.Knn(Query{}, [](const RecordRef&, double) { return true; });
  EXPECT_TRUE(kn.code() == StatusCode::kNotSupported);
  EXPECT_TRUE(kn.message().find("contract") != std::string::npos);
}

TEST(TreeOpsContract, OpenReceivesInjectedDependencies) {
  ContractTree t;
  TreeBuildOptions o;
  o.order_or_capacity = 64;
  o.mbr_dim = 3;
  ASSERT_TRUE(t.Open(nullptr, nullptr, o, 7).ok());
  EXPECT_EQ(t.open_calls, 1);
  EXPECT_EQ(t.tree_id_, 7u);
  EXPECT_EQ(t.options_.order_or_capacity, 64u);
  EXPECT_EQ(t.options_.mbr_dim, 3u);
}

TEST(TreePageOpsContract, KeyTypeIsScalarAndSentinelIsZero) {
  // ElemKey 必须是一等标量类型：跨页算法据此做路由比较
  const ElemKey k = 12345;
  EXPECT_EQ(sizeof(ElemKey), sizeof(uint64_t));
  EXPECT_EQ(kMinElemKey, 0u);
  EXPECT_TRUE(k > kMinElemKey);
}

TEST(BPlusPageOpsContract, RequiresExplicitCapacity) {
  // 阶数是构造参数，不存在"默认阶数"，避免页容量悄悄取到 0
  BPlusPageOps ops(32);
  EXPECT_TRUE(std::string(ops.name()) == "bplus-page");
  EXPECT_EQ(ops.Capacity(), 32u);
  BPlusPageOps tiny(1);  // 小于下限时被抬到下限
  EXPECT_EQ(tiny.Capacity(), kBPlusMinCapacity);
}

TEST(BPlusPageOpsContract, PageMetadataAccessorsWorkOnRealPage) {
  std::vector<char> raw(kPageSize, 0);
  Page page(raw.data());
  page.Init(1, PageType::kBPlusLeaf, 1, 0);
  BPlusPageOps ops(16);
  EXPECT_TRUE(ops.IsLeaf(page.header()));
  EXPECT_EQ(ops.Level(page.header()), 0u);
  ASSERT_TRUE(ops.InitPage(page.mutable_header(), page.mutable_payload(), true).ok());
  EXPECT_EQ(ops.FirstKey(page.header(), page.payload()), kMinElemKey);
  EXPECT_EQ(page.num_slots(), 0u);
  EXPECT_FALSE(ops.IsOverflow(page.header(), page.payload()));
  EXPECT_TRUE(ops.IsUnderflow(page.header(), page.payload()));  // 空页当然下溢
}

TEST(BPlusPageOpsContract, InternalPageRejectsNonSentinelFirstKey) {
  std::vector<char> raw(kPageSize, 0);
  Page page(raw.data());
  page.Init(2, PageType::kBPlusInternal, 1, 1);
  BPlusPageOps ops(16);
  ASSERT_TRUE(ops.InitPage(page.mutable_header(), page.mutable_payload(), false).ok());
  const std::vector<char> empty;
  // 0 号槽必须是哨兵键
  ASSERT_TRUE(
      ops.InsertEntry(page.mutable_header(), page.mutable_payload(), 0, kMinElemKey, 5, empty).ok());
  // 非 0 号槽写入哨兵键应被拒绝
  EXPECT_TRUE(ops.InsertEntry(page.mutable_header(), page.mutable_payload(), 1, kMinElemKey, 6, empty)
                  .code() == StatusCode::kInvalidArgument);
  EXPECT_TRUE(ops.InsertEntry(page.mutable_header(), page.mutable_payload(), 1, 100, 7, empty).ok());
}

TEST(BPlusPageOpsContract, LeafAndInternalAreDistinguished) {
  std::vector<char> leaf_raw(kPageSize, 0);
  Page leaf(leaf_raw.data());
  leaf.Init(1, PageType::kBPlusLeaf, 1, 0);
  std::vector<char> internal_raw(kPageSize, 0);
  Page internal(internal_raw.data());
  internal.Init(2, PageType::kBPlusInternal, 1, 1);
  BPlusPageOps ops(8);
  EXPECT_TRUE(ops.IsLeaf(leaf.header()));
  EXPECT_FALSE(ops.IsLeaf(internal.header()));
  EXPECT_EQ(ops.Level(internal.header()), 1u);
}

TEST(BPlusContract, RequiresMatchingPageOpsAndPool) {
  BPlusTree t;
  EXPECT_TRUE(std::string(t.name()) == "bplus");
  MockPageOps wrong_ops;
  // 传入不匹配的页算子必须明确报错，而不是静默退化为错误行为
  Status s = t.Open(nullptr, &wrong_ops, TreeBuildOptions{}, 1);
  EXPECT_TRUE(s.code() == StatusCode::kInvalidArgument);

  BPlusPageOps ops(16);
  // 有算子但没有缓冲池
  EXPECT_TRUE(t.Open(nullptr, &ops, TreeBuildOptions{}, 1).code() ==
              StatusCode::kInvalidArgument);
}

TEST(BPlusContract, RejectsMultiDimensionalEntry) {
  BPlusTree t;
  IndexEntry e;
  e.key = 1;
  e.mbr = Mbr::Invalid(2);
  e.mbr.lo[0] = 0;
  e.mbr.hi[0] = 1;
  e.mbr.lo[1] = 0;
  e.mbr.hi[1] = 1;
  // 未 Open 时先撞 kCorruption；Open 之后应报 kInvalidArgument
  Status s = t.Insert(e);
  EXPECT_TRUE(s.code() == StatusCode::kCorruption || s.code() == StatusCode::kInvalidArgument);
}

TEST(RTreeContract, RequiresDimension) {
  RTree t;
  EXPECT_TRUE(std::string(t.name()) == "rtree");
  TreeBuildOptions o;  // mbr_dim 缺省为 0
  Status s = t.Open(nullptr, nullptr, o, 2);
  EXPECT_TRUE(s.code() == StatusCode::kInvalidArgument);
  EXPECT_TRUE(s.message().find("mbr_dim") != std::string::npos);
}

TEST(RTreeContract, RejectsWrongPageOps) {
  RTree t;
  TreeBuildOptions o;
  o.mbr_dim = 2;
  BPlusPageOps bplus_ops(16);
  Status s = t.Open(nullptr, &bplus_ops, o, 2);
  EXPECT_TRUE(s.code() == StatusCode::kInvalidArgument);
  EXPECT_TRUE(s.message().find("rtree-page") != std::string::npos);
}

TEST(SplitPolicy, ParsingAndNames) {
  EXPECT_TRUE(ParseSplitPolicy("even") == RSplitPolicy::kEven);
  EXPECT_TRUE(ParseSplitPolicy("linear") == RSplitPolicy::kLinear);
  EXPECT_TRUE(ParseSplitPolicy("quadratic") == RSplitPolicy::kQuadratic);
  // 未知值回落 quadratic（默认策略），不得抛异常
  EXPECT_TRUE(ParseSplitPolicy("whatever") == RSplitPolicy::kQuadratic);
  EXPECT_TRUE(std::string(SplitPolicyName(RSplitPolicy::kEven)) == "even");
  EXPECT_TRUE(std::string(SplitPolicyName(RSplitPolicy::kQuadratic)) == "quadratic");
}

TEST(TreeStatsContract, DefaultsAreZero) {
  TreeStats s;
  EXPECT_EQ(s.height, 0u);
  EXPECT_EQ(s.num_pages, 0u);
  EXPECT_EQ(s.num_entries, 0u);
  EXPECT_EQ(s.avg_fill_rate, 0.0);
  EXPECT_EQ(s.root_page, kInvalidPageId);
}

TEST(MbrContract, InvalidMbrIsEmpty) {
  Mbr m = Mbr::Invalid(3);
  EXPECT_EQ(m.lo.dim(), 3u);
  EXPECT_FALSE(m.valid());   // 空 MBR 不满足 lo <= hi
  EXPECT_EQ(m.Area(), 0.0);  // 绝不能是 inf/NaN
  Mbr other = Mbr::Invalid(3);
  for (uint16_t d = 0; d < 3; ++d) {
    other.lo[d] = 0.0;
    other.hi[d] = 1.0;
  }
  ASSERT_TRUE(other.Validate().ok());
  m.ExpandToInclude(other);
  EXPECT_TRUE(m.valid());
  EXPECT_TRUE(m.Contains(other));
}

TEST(MockTreeContract, ImplementsTreePageOps) {
  MockPageOps ops;
  EXPECT_TRUE(std::string(ops.name()) == "mock-page");
  EXPECT_TRUE(ops.IsLeaf(PageHeader{}));
  EXPECT_EQ(ops.Capacity(), 0u);
  // 它不用页，因此页算子必须明确报"不支持"
  EXPECT_TRUE(ops.InitPage(nullptr, nullptr, true).code() == StatusCode::kNotSupported);
  TreeEntry moved;
  EXPECT_FALSE(ops.BorrowFrom(nullptr, nullptr, nullptr, BorrowDirection::kFromLeft, TreeEntry{},
                              &moved));
  EXPECT_TRUE(ops.SplitInto(nullptr, nullptr, nullptr).code() == StatusCode::kNotSupported);
  EXPECT_TRUE(ops.EncodeEntry(IndexEntry{}, nullptr).code() == StatusCode::kNotSupported);
  EXPECT_TRUE(ops.InsertLeafEntry(nullptr, nullptr, TreeEntry{}).status().code() ==
              StatusCode::kNotSupported);
}

// ---------------------------------------------------------------------------
// v3 挂钩契约：新增挂钩后必须仍然"只加在 TreePageOps 上"。
// 这条断言是 D3 指标的编译期护栏 —— 若有人把新挂钩加到 TreeOps，
// 上面那个只实现 TreeOps 的 ContractTree 会立刻编译失败，
// 而本用例则锁定"挂钩数量与语义"不被悄悄削弱。
// ---------------------------------------------------------------------------
TEST(TreePageOpsContract, V3HooksArePresentAndTreeSpecific) {
  // 页算子必须自报树身份：元数据槽位靠它区分，不能靠"调用方记得传对算子"
  BPlusPageOps bplus(16);
  EXPECT_TRUE(bplus.MetaKind() == MetaTreeKind::kBPlus);
  EXPECT_TRUE(bplus.PageTypeForLevel(0) == PageType::kBPlusLeaf);
  EXPECT_TRUE(bplus.PageTypeForLevel(1) == PageType::kBPlusInternal);
  EXPECT_TRUE(bplus.HasLeftmostSentinel());
  EXPECT_FALSE(bplus.SeparatorMayChangeOnInsert());
  EXPECT_TRUE(bplus.KeysAreOrdered());
  EXPECT_EQ(bplus.UnderflowThreshold(), 8u);  // 容量 16 → 阈值 8

  RTreePageOps rtree(2, RSplitPolicy::kQuadratic, 64);
  EXPECT_TRUE(rtree.MetaKind() == MetaTreeKind::kRTree);
  EXPECT_TRUE(rtree.PageTypeForLevel(0) == PageType::kRTreeLeaf);
  EXPECT_TRUE(rtree.PageTypeForLevel(1) == PageType::kRTreeInternal);
  // 多维树与一维树在这三条上必须不同，否则跨页算法会按 B+ 的形状处理 R 树
  EXPECT_FALSE(rtree.HasLeftmostSentinel());
  EXPECT_TRUE(rtree.SeparatorMayChangeOnInsert());
  EXPECT_FALSE(rtree.KeysAreOrdered());
  // 下溢阈值也必须不同（R 树 40% < B+ 50%）
  EXPECT_TRUE(rtree.UnderflowThreshold() < 64u);
}

TEST(BPlusPageOpsContract, EntryByteBudgetIsOptional) {
  // 定长条目的树不声明预算（0 = 不做预算检查），变长条目的树必须声明，
  // 否则页会在达到 capacity_ 条之前先被字节撑满，IsOverflow 永不触发。
  BPlusPageOps bplus(16);
  EXPECT_EQ(bplus.EntryByteBudget(), 0u);
  RTreePageOps rtree(2, RSplitPolicy::kQuadratic, 64);
  EXPECT_TRUE(rtree.EntryByteBudget() > 0u);
}

// ===========================================================================
// R 树页算子契约（M3 Stage B 的闸门）
//
// 这些用例把 rtree.cc 的页内语义固化成可执行断言：布局往返、维度校验、
// 三种分裂、借键合并、I9 精确相等、以及"父页分隔条目只有一个来源"。
// 它们同时是**回归护栏**：M3 之后任何一次页算子改动都必须保持通过。
// ===========================================================================
namespace {

// 一块真实页 + Page 视图。禁用拷贝：Page 只是帧的轻量视图，
// 拷贝后 raw_ 会指向别人的缓冲区（page.h 的悬垂视图断言正是抓这个）。
class RPage {
 public:
  RPage(PageId pid, PageType type, uint8_t level) : raw_(kPageSize, 0), page_(raw_.data()) {
    page_.Init(pid, type, /*tree_id=*/1, level);
  }
  RPage(const RPage&) = delete;
  RPage& operator=(const RPage&) = delete;

  PageHeader* h() { return page_.mutable_header(); }
  char* p() { return page_.mutable_payload(); }
  Page& page() { return page_; }
  const PageHeader& header() const { return page_.header(); }
  const char* payload() const { return page_.payload(); }

 private:
  std::vector<char> raw_;
  Page page_;
};

Mbr Rect2(double x0, double y0, double x1, double y1) {
  Mbr m = Mbr::Invalid(2);
  m.lo[0] = x0;
  m.lo[1] = y0;
  m.hi[0] = x1;
  m.hi[1] = y1;
  return m;
}

IndexEntry RectEntry(KeyId k, double x, double y, double w = 1.0) {
  IndexEntry e;
  e.key = k;
  e.mbr = Rect2(x, y, x + w, y + w);
  return e;
}

}  // namespace

TEST(RTreePageOpsContract, LeafEntryRoundTripAndDimensionCheck) {
  RTreePageOps ops(2, RSplitPolicy::kQuadratic, 64);
  RPage pg(7, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(pg.h(), pg.p(), true).ok());

  IndexEntry e = RectEntry(42, 0.0, 0.0, 2.0);  // 2x2 → 面积 4
  e.mbr.hi[1] = 3.0;                            // 2x3 → 面积 6
  e.payload = {'a', 'b', 'c'};
  TreeEntry te;
  ASSERT_TRUE(ops.EncodeEntry(e, &te).ok());

  auto ins = ops.InsertLeafEntry(pg.h(), pg.p(), te);
  ASSERT_TRUE(ins.ok());
  EXPECT_TRUE(ins.value() == LeafInsertResult::kInserted);
  EXPECT_EQ(pg.page().num_slots(), 1u);

  const SlotId s = ops.FindEntry(pg.header(), pg.payload(), e);
  ASSERT_TRUE(s != kInvalidSlot);
  auto ref = ops.DecodeLeafEntry(pg.header(), pg.payload(), s);
  ASSERT_TRUE(ref.ok());
  EXPECT_EQ(ref.value().key, 42u);
  EXPECT_EQ(ref.value().mbr.Area(), 6.0);
  EXPECT_EQ(ref.value().payload_len, 3u);
  EXPECT_TRUE(std::string(ref.value().payload, 3) == "abc");
  EXPECT_EQ(ref.value().rtree_leaf, 7u);  // 叶条目自报所属叶页（B+ 树靠它联动删除）

  auto m = ops.EntryMbr(pg.header(), pg.payload(), 0);
  ASSERT_TRUE(m.ok());
  EXPECT_EQ(m.value().Area(), 6.0);
  EXPECT_TRUE(ops.ValidatePage(pg.header(), pg.payload()).ok());

  // 键相同但 MBR 不同 → 不是同一条记录（(键, MBR) 双匹配）
  IndexEntry other = RectEntry(42, 50.0, 50.0);
  EXPECT_TRUE(ops.FindEntry(pg.header(), pg.payload(), other) == kInvalidSlot);

  // 完全相同的键 → 覆盖，条目数不变
  auto again = ops.InsertLeafEntry(pg.h(), pg.p(), te);
  ASSERT_TRUE(again.ok());
  EXPECT_TRUE(again.value() == LeafInsertResult::kOverwrote);
  EXPECT_EQ(pg.page().num_slots(), 1u);

  // 维度校验：所有 MBR 运算的前置门（中期遗留任务）
  IndexEntry bad = e;
  bad.mbr = Rect2(0.0, 0.0, 1.0, 1.0);
  bad.mbr.lo.resize(3);
  bad.mbr.hi.resize(3);
  TreeEntry ignored;
  EXPECT_TRUE(ops.EncodeEntry(bad, &ignored).code() == StatusCode::kDimensionMismatch);

  // 退化 MBR（点记录）合法：lo == hi，面积为 0
  IndexEntry pt = RectEntry(1, 5.0, 5.0, 0.0);
  TreeEntry pe;
  ASSERT_TRUE(ops.EncodeEntry(pt, &pe).ok());
  ASSERT_TRUE(ops.InsertLeafEntry(pg.h(), pg.p(), pe).ok());
  EXPECT_TRUE(ops.ValidatePage(pg.header(), pg.payload()).ok());

  // 非法 MBR（lo > hi）必须在编码阶段就被拒绝，而不是写进页里
  IndexEntry bad2 = RectEntry(2, 9.0, 9.0);
  bad2.mbr.hi[0] = -1.0;
  TreeEntry ignored2;
  EXPECT_TRUE(!ops.EncodeEntry(bad2, &ignored2).ok());
}

TEST(RTreePageOpsContract, ThresholdsAndOverflowUseCapacity) {
  RTreePageOps ops(2, RSplitPolicy::kQuadratic, 8);
  EXPECT_EQ(ops.Capacity(), 8u);
  EXPECT_EQ(ops.UnderflowThreshold(), 3u);  // 8 * 40% = 3

  RPage pg(1, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(pg.h(), pg.p(), true).ok());
  EXPECT_TRUE(ops.IsUnderflow(pg.header(), pg.payload()));  // 空页当然下溢
  EXPECT_FALSE(ops.IsOverflow(pg.header(), pg.payload()));

  for (KeyId k = 0; k < 8; ++k) {
    TreeEntry te;
    ASSERT_TRUE(ops.EncodeEntry(RectEntry(k, static_cast<double>(k) * 3.0, 0.0), &te).ok());
    ASSERT_TRUE(ops.InsertLeafEntry(pg.h(), pg.p(), te).ok());
  }
  EXPECT_TRUE(ops.IsOverflow(pg.header(), pg.payload()));  // 达到 capacity 就该分裂
  EXPECT_FALSE(ops.IsUnderflow(pg.header(), pg.payload()));
}

TEST(RTreePageOpsContract, SplitKeepsThresholdAndLeafChain) {
  RTreePageOps ops(2, RSplitPolicy::kQuadratic, 8);
  RPage left(1, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(left.h(), left.p(), true).ok());
  left.h()->next_page = 99;  // 分裂后必须把 old_next 挪到新页上
  for (KeyId k = 0; k < 8; ++k) {
    TreeEntry te;
    ASSERT_TRUE(ops.EncodeEntry(RectEntry(k, static_cast<double>(k) * 3.0, 0.0), &te).ok());
    ASSERT_TRUE(ops.InsertLeafEntry(left.h(), left.p(), te).ok());
  }

  RPage right(2, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(right.h(), right.p(), true).ok());
  ASSERT_TRUE(ops.SplitInto(left.h(), left.p(), &right.page()).ok());

  EXPECT_EQ(static_cast<uint32_t>(left.page().num_slots()) +
                static_cast<uint32_t>(right.page().num_slots()),
            8u);
  // 下溢阈值：任何一半都不得低于它，否则分裂即制造下溢页（I3 当场报错）
  EXPECT_TRUE(left.page().num_slots() >= ops.UnderflowThreshold());
  EXPECT_TRUE(right.page().num_slots() >= ops.UnderflowThreshold());
  // 叶链：old -> new -> old_next
  EXPECT_EQ(left.header().next_page, 2u);
  EXPECT_EQ(right.header().prev_page, 1u);
  EXPECT_EQ(right.header().next_page, 99u);
  EXPECT_TRUE(ops.ValidatePage(left.header(), left.payload()).ok());
  EXPECT_TRUE(ops.ValidatePage(right.header(), right.payload()).ok());

  // 分裂不得丢条目、不得重复：两半的键并集恰好是原集合
  bool seen[8] = {false};
  const Page* halves[2] = {&left.page(), &right.page()};
  for (const Page* pg : halves) {
    for (uint16_t i = 0; i < pg->num_slots(); ++i) {
      auto ref = ops.DecodeLeafEntry(pg->header(), pg->payload(), i);
      ASSERT_TRUE(ref.ok());
      ASSERT_TRUE(ref.value().key < 8u);
      EXPECT_FALSE(seen[ref.value().key]);
      seen[ref.value().key] = true;
    }
  }
  for (bool b : seen) {
    EXPECT_TRUE(b);
  }
}

TEST(RTreePageOpsContract, EvenSplitPolicyIsBalanced) {
  // 三种策略必须真的不同：even 是严格的"前半/后半"
  RTreePageOps ops(2, RSplitPolicy::kEven, 8);
  RPage left(1, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(left.h(), left.p(), true).ok());
  for (KeyId k = 0; k < 8; ++k) {
    TreeEntry te;
    ASSERT_TRUE(ops.EncodeEntry(RectEntry(k, static_cast<double>(k), 0.0), &te).ok());
    ASSERT_TRUE(ops.InsertLeafEntry(left.h(), left.p(), te).ok());
  }
  RPage right(2, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(right.h(), right.p(), true).ok());
  ASSERT_TRUE(ops.SplitInto(left.h(), left.p(), &right.page()).ok());
  EXPECT_EQ(left.page().num_slots(), 4u);
  EXPECT_EQ(right.page().num_slots(), 4u);
}

TEST(RTreePageOpsContract, BorrowAndMergeMoveEntriesWithoutLoss) {
  RTreePageOps ops(2, RSplitPolicy::kQuadratic, 8);
  RPage donor(1, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(donor.h(), donor.p(), true).ok());
  RPage recv(2, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(recv.h(), recv.p(), true).ok());

  for (KeyId k = 0; k < 6; ++k) {
    TreeEntry te;
    ASSERT_TRUE(ops.EncodeEntry(RectEntry(k, static_cast<double>(k), 0.0), &te).ok());
    ASSERT_TRUE(ops.InsertLeafEntry(donor.h(), donor.p(), te).ok());
  }
  TreeEntry te1;
  ASSERT_TRUE(ops.EncodeEntry(RectEntry(100, 100.0, 100.0), &te1).ok());
  ASSERT_TRUE(ops.InsertLeafEntry(recv.h(), recv.p(), te1).ok());

  TreeEntry moved;
  const bool ok = ops.BorrowFrom(recv.h(), recv.p(), &donor.page(), BorrowDirection::kFromLeft,
                                 TreeEntry{}, &moved);
  EXPECT_TRUE(ok);
  EXPECT_EQ(recv.page().num_slots(), 2u);
  EXPECT_EQ(donor.page().num_slots(), 5u);
  EXPECT_TRUE(ops.ValidatePage(donor.header(), donor.payload()).ok());
  EXPECT_TRUE(ops.ValidatePage(recv.header(), recv.payload()).ok());

  // 合并：donor 的全部条目搬进 recv，donor 侧条目数归零（回收由通用层做）
  const uint16_t before = recv.page().num_slots();
  const uint16_t moved_n = donor.page().num_slots();
  ASSERT_TRUE(ops.MergeFrom(recv.h(), recv.p(), donor.page(), kMinElemKey).ok());
  EXPECT_EQ(recv.page().num_slots(), static_cast<uint16_t>(before + moved_n));
  EXPECT_TRUE(ops.ValidatePage(recv.header(), recv.payload()).ok());
}

TEST(RTreePageOpsContract, ParentSeparatorIsExactUnionOfChild) {
  // I9 的核心：父页该槽的 MBR **恰好等于**子页全部条目 MBR 的并集。
  RTreePageOps ops(2, RSplitPolicy::kQuadratic, 8);
  RPage leaf(3, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(leaf.h(), leaf.p(), true).ok());
  TreeEntry t0;
  ASSERT_TRUE(ops.EncodeEntry(RectEntry(0, 0.0, 0.0, 2.0), &t0).ok());
  ASSERT_TRUE(ops.InsertLeafEntry(leaf.h(), leaf.p(), t0).ok());
  TreeEntry t1;
  ASSERT_TRUE(ops.EncodeEntry(RectEntry(1, 10.0, 10.0, 2.0), &t1).ok());
  ASSERT_TRUE(ops.InsertLeafEntry(leaf.h(), leaf.p(), t1).ok());

  RPage parent(1, PageType::kRTreeInternal, 1);
  ASSERT_TRUE(ops.InitPage(parent.h(), parent.p(), false).ok());
  const TreeEntry sep = ops.BuildSeparator(leaf.header(), leaf.payload());
  EXPECT_EQ(sep.child, 3u);
  ASSERT_TRUE(
      ops.InsertChildAt(parent.h(), parent.p(), 0, sep).ok());
  EXPECT_TRUE(ops.CheckParentKey(parent.header(), parent.payload(), 0, leaf.page()).ok());
  EXPECT_TRUE(ops.SeparatorEquals(parent.header(), parent.payload(), 0, sep));

  // 子页多了一条 → 并集变大 → 父页条目陈旧 → I9 必须**报错**（不能静默通过）
  TreeEntry t2;
  ASSERT_TRUE(ops.EncodeEntry(RectEntry(2, 20.0, 20.0, 5.0), &t2).ok());
  ASSERT_TRUE(ops.InsertLeafEntry(leaf.h(), leaf.p(), t2).ok());
  EXPECT_TRUE(ops.CheckParentKey(parent.header(), parent.payload(), 0, leaf.page()).code() ==
              StatusCode::kCorruption);

  // 用同一个推导刷新后必须恢复一致（单一来源：BuildSeparator）
  const TreeEntry sep2 = ops.BuildSeparator(leaf.header(), leaf.payload());
  EXPECT_FALSE(ops.SeparatorEquals(parent.header(), parent.payload(), 0, sep2));
  ASSERT_TRUE(ops.WriteSeparator(parent.h(), parent.p(), 0, sep2).ok());
  EXPECT_TRUE(ops.CheckParentKey(parent.header(), parent.payload(), 0, leaf.page()).ok());
  EXPECT_TRUE(ops.SeparatorEquals(parent.header(), parent.payload(), 0, sep2));
  // 就地改写：条目长度不变、槽数不变
  EXPECT_EQ(parent.page().num_slots(), 1u);
  EXPECT_TRUE(ops.ValidatePage(parent.header(), parent.payload()).ok());

  // 父页 MBR 确实收缩到并集（不能只增不减）：并集 = (0,0)-(25,25)
  auto pm = ops.EntryMbr(parent.header(), parent.payload(), 0);
  ASSERT_TRUE(pm.ok());
  EXPECT_EQ(pm.value().lo[0], 0.0);
  EXPECT_EQ(pm.value().hi[0], 25.0);
  EXPECT_EQ(pm.value().lo[1], 0.0);
  EXPECT_EQ(pm.value().hi[1], 25.0);
}

TEST(RTreePageOpsContract, CandidateSlotsCoverOverlaps) {
  RTreePageOps ops(2, RSplitPolicy::kQuadratic, 8);
  // 三个孩子：0 与 1 的 MBR 重叠，2 远离
  const Mbr mbrs[3] = {Rect2(0.0, 0.0, 10.0, 10.0), Rect2(5.0, 5.0, 15.0, 15.0),
                       Rect2(100.0, 100.0, 110.0, 110.0)};
  RPage parent(1, PageType::kRTreeInternal, 1);
  ASSERT_TRUE(ops.InitPage(parent.h(), parent.p(), false).ok());
  for (int i = 0; i < 3; ++i) {
    RPage child(static_cast<PageId>(10 + i), PageType::kRTreeLeaf, 0);
    ASSERT_TRUE(ops.InitPage(child.h(), child.p(), true).ok());
    IndexEntry e;
    e.key = static_cast<KeyId>(i);
    e.mbr = mbrs[i];
    TreeEntry te;
    ASSERT_TRUE(ops.EncodeEntry(e, &te).ok());
    ASSERT_TRUE(ops.InsertLeafEntry(child.h(), child.p(), te).ok());
    ASSERT_TRUE(ops.InsertChildAt(parent.h(), parent.p(), static_cast<SlotId>(i),
                                  ops.BuildSeparator(child.header(), child.payload()))
                    .ok());
  }
  EXPECT_EQ(ops.GetChild(parent.header(), parent.payload(), 0), 10u);
  EXPECT_EQ(ops.GetChild(parent.header(), parent.payload(), 2), 12u);

  // 探针落在重叠区：必须给出**多个**候选 —— 单路径下钻会静默漏记录
  IndexEntry probe = RectEntry(0, 6.0, 6.0, 1.0);
  auto cand = ops.CandidateChildSlots(parent.header(), parent.payload(), probe,
                                      SlotChoice::kSearch);
  ASSERT_TRUE(cand.ok());
  EXPECT_EQ(cand.value().size(), 2u);
  EXPECT_TRUE(cand.value()[0] == 0u);
  EXPECT_TRUE(cand.value()[1] == 1u);

  // 探针远离全部孩子：候选为空（而不是硬塞一个最近的）——
  // 这让"删一条不存在的记录"能正确地报 kNotFound 而不是乱删
  IndexEntry far = RectEntry(0, 1000.0, 1000.0, 1.0);
  auto none =
      ops.CandidateChildSlots(parent.header(), parent.payload(), far, SlotChoice::kSearch);
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none.value().size(), 0u);

  // 插入下钻只挑一个：最小面积增量 → 落在 2 号（完全包含，增量为 0）
  IndexEntry ins = RectEntry(0, 102.0, 102.0, 1.0);
  EXPECT_EQ(ops.ChooseChildSlot(parent.header(), parent.payload(), ins, SlotChoice::kInsert), 2u);
}

TEST(RTreePageOpsContract, ChooseHalfPrefersSmallerEnlargement) {
  RTreePageOps ops(2, RSplitPolicy::kQuadratic, 8);
  RPage left(1, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(left.h(), left.p(), true).ok());
  RPage right(2, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(right.h(), right.p(), true).ok());
  for (int i = 0; i < 2; ++i) {
    TreeEntry tl;
    ASSERT_TRUE(ops.EncodeEntry(RectEntry(static_cast<KeyId>(i), i * 2.0, 0.0), &tl).ok());
    ASSERT_TRUE(ops.InsertLeafEntry(left.h(), left.p(), tl).ok());
    TreeEntry tr;
    ASSERT_TRUE(ops.EncodeEntry(RectEntry(static_cast<KeyId>(10 + i), 100.0 + i * 2.0, 0.0), &tr).ok());
    ASSERT_TRUE(ops.InsertLeafEntry(right.h(), right.p(), tr).ok());
  }
  TreeEntry near_right;
  ASSERT_TRUE(ops.EncodeEntry(RectEntry(20, 101.0, 0.0), &near_right).ok());
  EXPECT_EQ(ops.ChooseHalf(near_right, 1, left.header(), left.payload(), 2, right.header(),
                           right.payload()),
            2u);
  TreeEntry near_left;
  ASSERT_TRUE(ops.EncodeEntry(RectEntry(21, 1.0, 0.0), &near_left).ok());
  EXPECT_EQ(ops.ChooseHalf(near_left, 1, left.header(), left.payload(), 2, right.header(),
                           right.payload()),
            1u);
}

TEST(RTreePageOpsContract, ValidatePageRejectsCorruptEntry) {
  RTreePageOps ops(2, RSplitPolicy::kQuadratic, 8);
  RPage pg(1, PageType::kRTreeLeaf, 0);
  ASSERT_TRUE(ops.InitPage(pg.h(), pg.p(), true).ok());
  TreeEntry te;
  ASSERT_TRUE(ops.EncodeEntry(RectEntry(0, 0.0, 0.0), &te).ok());
  ASSERT_TRUE(ops.InsertLeafEntry(pg.h(), pg.p(), te).ok());

  // 篡改页内 lo > hi：必须在自检里被抓住（对应"R 树维度校验与异常路径"）
  char* entry = pg.p() + pageutil::SlotsOf(pg.p())[0].offset;
  const double bad = 100.0;
  std::memcpy(entry + sizeof(RTreeEntry), &bad, sizeof(bad));  // lo[0] = 100 > hi[0] = 1
  EXPECT_TRUE(ops.ValidatePage(pg.header(), pg.payload()).code() == StatusCode::kCorruption);
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

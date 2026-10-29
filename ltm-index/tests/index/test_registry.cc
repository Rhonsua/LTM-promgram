#include <memory>
#include <string>
#include <vector>

#include "tests/harness/test_harness.h"
#include "index/index_registry.h"
#include "index/mock_tree.h"

using namespace ltm;

// 这组用例是方案 D3「通用性」的运行时验收：
// 新增树变体只实现 TreeOps/TreePageOps 即可接入，注册表之外无需改动任何存储层代码。
TEST(Registry, BuiltinsAreRegistered) {
  auto& reg = IndexRegistry::Instance();
  auto names = reg.List();
  EXPECT_TRUE(names.size() >= 3u);
  EXPECT_TRUE(reg.Contains("bplus"));
  EXPECT_TRUE(reg.Contains("rtree"));
  EXPECT_TRUE(reg.Contains("mock"));
}

TEST(Registry, CreateReturnsUsableTree) {
  auto& reg = IndexRegistry::Instance();
  auto t = reg.Create("mock", TreeBuildOptions{});
  ASSERT_TRUE(t.ok());
  EXPECT_TRUE(std::string(t.value()->name()) == "mock");
}

TEST(Registry, UnknownNameReturnsNotFound) {
  auto& reg = IndexRegistry::Instance();
  auto t = reg.Create("bw-tree", TreeBuildOptions{});
  EXPECT_FALSE(t.ok());
  EXPECT_TRUE(t.status().code() == StatusCode::kNotFound);
}

TEST(Registry, DuplicateRegistrationIsRejected) {
  auto& reg = IndexRegistry::Instance();
  // 不能静默覆盖已有工厂
  Status s = reg.Register("mock", [](const TreeBuildOptions&) {
    return std::unique_ptr<TreeOps>(new MockTree());
  });
  EXPECT_TRUE(s.code() == StatusCode::kDuplicateKey);
}

TEST(Registry, EmptyNameOrFactoryRejected) {
  auto& reg = IndexRegistry::Instance();
  EXPECT_TRUE(reg.Register("", [](const TreeBuildOptions&) {
                return std::unique_ptr<TreeOps>(new MockTree());
              }).code() == StatusCode::kInvalidArgument);
  EXPECT_TRUE(reg.Register("empty-factory", TreeFactory{}).code() ==
              StatusCode::kInvalidArgument);
}

TEST(Registry, MultipleTreesAreIndependent) {
  auto& reg = IndexRegistry::Instance();
  auto a = reg.Create("mock", TreeBuildOptions{});
  auto b = reg.Create("mock", TreeBuildOptions{});
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  ASSERT_TRUE(a.value()->Open(nullptr, nullptr, TreeBuildOptions{}, 1).ok());
  ASSERT_TRUE(b.value()->Open(nullptr, nullptr, TreeBuildOptions{}, 2).ok());

  IndexEntry e;
  e.key = 1;
  ASSERT_TRUE(a.value()->Insert(e).ok());
  EXPECT_EQ(a.value()->stats().num_entries, 1u);
  EXPECT_EQ(b.value()->stats().num_entries, 0u);  // 两个实例互不影响
}

TEST(MockTree, InsertSearchErase) {
  MockTree tree;
  ASSERT_TRUE(tree.Open(nullptr, nullptr, TreeBuildOptions{}, 1).ok());

  IndexEntry e;
  e.key = 99;
  e.payload = {'x', 'y', 'z'};
  ASSERT_TRUE(tree.Insert(e).ok());

  auto r = tree.Search(e);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().key, 99u);
  EXPECT_EQ(r.value().payload_len, 3u);

  ASSERT_TRUE(tree.Erase(e).ok());
  EXPECT_TRUE(tree.Search(e).status().code() == StatusCode::kNotFound);
  // 重复删除必须报 kNotFound
  EXPECT_TRUE(tree.Erase(e).code() == StatusCode::kNotFound);
}

TEST(MockTree, RejectsOperationsBeforeOpen) {
  MockTree tree;
  IndexEntry e;
  e.key = 1;
  EXPECT_TRUE(tree.Insert(e).code() == StatusCode::kCorruption);
}

TEST(MockTree, RangeScanRespectsBoundsAndCanStopEarly) {
  MockTree tree;
  ASSERT_TRUE(tree.Open(nullptr, nullptr, TreeBuildOptions{}, 1).ok());
  for (uint64_t k = 0; k < 10; ++k) {
    IndexEntry e;
    e.key = k;
    ASSERT_TRUE(tree.Insert(e).ok());
  }
  Query q;
  q.kind = QueryKind::kRange1D;
  q.key_lo = 3;
  q.key_hi = 7;
  std::vector<uint64_t> seen;
  ASSERT_TRUE(tree.RangeScan(q, [&seen](const RecordRef& r) {
    seen.push_back(r.key);
    return true;
  }).ok());
  ASSERT_EQ(seen.size(), 5u);
  EXPECT_EQ(seen.front(), 3u);
  EXPECT_EQ(seen.back(), 7u);

  // emit 返回 false 必须立即终止
  size_t count = 0;
  ASSERT_TRUE(tree.RangeScan(q, [&count](const RecordRef&) {
    ++count;
    return count < 2;
  }).ok());
  EXPECT_EQ(count, 2u);
}

TEST(MockTree, ScanAllVisitsEveryEntryInKeyOrder) {
  MockTree tree;
  ASSERT_TRUE(tree.Open(nullptr, nullptr, TreeBuildOptions{}, 1).ok());
  for (uint64_t k : {5u, 1u, 9u, 3u}) {
    IndexEntry e;
    e.key = k;
    ASSERT_TRUE(tree.Insert(e).ok());
  }
  std::vector<uint64_t> keys;
  ASSERT_TRUE(tree.ScanAll([&keys](const RecordRef& r) {
    keys.push_back(r.key);
    return true;
  }).ok());
  ASSERT_EQ(keys.size(), 4u);
  EXPECT_EQ(keys[0], 1u);
  EXPECT_EQ(keys[1], 3u);
  EXPECT_EQ(keys[2], 5u);
  EXPECT_EQ(keys[3], 9u);
}

TEST(MockTree, UnsupportedQueriesReportNotSupported) {
  MockTree tree;
  ASSERT_TRUE(tree.Open(nullptr, nullptr, TreeBuildOptions{}, 1).ok());
  EXPECT_TRUE(tree.SpatialQuery(Query{}, [](const RecordRef&) { return true; }).code() ==
              StatusCode::kNotSupported);
  EXPECT_TRUE(tree.Knn(Query{}, [](const RecordRef&, double) { return true; }).code() ==
              StatusCode::kNotSupported);
}

TEST(MockTree, DestroyClearsContents) {
  MockTree tree;
  ASSERT_TRUE(tree.Open(nullptr, nullptr, TreeBuildOptions{}, 1).ok());
  IndexEntry e;
  e.key = 7;
  ASSERT_TRUE(tree.Insert(e).ok());
  ASSERT_TRUE(tree.Destroy().ok());
  EXPECT_EQ(tree.stats().num_entries, 0u);
}

TEST(MockTree, InsertRejectsUnvalidatedEntry) {
  MockTree tree;
  ASSERT_TRUE(tree.Open(nullptr, nullptr, TreeBuildOptions{}, 1).ok());
  IndexEntry bad;
  bad.key = 1;
  bad.mbr = Mbr::Invalid(2);
  bad.mbr.lo[0] = 5.0;
  bad.mbr.hi[0] = 1.0;  // 下界大于上界
  bad.mbr.lo[1] = 0.0;
  bad.mbr.hi[1] = 1.0;
  Status s = tree.Insert(bad);
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.code() == StatusCode::kInvalidArgument);
}

int main(int argc, char** argv) { return ltm::test::RunAll(argc, argv); }

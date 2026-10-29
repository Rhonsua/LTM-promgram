#include "bench/spatial_reference.h"
#include "tests/harness/test_harness.h"
using namespace ltm;
namespace {
std::vector<IndexEntry> Truth() {
  std::vector<IndexEntry> rows(4);
  const double xs[] = {0, 2, -2, 8};
  for (size_t i = 0; i < rows.size(); ++i) {
    auto& e = rows[i]; e.key = i; e.payload = {'v', static_cast<char>('0' + i)};
    e.mbr.lo = Point(2); e.mbr.hi = Point(2);
    e.mbr.lo[0] = xs[i]; e.mbr.hi[0] = xs[i];
  }
  return rows;
}
Query Knn() { Query q; q.kind = QueryKind::kKnn; q.probe = Point(2); q.k = 2; return q; }
}
TEST(SpatialReference, KnnBoundaryTiesAndCorruption) {
  auto truth = Truth(); auto q = Knn(); const auto ref = bench::PrepareReference(truth, q);
  EXPECT_TRUE(bench::VerifyReference(truth, q, ref, {truth[0], truth[1]}));
  EXPECT_TRUE(bench::VerifyReference(truth, q, ref, {truth[0], truth[2]}));
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[1], truth[2]})); // omitted closer
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[0], truth[3]})); // too far
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[1], truth[0]})); // distance order
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[0], truth[0]})); // duplicate
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[0]}));
  auto bad = truth[1]; bad.payload = {'x'};
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[0], bad}));
  bad = truth[1]; bad.mbr.hi[1] = 1;
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[0], bad}));
  q.k = 4;
  EXPECT_TRUE(bench::VerifyReference(truth, q, bench::PrepareReference(truth, q), {truth[0], truth[1], truth[2], truth[3]}));
}
TEST(SpatialReference, IntersectionContainmentAndEmpty) {
  auto truth = Truth(); truth[1].mbr.hi[0] = 3;
  Query q; q.kind = QueryKind::kIntersect; q.mbr.lo = Point(2); q.mbr.hi = Point(2); q.mbr.hi[0] = 2;
  auto ref = bench::PrepareReference(truth, q);
  EXPECT_TRUE(bench::VerifyReference(truth, q, ref, {truth[1], truth[0]})); // closed boundary
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[0]}));
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[0], truth[0]}));
  q.kind = QueryKind::kRangeSpatial; ref = bench::PrepareReference(truth, q);
  EXPECT_TRUE(bench::VerifyReference(truth, q, ref, {truth[0]}));
  EXPECT_FALSE(bench::VerifyReference(truth, q, ref, {truth[0], truth[1]}));
  q.mbr.lo[0] = 20; q.mbr.hi[0] = 21;
  EXPECT_TRUE(bench::VerifyReference(truth, q, bench::PrepareReference(truth, q), {}));
}
int main(int argc, char** argv) { return test::RunAll(argc, argv); }

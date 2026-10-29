// Independent benchmark oracle: no tree traversal or production geometry helpers.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "index/index_entry.h"

namespace ltm::bench {
inline double DistanceSquared(const IndexEntry& row, const Point& probe) {
  double sum = 0;
  for (uint16_t d = 0; d < probe.dim(); ++d) {
    const double delta = probe[d] < row.mbr.lo[d] ? row.mbr.lo[d] - probe[d] :
                         probe[d] > row.mbr.hi[d] ? probe[d] - row.mbr.hi[d] : 0;
    sum += delta * delta;
  }
  return sum;
}
struct SpatialReference {
  std::vector<KeyId> required;
  double cutoff = 0;
  uint32_t k = 0;
};
inline SpatialReference PrepareReference(const std::vector<IndexEntry>& truth, const Query& q) {
  SpatialReference result;
  if (q.kind == QueryKind::kKnn) {
    std::vector<double> distances;
    for (const auto& row : truth) distances.push_back(DistanceSquared(row, q.probe));
    result.k = std::min(q.k, static_cast<uint32_t>(truth.size()));
    if (result.k == 0) return result;
    std::nth_element(distances.begin(), distances.begin() + result.k - 1, distances.end());
    result.cutoff = distances[result.k - 1];
    for (const auto& row : truth)
      if (DistanceSquared(row, q.probe) < result.cutoff) result.required.push_back(row.key);
  } else {
    for (const auto& row : truth) {
      bool match = true;
      for (uint16_t d = 0; d < q.mbr.lo.dim(); ++d) {
        if (q.kind == QueryKind::kRangeSpatial)
          match = match && q.mbr.lo[d] <= row.mbr.lo[d] && q.mbr.hi[d] >= row.mbr.hi[d];
        else
          match = match && q.mbr.lo[d] <= row.mbr.hi[d] && q.mbr.hi[d] >= row.mbr.lo[d];
      }
      if (match) result.required.push_back(row.key);
    }
  }
  std::sort(result.required.begin(), result.required.end());
  return result;
}
// Benchmark truth is indexed by key. Ignore physical leaf hints, check all logical fields.
inline bool VerifyReference(const std::vector<IndexEntry>& truth, const Query& q,
                            const SpatialReference& reference, const std::vector<IndexEntry>& rows) {
  if (rows.size() != (q.kind == QueryKind::kKnn ? reference.k : reference.required.size())) return false;
  std::vector<KeyId> keys;
  double previous = -1;
  for (const auto& row : rows) {
    if (row.key >= truth.size()) return false;
    const auto& expected = truth[static_cast<size_t>(row.key)];
    if (row.key != expected.key || row.payload != expected.payload ||
        row.mbr.lo.coords() != expected.mbr.lo.coords() || row.mbr.hi.coords() != expected.mbr.hi.coords()) return false;
    if (q.kind == QueryKind::kKnn) {
      const double distance = DistanceSquared(row, q.probe);
      // sqrt in the production output may round equal distances; allow only roundoff.
      const double tolerance = 1e-12 * std::max(1.0, reference.cutoff);
      if (distance + tolerance < previous || distance > reference.cutoff + tolerance) return false;
      previous = distance;
    }
    keys.push_back(row.key);
  }
  std::sort(keys.begin(), keys.end());
  if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return false;
  if (q.kind != QueryKind::kKnn) return keys == reference.required;
  // All strictly closer records must appear, even if the boundary has ties.
  return std::includes(keys.begin(), keys.end(), reference.required.begin(), reference.required.end());
}
}  // namespace ltm::bench

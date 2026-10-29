// Fixed bounded workload and independent logical-record oracle; no tree traversal.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <vector>
#include "bench/spatial_reference.h"

namespace ltm::bench {
using MixedTruth = std::map<KeyId, IndexEntry>;
enum class MixedKind : size_t { Get, Range, Spatial, Contain, Knn, Put, Erase };
inline constexpr std::array<const char*, 7> kMixedNames{"get", "range", "spatial", "contain", "knn", "put", "erase"};
inline constexpr std::array<uint64_t, 7> kMixedCounts{35, 15, 10, 5, 5, 15, 15};
inline void MixedHash(uint64_t value, uint64_t* hash) {
  for (unsigned i = 0; i < 8; ++i) { *hash ^= value & 255u; *hash *= 1099511628211ull; value >>= 8; }
}
inline void MixedHashDouble(double value, uint64_t* hash) {
  uint64_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); MixedHash(bits, hash);
}
inline void MixedHashEntry(const IndexEntry& e, uint64_t* hash) {
  MixedHash(e.key, hash); MixedHash(e.mbr.lo.dim(), hash);
  for (uint16_t d = 0; d < e.mbr.lo.dim(); ++d) {
    MixedHashDouble(e.mbr.lo[d], hash); MixedHashDouble(e.mbr.hi[d], hash);
  }
  MixedHash(e.payload.size(), hash);
  for (char c : e.payload) { *hash ^= static_cast<unsigned char>(c); *hash *= 1099511628211ull; }
}
inline uint64_t MixedTruthHash(const MixedTruth& truth) {
  uint64_t hash = 14695981039346656037ull;
  MixedHash(truth.size(), &hash);
  for (const auto& pair : truth) MixedHashEntry(pair.second, &hash);
  return hash;
}
inline bool MixedEqual(const IndexEntry& actual, const IndexEntry& expected) {
  return actual.key == expected.key && actual.payload == expected.payload &&
    actual.mbr.lo.coords() == expected.mbr.lo.coords() && actual.mbr.hi.coords() == expected.mbr.hi.coords();
}
inline bool VerifyMixedRows(const MixedTruth& truth, const Query& q, const std::vector<IndexEntry>& rows) {
  if (q.kind == QueryKind::kKnn) {
    // Full independent ranking, including the key at a tied cutoff. Comparing
    // only distances can accept the wrong records despite identical geometry.
    std::vector<std::pair<double, KeyId>> ranked;
    for (const auto& pair : truth) ranked.emplace_back(std::sqrt(DistanceSquared(pair.second, q.probe)), pair.first);
    std::sort(ranked.begin(), ranked.end());
    const size_t limit = std::min<size_t>(q.k == 0 ? 1u : q.k, ranked.size());
    if (rows.size() != limit) return false;
    for (size_t i = 0; i < limit; ++i) {
      if (rows[i].key != ranked[i].second || !MixedEqual(rows[i], truth.at(ranked[i].second))) return false;
    }
    return true;
  }
  std::vector<KeyId> keys, required;
  SpatialReference reference;
  if (q.kind == QueryKind::kRange1D) {
    for (const auto& pair : truth) if (pair.first >= q.key_lo && pair.first <= q.key_hi) required.push_back(pair.first);
  } else {
    std::vector<IndexEntry> values;
    values.reserve(truth.size());
    for (const auto& pair : truth) values.push_back(pair.second);
    reference = PrepareReference(values, q); required = reference.required;
  }
  if (rows.size() != required.size()) return false;
  for (const auto& row : rows) {
    const auto it = truth.find(row.key);
    if (it == truth.end() || !MixedEqual(row, it->second)) return false;
    keys.push_back(row.key);
  }
  if (q.kind == QueryKind::kRange1D && !std::is_sorted(keys.begin(), keys.end())) return false;
  std::sort(keys.begin(), keys.end());
  if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return false;
  return keys == required;
}
inline IndexEntry MixedEntry(uint64_t key, uint64_t revision, uint64_t records) {
  IndexEntry e; e.key = key; e.mbr.lo = Point(2); e.mbr.hi = Point(2);
  const auto side = static_cast<uint64_t>(std::ceil(std::sqrt(static_cast<double>(records + records / 4))));
  e.mbr.lo[0] = static_cast<double>(key % side) * 4 + static_cast<double>(revision % 23) / 8;
  e.mbr.lo[1] = static_cast<double>(key / side) * 4 + static_cast<double>(revision % 17) / 16;
  e.mbr.hi[0] = e.mbr.lo[0] + .75; e.mbr.hi[1] = e.mbr.lo[1] + .5;
  const auto text = std::to_string(key) + ":mixed-revision:" + std::to_string(revision);
  e.payload.assign(text.begin(), text.end()); return e;
}
struct MixedOperation {
  MixedKind kind = MixedKind::Get;
  KeyId key = 0;
  IndexEntry entry;
  Query query;
};
struct MixedTrace {
  MixedTruth initial, final;
  std::vector<KeyId> load_order;
  std::vector<MixedOperation> operations;
  uint64_t max_keys = 0, peak_records = 0;
  uint64_t dataset_hash = 0, operation_hash = 14695981039346656037ull, query_hash = 14695981039346656037ull;
  uint64_t put_new = 0, put_update = 0, put_reinsert = 0, erase_found = 0, erase_missing = 0;
};
inline void ApplyMixed(const MixedOperation& op, MixedTruth* truth) {
  if (op.kind == MixedKind::Put) (*truth)[op.key] = op.entry;
  if (op.kind == MixedKind::Erase) truth->erase(op.key);
}
inline MixedTrace MakeMixedTrace(uint64_t seed, uint64_t records, uint64_t count,
                                uint64_t range_width, uint64_t spatial_width, uint32_t k) {
  MixedTrace trace; trace.max_keys = records + records / 4; trace.peak_records = records;
  for (uint64_t key = 0; key < records; ++key) { trace.initial.emplace(key, MixedEntry(key, 0, records)); trace.load_order.push_back(key); }
  trace.dataset_hash = MixedTruthHash(trace.initial);
  std::mt19937_64 rng(seed);
  for (size_t i = trace.load_order.size(); i > 1; --i) std::swap(trace.load_order[i - 1], trace.load_order[static_cast<size_t>(rng() % i)]);
  auto model = trace.initial;
  std::map<KeyId, bool> deleted;
  uint64_t revision = 0;
  auto present = [&]() { auto it = model.begin(); std::advance(it, static_cast<std::ptrdiff_t>(rng() % model.size())); return it->first; };
  auto absent = [&]() {
    const auto start = rng() % trace.max_keys;
    for (uint64_t n = 0; n < trace.max_keys; ++n) {
      const auto key = (start + n) % trace.max_keys; if (model.find(key) == model.end()) return key;
    }
    return present();
  };
  auto append = [&](MixedKind kind, KeyId key) {
    MixedOperation op; op.kind = kind; op.key = key;
    if (kind == MixedKind::Put) {
      op.entry = MixedEntry(key, ++revision, records);
      if (model.find(key) == model.end()) { ++trace.put_new; if (deleted.count(key)) ++trace.put_reinsert; }
      else ++trace.put_update;
    }
    if (kind == MixedKind::Erase) {
      if (model.count(key)) { ++trace.erase_found; deleted[key] = true; }
      else ++trace.erase_missing;
    }
    op.query.key_lo = key; op.query.key_hi = std::min(trace.max_keys - 1, key + range_width - 1);
    op.query.mbr.lo = Point(2); op.query.mbr.hi = Point(2); op.query.probe = Point(2); op.query.k = k;
    const auto anchor = MixedEntry(key % trace.max_keys, 0, records);
    for (uint16_t d = 0; d < 2; ++d) {
      op.query.probe[d] = anchor.mbr.lo[d] + .25;
      op.query.mbr.lo[d] = anchor.mbr.lo[d] - .25;
      op.query.mbr.hi[d] = anchor.mbr.lo[d] + static_cast<double>(spatial_width);
    }
    op.query.kind = kind == MixedKind::Range ? QueryKind::kRange1D : kind == MixedKind::Contain ? QueryKind::kRangeSpatial :
      kind == MixedKind::Knn ? QueryKind::kKnn : QueryKind::kIntersect;
    ApplyMixed(op, &model); trace.peak_records = std::max(trace.peak_records, static_cast<uint64_t>(model.size()));
    trace.operations.push_back(std::move(op));
  };
  for (uint64_t block = 0; block < count / 100; ++block) {
    // A fixed dependency prefix guarantees delete/reinsert, update, and missing erase.
    const auto key = present(); append(MixedKind::Erase, key); append(MixedKind::Put, key);
    append(MixedKind::Put, present()); append(MixedKind::Erase, trace.max_keys + rng() % records);
    std::vector<MixedKind> kinds;
    for (size_t type = 0; type < kMixedCounts.size(); ++type) {
      const auto n = kMixedCounts[type] - (type == 5 || type == 6 ? 2u : 0u);
      for (uint64_t i = 0; i < n; ++i) kinds.push_back(static_cast<MixedKind>(type));
    }
    for (size_t i = kinds.size(); i > 1; --i) std::swap(kinds[i - 1], kinds[static_cast<size_t>(rng() % i)]);
    for (auto kind : kinds) {
      KeyId target = rng() % trace.max_keys;
      if (kind == MixedKind::Put) target = (rng() & 1u) ? present() : absent();
      // Reserve one record for the next block's unconditional delete/reinsert.
      if (kind == MixedKind::Erase) target = model.size() > records / 2 + 1 && (rng() & 1u) ? present() : trace.max_keys + rng() % records;
      append(kind, target);
    }
  }
  MixedHash(seed, &trace.operation_hash); MixedHash(trace.dataset_hash, &trace.operation_hash);
  for (auto key : trace.load_order) MixedHash(key, &trace.operation_hash);
  for (const auto& op : trace.operations) {
    uint64_t h = 14695981039346656037ull;
    MixedHash(static_cast<uint64_t>(op.kind), &h); MixedHash(op.key, &h);
    if (op.kind == MixedKind::Put) MixedHashEntry(op.entry, &h);
    MixedHash(static_cast<uint64_t>(op.query.kind), &h); MixedHash(op.query.key_lo, &h); MixedHash(op.query.key_hi, &h); MixedHash(op.query.k, &h);
    for (uint16_t d = 0; d < 2; ++d) {
      MixedHashDouble(op.query.probe[d], &h); MixedHashDouble(op.query.mbr.lo[d], &h); MixedHashDouble(op.query.mbr.hi[d], &h);
    }
    MixedHash(h, &trace.operation_hash);
    if (static_cast<size_t>(op.kind) < 5) MixedHash(h, &trace.query_hash);
  }
  trace.final = std::move(model); return trace;
}
}  // namespace ltm::bench

#include "migration/hot_cold_tracker.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ltm {

struct HotColdTracker::Impl {
  explicit Impl(const MigrationPolicyOptions& o) : opt(o) {}
  MigrationPolicyOptions opt;
  // Leaf lock in the lock graph: no tracker method calls back into the pool.
  mutable std::mutex mutex;
  std::unordered_map<PageId, Entry> table;
  Stats stats;
};

HotColdTracker::HotColdTracker(const MigrationPolicyOptions& options)
    : impl_(new Impl(options)) {}

HotColdTracker::~HotColdTracker() = default;

void HotColdTracker::Touch(PageId pid, bool is_prefetch) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.mutex);
  Entry& e = im.table[pid];
  uint32_t delta = im.opt.hot_delta;
  if (is_prefetch) {
    delta = static_cast<uint32_t>(delta) * im.opt.prefetch_weight_pct / 100u;
    ++im.stats.prefetch_touches;
  }
  const uint32_t next = static_cast<uint32_t>(e.heat) + delta;
  e.heat = static_cast<uint8_t>(std::min<uint32_t>(next, 255u));  // 饱和，不回绕
  e.ref_bit = 1;
  ++im.stats.touches;
}

void HotColdTracker::MarkOutOfPool(PageId pid) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->table[pid].in_pool = 0;
}

void HotColdTracker::ForgetPage(PageId pid) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->table.erase(pid);
}

uint8_t HotColdTracker::Heat(PageId pid) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->table.find(pid);
  return it == impl_->table.end() ? 0 : it->second.heat;
}

void HotColdTracker::SetLevel(PageId pid, uint8_t level) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->table[pid].level = level;
}

void HotColdTracker::SetInPool(PageId pid, bool in_pool) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->table[pid].in_pool = in_pool ? 1 : 0;
}

bool HotColdTracker::ref_bit(PageId pid) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->table.find(pid);
  return it != impl_->table.end() && it->second.ref_bit != 0;
}

uint8_t HotColdTracker::level(PageId pid) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto it = impl_->table.find(pid);
  return it == impl_->table.end() ? 0 : it->second.level;
}

bool HotColdTracker::IsCold(PageId pid) const {
  return Heat(pid) < impl_->opt.cold_threshold;
}

void HotColdTracker::DecayAll() {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.mutex);
  uint64_t cold = 0;
  for (auto& kv : im.table) {
    Entry& e = kv.second;
    e.heat = static_cast<uint8_t>(e.heat - (e.heat >> im.opt.decay_shift));
    e.ref_bit = 0;
    if (e.heat < im.opt.cold_threshold) {
      ++cold;
    }
  }
  im.stats.decays++;
  im.stats.cold_pages = cold;
  im.stats.tracked_pages = im.table.size();
}

size_t HotColdTracker::tracked_pages() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->table.size();
}

std::vector<PageId> HotColdTracker::ColdestInPool(uint32_t k) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::pair<uint8_t, PageId>> tmp;
  tmp.reserve(impl_->table.size());
  for (const auto& kv : impl_->table) {
    if (kv.second.in_pool != 0) {
      tmp.emplace_back(kv.second.heat, kv.first);
    }
  }
  std::sort(tmp.begin(), tmp.end());
  std::vector<PageId> out;
  for (uint32_t i = 0; i < k && i < tmp.size(); ++i) {
    out.push_back(tmp[i].second);
  }
  return out;
}

HotColdTracker::Stats HotColdTracker::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  Stats snapshot = impl_->stats;
  snapshot.tracked_pages = impl_->table.size();
  snapshot.cold_pages = 0;
  for (const auto& kv : impl_->table)
    if (kv.second.heat < impl_->opt.cold_threshold) ++snapshot.cold_pages;
  return snapshot;
}

void HotColdTracker::ResetStats() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->stats = Stats{};
}

const MigrationPolicyOptions& HotColdTracker::options() const { return impl_->opt; }

}  // namespace ltm

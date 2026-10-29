// src/migration/hot_cold_tracker.h
// 页级热度跟踪（方案 D5）。
//
// 与中期报告的差异：中期 ColdHotManager 是**条目级**（缓存 item 的访问计数），
// 外存场景需要的是**页级**热度 —— 因为 I/O 代价由页决定。
//
// 算法：
//   Touch()      : heat = min(255, heat + hot_delta)，并置参考位（供 CLOCK 使用）
//   DecayAll()   : heat -= heat >> decay_shift，周期性衰减（默认每 100ms）
//   IsCold()     : heat < cold_threshold
//
// 本类实现 storage/eviction_policy.h 的 HeatSource 接口，因此 BufferPool 可以
// 通过基类指针上报访问而**不需要** include 本头文件（依赖倒置，保持分层单向）。
//
// 注意：页被驱逐后仍保留 heat==0 的记录（不清除），以便区分"从未访问"与"曾经热过"；
//       页被彻底释放（归还空闲链）时由 BufferPool 调用 ForgetPage 删除记录。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "storage/eviction_policy.h"
#include "util/config.h"
#include "util/types.h"

namespace ltm {

class HotColdTracker final : public HeatSource {
 public:
  struct Entry {
    uint8_t heat = 0;
    uint8_t ref_bit = 0;
    uint8_t level = 0;    // 页层级（0 = 叶），用于迁移打分
    uint8_t in_pool = 0;  // 当前是否在缓冲池内
  };

  explicit HotColdTracker(const MigrationPolicyOptions& options);
  ~HotColdTracker() override;
  HotColdTracker(const HotColdTracker&) = delete;
  HotColdTracker& operator=(const HotColdTracker&) = delete;

  // ---- HeatSource 实现 ----
  void Touch(PageId pid, bool is_prefetch) override;
  void MarkOutOfPool(PageId pid) override;
  void ForgetPage(PageId pid) override;
  uint8_t Heat(PageId pid) const override;

  // ---- 迁移层自用 ----
  void SetLevel(PageId pid, uint8_t level);
  uint8_t heat(PageId pid) const { return Heat(pid); }
  bool ref_bit(PageId pid) const;
  uint8_t level(PageId pid) const;
  bool IsCold(PageId pid) const;
  void SetInPool(PageId pid, bool in_pool);

  // 周期衰减（由后台时钟或测试显式驱动）
  void DecayAll();

  // 兼容命名：等价于 ForgetPage
  void Forget(PageId pid) { ForgetPage(pid); }
  // 热度表当前规模（用于验证内存占用与 pool_frames 同阶）
  size_t tracked_pages() const;

  // 热度最低的 k 个"在池内"页（用于主动下刷）
  std::vector<PageId> ColdestInPool(uint32_t k) const;

  // 统计
  struct Stats {
    uint64_t touches = 0;
    uint64_t prefetch_touches = 0;
    uint64_t decays = 0;
    uint64_t cold_pages = 0;
    uint64_t tracked_pages = 0;
  };
  // Return a synchronized value snapshot; references would escape the lock.
  Stats stats() const;
  void ResetStats();

  const MigrationPolicyOptions& options() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm

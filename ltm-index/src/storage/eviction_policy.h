// src/storage/eviction_policy.h
// 存储层的两个抽象接口：驱逐策略 + 热度来源。
//
// 为什么需要它们：
//   BufferPool 需要"谁该被驱逐"与"这个页值不值得留"两类信息，但**具体策略属于
//   迁移层**。如果 BufferPool 直接 include migration/*，就形成 storage → migration
//   的反向依赖，破坏分层单向性（由 scripts/check_layering.py 的 R2 抓出）。
//   因此这里只定义接口，BufferPool 通过基类指针使用，实现由上层（HybridIndex）
//   注入 —— 依赖倒置，存储层不再知道迁移层的存在。
//
// 语义约定（务必保持，测试锁死了方向）：
//   分值越小 == 越应该被驱逐。
#pragma once

#include <cstdint>
#include <functional>
#include <limits>

#include "storage/page.h"
#include "util/types.h"

namespace ltm {

// ---------------------------------------------------------------------------
// 驱逐策略
// ---------------------------------------------------------------------------
class EvictionPolicy {
 public:
  virtual ~EvictionPolicy() = default;

  // 打分：pinned 为真时实现必须返回一个极大值（不可驱逐）
  virtual int32_t EvictScore(const PageHeader& header, uint8_t heat, bool pinned) const = 0;

  // 脏页数超过阈值时应触发主动下刷，而不是被动等驱逐
  virtual bool ShouldFlushDirty(uint32_t dirty_pages) const = 0;

  // 不可驱逐的哨兵值
  static constexpr int32_t kNotEvictable = std::numeric_limits<int32_t>::max();
};

// ---------------------------------------------------------------------------
// 热度来源：BufferPool 只通过它上报访问、查询热度，不关心热度怎么算
// ---------------------------------------------------------------------------
class HeatSource {
 public:
  virtual ~HeatSource() = default;

  // 命中一页；is_prefetch 为真时由实现按折扣计权，防止顺序扫描污染热集
  virtual void Touch(PageId pid, bool is_prefetch) = 0;
  // 页离开缓冲池
  virtual void MarkOutOfPool(PageId pid) = 0;
  // 页被彻底释放（归还空闲链）——实现应删除其记录，避免热度表无界增长
  virtual void ForgetPage(PageId pid) = 0;
  // 热度查询（0..255），供驱逐打分使用
  virtual uint8_t Heat(PageId pid) const = 0;
};

// CLOCK 环上的受害者选择：把"是否可驱逐"的判定集中在一处，
// 避免 BufferPool 里散落多份不完整判断。
class ClockEvictor {
 public:
  ClockEvictor() = default;

  // 由 BufferPool 提供的帧探测回调：返回 false 表示该帧当前不可作为受害者
  // （被钉住、或仍在被引用）
  using ProbeFn = std::function<bool(uint32_t frame_index)>;
  // 由 BufferPool 提供的帧信息回调：写出该帧当前对应的页头（含 dirty/level 标志）、
  // 热度与钉住状态。传真实页头而不是只传 level，否则 EvictScore 拿不到 dirty 标志。
  using InfoFn =
      std::function<void(uint32_t frame_index, PageHeader* header, uint8_t* heat, bool* pinned)>;

  // 单步 CLOCK：从 *hand 起扫描全部帧，返回分值最低的可驱逐帧；无候选返回 false
  bool PickVictim(uint32_t num_frames, uint32_t* hand, const ProbeFn& probe, const InfoFn& info,
                  const EvictionPolicy& policy, uint32_t* out_frame) const;
};

}  // namespace ltm

#include "storage/eviction_policy.h"

#include <climits>

namespace ltm {

bool ClockEvictor::PickVictim(uint32_t num_frames, uint32_t* hand, const ProbeFn& probe,
                              const InfoFn& info, const EvictionPolicy& policy,
                              uint32_t* out_frame) const {
  if (num_frames == 0 || hand == nullptr) {
    return false;
  }
  int32_t best_score = EvictionPolicy::kNotEvictable;
  uint32_t best_frame = 0;
  bool found = false;
  uint32_t scanned = 0;
  // 单轮扫描取分值最低者。完整实现（M1-3）在此基础上叠加"参考位优先"：
  // 第一轮只考虑 ref_bit == 0 的帧（并清掉经过的 ref_bit），找不到再放宽。
  while (scanned < num_frames) {
    const uint32_t idx = *hand;
    *hand = (*hand + 1) % num_frames;
    ++scanned;
    bool pinned = false;
    uint8_t heat = 0;
    PageHeader header{};
    info(idx, &header, &heat, &pinned);
    if (pinned || !probe(idx)) {
      continue;
    }
    const int32_t score = policy.EvictScore(header, heat, false);
    if (score < best_score) {
      best_score = score;
      best_frame = idx;
      found = true;
    }
  }
  if (found && out_frame != nullptr) {
    *out_frame = best_frame;
  }
  return found;
}

}  // namespace ltm

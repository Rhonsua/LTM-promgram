#include "migration/migration_policy.h"

#include <string>
#include <utility>

namespace ltm {

MigrationPolicy::MigrationPolicy(const MigrationPolicyOptions& options) : options_(options) {}

int32_t MigrationPolicy::EvictScore(const PageHeader& header, uint8_t heat, bool pinned) const {
  if (pinned) {
    return kNotEvictable;  // 钉住页不可驱逐
  }
  // 分级编码（分值越小 = 越该被驱逐）：
  //   [0..255] 热度：**越热分值越大**（越该保留）；越冷分值越小，越先被换出。
  //   [+100]  脏页惩罚：驱逐脏页需先刷盘，短期成本更高。
  //   [+1000] 非叶页惩罚：非叶页携带路由信息、重加载代价高。仅当它已经
  //            "冷到不值得保留"（heat < nonleaf_evict_penalty）时才取消惩罚，
  //            允许非叶页在内存极度紧张时被换出，而不是永久钉死。
  // 方向约定极易写反（本实现第一版就写反过：把 -heat 当成"越热越该走"，
  // 结果最热的页成了首选受害者）。tests/integration/test_smoke.cc 的
  // EvictionPolicyPicksColdestLeafVictim 与单调性断言专门锁住这个方向。
  // 为什么不把三项压成一个未分级的和：一个极热的非叶页会排在冷叶页前面，
  // 与"非叶页整体晚于叶页"的设计意图相反，且结论随参数取值翻转。
  int32_t score = static_cast<int32_t>(heat);  // 越热越大 → 越不该被驱逐
  if ((header.flags & kPageFlagDirty) != 0) {
    score += 100;
  }
  if (header.level != 0 && heat >= options_.nonleaf_evict_penalty) {
    score += 1000;
  }
  return score;
}

bool MigrationPolicy::ShouldFlushDirty(uint32_t dirty_pages) const {
  return dirty_pages >= options_.dirty_flush_high_water;
}

}  // namespace ltm

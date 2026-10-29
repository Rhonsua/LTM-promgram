// src/migration/migration_policy.h
// 迁移决策：给候选页打分，决定"谁更应该被驱逐"（方案 D5）。
//
// 本类是 src/storage/eviction_policy.h 中 EvictionPolicy 接口的**实现**，
// 由上层（HybridIndex）构造并注入 BufferPool。这样存储层只依赖接口，
// 不依赖迁移层的具体类型，保持分层单向。
#pragma once

#include <cstdint>

#include "storage/eviction_policy.h"
#include "util/config.h"

namespace ltm {

class MigrationPolicy final : public EvictionPolicy {
 public:
  explicit MigrationPolicy(const MigrationPolicyOptions& options);

  // 分值越小越该被驱逐；pinned 页返回 kNotEvictable
  int32_t EvictScore(const PageHeader& header, uint8_t heat, bool pinned) const override;

  bool ShouldFlushDirty(uint32_t dirty_pages) const override;

  const MigrationPolicyOptions& options() const { return options_; }

 private:
  MigrationPolicyOptions options_;
};

}  // namespace ltm

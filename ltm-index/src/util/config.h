// src/util/config.h
// 全部可调参数集中于此（方案 §9.0 参数表）。
// 读取优先级：命令行 --key=value  >  环境变量 LTM_<KEY>  >  内置默认值
// 不引入第三方 JSON 库：解析极简 "key=value" 行，保证零依赖可构建。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "util/log.h"
#include "util/status.h"
#include "util/types.h"

namespace ltm {

struct BufferPoolOptions {
  uint32_t pool_frames = 1u << 12;          // 16384 帧 × 4KB = 64MB
  uint32_t clock_hand_ms = 10;
  uint32_t decay_interval_ms = 100;
  uint8_t cold_threshold = 32;
  uint8_t hot_delta = 8;
  uint8_t decay_shift = 3;
  bool enable_prefetch = true;
  uint32_t prefetch_depth = 2;
  uint32_t prefetch_weight_pct = 50;        // 预取页热度只按 50% 计

  // M4-2 写屏障（不变量 I6）。
  //
  // 先分清两件事 —— 混在一起会得到一个"把正确实现判成违规"的检查：
  //
  //   1) 写前日志规则本身（方案 §4.4）：脏页落盘前，先把日志 fsync 到覆盖它的 LSN。
  //      FlushFrame **总是**执行这一步，并计入 PoolStats::wal_sync_on_flush。
  //      这个计数非零是**正常的**（事务中途淘汰脏页就必然发生），它是性能指标，
  //      不是正确性指标。所以"验收要求它为 0"是错的判据，已废弃。
  //
  //   2) 真正可证伪的检查：一个脏页带着 page_lsn == kInvalidLsn 落盘 ——
  //      即"它被写过，却没有任何 REDO 覆盖它"。**Sync 修不好这件事**，
  //      所以这条检查不会被"顺手同步"弄瞎。
  //
  //   true （默认）严格：情形 2 计数、记日志、返回 kCorruption，并把池毒化。
  //   false         生产：只计数（PoolStats::i6_unstamped_writes），继续写。
  bool strict_i6_barrier = true;
};

struct WalOptions {
  uint32_t group_commit_ms = 2;             // 0 = 每条立即 fsync
  uint32_t group_commit_records = 64;
  uint64_t max_wal_bytes = 64ull << 20;     // 超过则触发检查点
};

struct DiskOptions {
  uint32_t page_size = kPageSize;
  uint64_t num_pages = 1ull << 16;          // 逻辑容量，文件按需扩展
  std::string page_file = "data/index.data";
  std::string wal_file = "data/index.wal";
  bool sync_on_write = false;               // 数据页默认异步，持久性由 WAL 保证
};

struct TreeBuildOptions {
  uint16_t order_or_capacity = 0;  // B+ 树阶数；R 树填每页最大条目数
  uint16_t mbr_dim = 0;            // 仅 R 树：维度
  PageId meta_page = kInvalidPageId;  // 保留字段（页 0 是唯一元数据页）
  bool bulk_load = false;
  std::string split_policy = "quadratic";  // R 树：even | linear | quadratic
  // 在元数据页里占用哪个树槽位（见 storage/meta_page.h）。
  // 同一数据文件里可以放多棵树，各自占一个槽位；混合索引用 0/1。
  uint32_t caller_tree_slot = 0;
};

// 迁移层（页级热度与驱逐决策）参数。
// 注意：BufferPoolOptions 里也有一份冷热参数，那是**存储层自己的**缺省值；
// HybridIndex 组装时以本结构为准并显式注入，避免两处各改一半。
struct MigrationPolicyOptions {
  uint32_t clock_hand_ms = 10;
  uint32_t decay_interval_ms = 100;
  uint8_t cold_threshold = 32;
  uint8_t hot_delta = 8;
  uint8_t decay_shift = 3;
  uint32_t dirty_flush_high_water = 512;
  int32_t nonleaf_evict_penalty = 40;
  // 预取页的热度折扣（%）：防止顺序扫描把冷页刷成热页
  uint32_t prefetch_weight_pct = 50;
};

struct Config {
  DiskOptions disk;
  BufferPoolOptions pool;
  WalOptions wal;
  MigrationPolicyOptions migration;
  LogLevel log_level = LogLevel::kInfo;
  std::string log_file = "log/index.log";
  uint64_t bench_ops = 100000;

  // ---- 取值 ----
  // 可用键：disk.page_size / disk.num_pages / disk.page_file / disk.wal_file
  //        pool.pool_frames / pool.prefetch_depth / ...
  //        wal.group_commit_ms / migration.cold_threshold / log.level / log.file / bench.ops
  Status Set(const std::string& key, const std::string& value);
  std::string Get(const std::string& key) const;
  std::string Dump() const;  // 多行 "key = value"，用于实验记录

  // ---- 加载 ----
  static Config Default();
  // 解析命令行：--config=a.cfg --key=value ...
  static StatusOr<Config> FromCommandLine(int argc, char** argv);
  // 解析 key=value 配置文件（# 开头为注释）
  Status LoadFile(const std::string& path);
  // 应用环境变量 LTM_<UPPER_SNAKE(key)>
  void ApplyEnvironment();
};

}  // namespace ltm

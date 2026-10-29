// src/util/types.h
// 全项目共用的基础类型别名与常量。
#pragma once

#include <cstddef>
#include <cstdint>

namespace ltm {

using PageId = uint32_t;    // 页号：唯一稳定的页标识（不是文件偏移，可搬迁）
using Lsn = uint64_t;       // 日志序列号：单调递增，崩溃恢复的去重依据
using TxnId = uint64_t;     // 逻辑事务号：一次索引操作 = 一个逻辑事务
using SlotId = uint16_t;    // 页内槽位号
using TreeId = uint16_t;    // 混合索引中区分哪棵子树
using KeyId = uint64_t;     // 一维键（同时作为逻辑记录的主键）

constexpr PageId kInvalidPageId = 0xFFFFFFFFu;
constexpr Lsn kInvalidLsn = 0;
constexpr TxnId kInvalidTxnId = 0;
constexpr SlotId kInvalidSlot = 0xFFFFu;

constexpr uint32_t kPageSize = 4096;         // 页大小（字节），见方案 §4.3
constexpr uint32_t kPageHeaderSize = 32;
constexpr uint32_t kPageTrailerSize = 16;
constexpr uint32_t kPagePayloadSize = kPageSize - kPageHeaderSize - kPageTrailerSize;

constexpr PageId kMetaPageId = 0;            // 页 0 固定为树/索引元数据页
constexpr PageId kFirstDataPageId = 1;

// 页内碎片率上限：超过即触发 compact()，见方案 D1
constexpr uint32_t kMaxFragRatioPercent = 25;

}  // namespace ltm

// 显式标记未使用的参数：让 -Wall -Wextra / /W4 下的骨架函数保持零警告，
// 同时留下清晰的"这里尚未实现"语义。禁止用它掩盖真实遗漏。
#define LTM_UNUSED(x) static_cast<void>(x)

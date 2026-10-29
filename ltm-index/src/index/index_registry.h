// src/index/index_registry.h
// 树变体注册表 —— "通用"的可扩展点（方案 §4.9）。
//
// 使用方式：
//   IndexRegistry::Instance().Register("bplus", [](const TreeBuildOptions& o) {
//     return std::unique_ptr<TreeOps>(new BPlusTree());
//   });
//   auto tree = IndexRegistry::Instance().Create("bplus", options);
//
// 新增变体只允许新增文件 + 在本文件/一个独立的 register_*.cc 里注册，
// 禁止修改 src/storage/** 与 src/index/tree_ops.cc（由 check_layering.py 强制）。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "index/tree_ops.h"
#include "util/status.h"

namespace ltm {

using TreeFactory = std::function<std::unique_ptr<TreeOps>(const TreeBuildOptions&)>;

class IndexRegistry {
 public:
  static IndexRegistry& Instance();

  Status Register(const std::string& name, TreeFactory factory);
  StatusOr<std::unique_ptr<TreeOps>> Create(const std::string& name,
                                            const TreeBuildOptions& options) const;
  std::vector<std::string> List() const;
  bool Contains(const std::string& name) const;

  // 幂等注册：重复注册同名工厂时返回错误（避免静默覆盖）
  void RegisterBuiltins();

 private:
  IndexRegistry();
  ~IndexRegistry();
  IndexRegistry(const IndexRegistry&) = delete;
  IndexRegistry& operator=(const IndexRegistry&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm

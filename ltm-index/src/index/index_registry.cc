#include "index/index_registry.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "index/bplus_tree.h"
#include "index/mock_tree.h"
#include "index/rtree.h"
#include "util/log.h"

namespace ltm {

struct IndexRegistry::Impl {
  std::map<std::string, TreeFactory> factories;
  bool builtins_registered = false;
};

IndexRegistry::IndexRegistry() : impl_(new Impl()) {
  RegisterBuiltins();
}

IndexRegistry::~IndexRegistry() = default;

IndexRegistry& IndexRegistry::Instance() {
  static IndexRegistry inst;
  return inst;
}

Status IndexRegistry::Register(const std::string& name, TreeFactory factory) {
  if (name.empty() || !factory) {
    return InvalidArgument("Register needs a non-empty name and factory");
  }
  auto it = impl_->factories.find(name);
  if (it != impl_->factories.end()) {
    return Status::Error(StatusCode::kDuplicateKey, "tree variant already registered: " + name);
  }
  impl_->factories.emplace(name, std::move(factory));
  LOG_INFO("registered tree variant: ", name);
  return Status::Ok();
}

StatusOr<std::unique_ptr<TreeOps>> IndexRegistry::Create(const std::string& name,
                                                         const TreeBuildOptions& options) const {
  auto it = impl_->factories.find(name);
  if (it == impl_->factories.end()) {
    return NotFound("unregistered tree variant: " + name);
  }
  std::unique_ptr<TreeOps> tree = it->second(options);
  if (!tree) {
    return Status::Error(StatusCode::kCorruption, "the factory returned a null pointer: " + name);
  }
  return tree;
}

std::vector<std::string> IndexRegistry::List() const {
  std::vector<std::string> names;
  names.reserve(impl_->factories.size());
  for (const auto& kv : impl_->factories) {
    names.push_back(kv.first);
  }
  return names;
}

bool IndexRegistry::Contains(const std::string& name) const {
  return impl_->factories.find(name) != impl_->factories.end();
}

void IndexRegistry::RegisterBuiltins() {
  if (impl_->builtins_registered) {
    return;
  }
  impl_->builtins_registered = true;
  // "bplus" 与 "rtree" 的构造本身不碰存储；页算子由调用方在 Open 时注入
  Register("bplus", [](const TreeBuildOptions&) {
    return std::unique_ptr<TreeOps>(new BPlusTree());
  });
  Register("rtree", [](const TreeBuildOptions&) {
    return std::unique_ptr<TreeOps>(new RTree());
  });
  // 通用性验收样例：第三种树变体，零改动存储层即可接入
  Register("mock", [](const TreeBuildOptions&) {
    return std::unique_ptr<TreeOps>(new MockTree());
  });
}

}  // namespace ltm

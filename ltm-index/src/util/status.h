// src/util/status.h
// 全项目统一的错误传递方式：返回 Status / StatusOr<T>，禁止用异常控制正常流程。
// 见 docs/architecture.md「依赖规则」与方案 §4.1。
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace ltm {

enum class StatusCode : uint8_t {
  kOk = 0,
  kNotFound,
  kInvalidArgument,
  kOutOfSpace,        // 容器/页空间不足
  kPageFull,          // 页已满，需分裂
  kUnderflow,         // 下溢，需借键或合并
  kChecksumMismatch,  // 校验和不匹配（页损坏或撕裂写）
  kIoError,
  kCorruption,        // 结构不变量被破坏
  kNotSupported,
  kDuplicateKey,
  kDimensionMismatch,
  kNotImplemented,    // 骨架占位：本里程碑尚未实现
};

const char* StatusCodeName(StatusCode c);

class Status {
 public:
  Status() = default;  // 默认为 kOk

  static Status Ok() { return Status(); }
  static Status Error(StatusCode c, std::string msg) { return Status(c, std::move(msg)); }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

  // 便于测试与日志输出的单行描述："kNotFound: key 42 missing"
  std::string ToString() const;

  // 若 *this 失败则原样返回，否则返回 other（链式传播）
  Status Propagate(const Status& other) const { return ok() ? other : *this; }

  bool operator==(StatusCode c) const { return code_ == c; }
  bool operator!=(StatusCode c) const { return code_ != c; }

 private:
  Status(StatusCode c, std::string msg) : code_(c), message_(std::move(msg)) {}
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

// ---------------------------------------------------------------------------
// StatusOr<T>：要么持有一个 T，要么持有一个错误 Status
// ---------------------------------------------------------------------------
template <class T>
class StatusOr {
 public:
  StatusOr(T value) : value_(std::move(value)), ok_(true) {}          // NOLINT
  StatusOr(Status s) : status_(std::move(s)), ok_(false) {}           // NOLINT

  bool ok() const { return ok_; }
  const Status& status() const { return status_; }

  // 仅在 ok() 为真时合法；非法访问返回默认构造值并在 DEBUG 下断言
  const T& value() const {
    if (!ok_) {
      std::abort();
    }
    return value_;
  }
  T& value() {
    if (!ok_) {
      std::abort();
    }
    return value_;
  }
  T value_or(T fallback) const { return ok_ ? value_ : std::move(fallback); }

  const T* operator->() const { return &value(); }
  T* operator->() { return &value(); }
  const T& operator*() const { return value(); }
  T& operator*() { return value(); }

 private:
  T value_{};
  Status status_;
  bool ok_ = true;
};

// 便捷构造
inline Status OkStatus() { return Status::Ok(); }
inline Status InvalidArgument(std::string msg) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(msg));
}
inline Status NotFound(std::string msg) {
  return Status::Error(StatusCode::kNotFound, std::move(msg));
}
inline Status NotImplemented(const char* what) {
  return Status::Error(StatusCode::kNotImplemented,
                       std::string(what) + " not implemented yet (see the milestones in docs/status.md)");
}

// 断言宏：把 Status 失败变成带文件行号的明确失败
#define LTM_RETURN_IF_ERROR(expr)          \
  do {                                     \
    ::ltm::Status ltm_s_ = (expr);         \
    if (!ltm_s_.ok()) {                    \
      return ltm_s_;                       \
    }                                      \
  } while (0)

#define LTM_ASSIGN_OR_RETURN(lhs, expr)                  \
  do {                                                   \
    auto ltm_or_ = (expr);                               \
    if (!ltm_or_.ok()) {                                 \
      return ltm_or_.status();                           \
    }                                                    \
    lhs = std::move(ltm_or_.value());                    \
  } while (0)

}  // namespace ltm

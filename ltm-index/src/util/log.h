// src/util/log.h
// 由中期报告的 log.h(Logger 单例) 升级而来。变更点：
//   1. 五级日志 + 运行期级别过滤（热路径不再无条件刷屏）
//   2. LOG_EVERY_N 限流宏：高频操作只按频率输出，避免 I/O 拖慢基准测试
//   3. 线程安全：单互斥锁保护控制台与文件双通道（保持中期原有语义）
//   4. 关闭文件通道后可零 I/O 运行，基准测试用 LOG_SET_FILE("") 关闭
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

namespace ltm {

enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3, kOff = 4 };

class Logger {
 public:
  static Logger& Instance();

  void SetLevel(LogLevel lv);
  LogLevel level() const { return level_; }

  // 传空串则关闭文件通道（仅控制台）
  void SetLogFile(const std::string& path);
  const std::string& log_file() const { return file_path_; }

  void SetConsoleEnabled(bool on) { console_enabled_ = on; }

  // 供宏调用：先判断级别，避免构造开销
  bool ShouldLog(LogLevel lv) const { return static_cast<int>(lv) >= static_cast<int>(level_); }

  void Write(LogLevel lv, const char* file, int line, const std::string& msg);

  // 测试用：返回本次进程已输出的行数（按级别）
  uint64_t lines(LogLevel lv) const { return lines_[static_cast<int>(lv)]; }

 private:
  Logger() = default;
  ~Logger();
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  static const char* LevelName(LogLevel lv);

  LogLevel level_ = LogLevel::kInfo;
  std::string file_path_;
  std::mutex mu_;
  bool console_enabled_ = true;
  std::atomic<uint64_t> lines_[5] = {};
};

// 流式构造。支持两种写法：
//   LOG_INFO("x=", 3, " y=", 4.5);        // 多参数，C++17 起逗号在括号内合法
//   LOG_INFO("done") << std::endl;        // 显式刷新
//
// 实现说明（踩过的坑，务必保留）：
//   LOG_INFO 是**函数式宏**。调用 `LOG_INFO("a=", 1)` 时预处理器会吃下整对括号，
//   把多余实参原样粘到展开式末尾，得到 `... LogMessage(...)("a=", 1)`。
//   因此 LogMessage 必须提供变参 operator() 来接住这些实参 —— 这也是宏 + 变参的
//   标准配法（同 glog 的 LOG(INFO) << 与 SPDLOG_LOGGER_CALL 的取舍）。
class LogMessage {
 public:
  LogMessage(LogLevel lv, const char* file, int line) : lv_(lv), file_(file), line_(line) {}
  ~LogMessage() {
    if (!skip_) {
      Logger::Instance().Write(lv_, file_, line_, ss_.str());
    }
  }

  template <class T>
  LogMessage& operator<<(const T& v) {
    ss_ << v;
    return *this;
  }

  // 支持 LOG_INFO("x") << std::endl 的显式刷新写法
  LogMessage& operator<<(std::ostream& (*manip)(std::ostream&)) {
    if (manip == static_cast<std::ostream& (*)(std::ostream&)>(std::endl)) {
      Logger::Instance().Write(lv_, file_, line_, ss_.str());
      skip_ = true;
    }
    return *this;
  }

  // 接住函数式宏展开后粘连的实参
  template <class... Args>
  LogMessage& operator()(const Args&... args) {
    Append(args...);
    return *this;
  }

  LogMessage(const LogMessage&) = delete;
  LogMessage& operator=(const LogMessage&) = delete;

 private:
  void Append() {}

  template <class First, class... Rest>
  void Append(const First& first, const Rest&... rest) {
    ss_ << first;
    Append(rest...);
  }

  LogLevel lv_;
  const char* file_;
  int line_;
  bool skip_ = false;
  std::ostringstream ss_;
};

inline const char* BaseName(const char* path) {
  const char* p = path;
  for (const char* q = path; *q != '\0'; ++q) {
    if (*q == '/' || *q == '\\') {
      p = q + 1;
    }
  }
  return p;
}

}  // namespace ltm

// ---------------------------------------------------------------------------
// 对外宏
// ---------------------------------------------------------------------------
#define LTM_LOG_IMPL(lv)                                                       \
  if (!::ltm::Logger::Instance().ShouldLog(lv)) {                              \
  } else                                                                       \
    ::ltm::LogMessage(lv, ::ltm::BaseName(__FILE__), __LINE__)

#define LOG_DEBUG LTM_LOG_IMPL(::ltm::LogLevel::kDebug)
#define LOG_INFO LTM_LOG_IMPL(::ltm::LogLevel::kInfo)
#define LOG_WARN LTM_LOG_IMPL(::ltm::LogLevel::kWarn)
#define LOG_ERROR LTM_LOG_IMPL(::ltm::LogLevel::kError)

// 限流：每 N 次调用只输出一次
#define LOG_EVERY_N(lv, n)                                                     \
  static std::atomic<uint64_t> ltm_log_cnt_{0};                                \
  if ((ltm_log_cnt_.fetch_add(1) % (n)) != 0) {                                \
  } else                                                                       \
    LTM_LOG_IMPL(lv)

#define LOG_SET_LEVEL(lv) ::ltm::Logger::Instance().SetLevel(lv)
#define LOG_SET_FILE(p) ::ltm::Logger::Instance().SetLogFile(p)

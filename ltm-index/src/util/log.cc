#include "util/log.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>

namespace ltm {

Logger& Logger::Instance() {
  static Logger inst;
  return inst;
}

Logger::~Logger() = default;

const char* Logger::LevelName(LogLevel lv) {
  switch (lv) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo: return "INFO ";
    case LogLevel::kWarn: return "WARN ";
    case LogLevel::kError: return "ERROR";
    case LogLevel::kOff: return "OFF  ";
  }
  return "?????";
}

void Logger::SetLevel(LogLevel lv) { level_ = lv; }

void Logger::SetLogFile(const std::string& path) {
  std::lock_guard<std::mutex> lock(mu_);
  file_path_ = path;
}

void Logger::Write(LogLevel lv, const char* file, int line, const std::string& msg) {
  if (!ShouldLog(lv)) {
    return;
  }
  lines_[static_cast<int>(lv)].fetch_add(1);

  // 时间戳：本地时间 HH:MM:SS.mmm。使用 snprintf 以支持 < MSVC2015 的 gmtime_s 差异
  char ts[32];
  std::time_t now = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

  std::lock_guard<std::mutex> lock(mu_);
  if (console_enabled_) {
    std::fprintf(stdout, "[%s][%s][%s:%d] %s\n", ts, LevelName(lv), file, line, msg.c_str());
    std::fflush(stdout);
  }
  if (!file_path_.empty()) {
    // 追加模式：同一进程内多次 SetLogFile 不覆盖历史
    std::ofstream ofs(file_path_, std::ios::app);
    if (ofs) {
      ofs << '[' << ts << "][" << LevelName(lv) << "][" << file << ':' << line << "] " << msg
          << '\n';
    }
  }
}

}  // namespace ltm

#ifndef LOG_H
#define LOG_H

#include <iostream>
#include <fstream>
#include <mutex>

// 日志类：同时输出到控制台和文件 index.log
class Logger {
private:
    std::ofstream file;
    std::mutex mtx;  // 用于多线程安全（如果你的程序是单线程，可以去掉锁）
public:
    Logger() {
        file.open("index.log", std::ios::app);  // 追加模式
        if (!file.is_open()) {
            std::cerr << "[ERROR] 无法打开日志文件 index.log" << std::endl;
        }
    }
    ~Logger() {
        if (file.is_open()) file.close();
    }

    // 输出任意类型的数据
    template<typename T>
    Logger& operator<<(const T& val) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << val;          // 输出到控制台
        if (file.is_open()) {
            file << val;            // 输出到文件
            file.flush();            // 立即写入磁盘（可选）
        }
        return *this;
    }

    // 专门处理 std::endl 这类操纵符
    Logger& operator<<(std::ostream& (*manip)(std::ostream&)) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << manip;
        if (file.is_open()) {
            file << manip;
            file.flush();
        }
        return *this;
    }
};

// 全局访问点（静态局部变量保证唯一实例）
inline Logger& logger() {
    static Logger logger;
    return logger;
}

// 日志宏，自动添加 [INFO] 或 [ERROR] 前缀
#define LOG_INFO  logger() << "[INFO] "
#define LOG_ERROR logger() << "[ERROR] "

#endif // LOG_H
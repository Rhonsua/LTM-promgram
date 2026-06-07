#ifndef LOG_H
#define LOG_H

#include <iostream>
#include <fstream>
#include <mutex>

// 日志类：同时输出到控制台和文件 index.log
class Logger {
private:
    std::ofstream file;
    std::mutex mtx;
public:
    Logger() {
        file.open("index.log", std::ios::app);
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
        std::cout << val;
        if (file.is_open()) {
            file << val;
            file.flush();
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

// 全局访问点
inline Logger& logger() {
    static Logger logger;
    return logger;
}

#define LOG_INFO  logger() << "[INFO] "
#define LOG_ERROR logger() << "[ERROR] "

#endif // LOG_H
#ifndef LOG_H
#define LOG_H

#include <iostream>
#include <fstream>
#include <mutex>

class Logger {
private:
    std::ofstream file;
    std::mutex mtx;
public:
    Logger() {
        file.open("index.log", std::ios::app);
        if (!file.is_open()) {
            std::cerr << "[ERROR] Cannot open log file index.log" << std::endl;
        }
    }
    ~Logger() {
        if (file.is_open()) file.close();
    }

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

inline Logger& logger() {
    static Logger logger;
    return logger;
}

#define LOG_INFO  logger() << "[INFO] "
#define LOG_ERROR logger() << "[ERROR] "

#endif // LOG_H
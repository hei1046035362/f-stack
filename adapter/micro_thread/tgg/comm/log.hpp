#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <iomanip>
#include <cstdarg>
#include <atomic>
#include <filesystem>

// 日志级别枚举
enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

class AsyncLogger {
public:
    // 获取单例实例
    static AsyncLogger& getInstance() {
        static AsyncLogger instance;
        return instance;
    }

    // 初始化日志系统
    int init(const std::string& filename, const std::string& loglevel);

    // 停止日志系统
    void shutdown();

    // 日志记录函数（支持可变参数）
    void log(LogLevel level, const char* file, int line, const char* format, ...);

    LogLevel getloglevel() {return currentLevel_;}
private:
    AsyncLogger() = default;
    ~AsyncLogger() {
        shutdown();
    }

    // 禁用拷贝
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    // 日志级别转字符串
    const char* levelToString(LogLevel level);

    // 后台线程处理日志
    void processLogs();

    // 获取当前程序名称和进程id，exename_pid.log
    std::string genLogFilename();

    std::ofstream logFile_;             // 日志文件流
    std::queue<std::string> logQueue_;  // 日志队列
    std::mutex queueMutex_;             // 队列互斥锁
    std::mutex mutex_;                  // 文件互斥锁
    std::condition_variable condition_; // 条件变量
    std::thread writerThread_;          // 写线程
    std::atomic<bool> running_{false};  // 运行标志
    LogLevel currentLevel_;             // 当前日志级别
};

// 日志宏（自动添加文件、行号信息）
#define LOG_DEBUG(format, ...) \
    AsyncLogger::getInstance().log(LogLevel::DEBUG, __FILE__, __LINE__, format, ##__VA_ARGS__)

#define LOG_INFO(format, ...) \
    AsyncLogger::getInstance().log(LogLevel::INFO, __FILE__, __LINE__, format, ##__VA_ARGS__)

#define LOG_WARNING(format, ...) \
    AsyncLogger::getInstance().log(LogLevel::WARNING, __FILE__, __LINE__, format, ##__VA_ARGS__)

#define LOG_ERROR(format, ...) \
    AsyncLogger::getInstance().log(LogLevel::ERROR, __FILE__, __LINE__, format, ##__VA_ARGS__)

#define LOG_FATAL(format, ...) \
    AsyncLogger::getInstance().log(LogLevel::FATAL, __FILE__, __LINE__, format, ##__VA_ARGS__)


// 在不可重入函数中打印信息
#include <unistd.h>
#include <signal.h>

void __sig_snprintf(char *buf, size_t size, const char *fmt, ...);

#define SIG_PRINTF(fmt, ...) \
    do { \
        static char __sig_buf__[256]; \
        __sig_snprintf(__sig_buf__, sizeof(__sig_buf__), fmt, ##__VA_ARGS__); \
    } while(0)


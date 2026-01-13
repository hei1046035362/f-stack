#include "log.hpp"
#include <map>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>

// 初始化日志系统
int AsyncLogger::init(const std::string& filepath, const std::string& loglevel)
{
    std::map<std::string, LogLevel> logLevelMap;
    logLevelMap["DEBUG"] = LogLevel::DEBUG;
    logLevelMap["INFO"] = LogLevel::INFO;
    logLevelMap["WARNING"] = LogLevel::WARNING;
    logLevelMap["ERROR"] = LogLevel::ERROR;
    logLevelMap["FATAL"] = LogLevel::FATAL;
    std::lock_guard<std::mutex> lock(mutex_);
    std::string full_path = filepath + "/" + genLogFilename();
    logFile_.open(full_path, std::ios::out | std::ios::app);
    if (!logFile_.is_open()) {
        throw std::runtime_error("Failed to open log file: " + full_path);
        return -1;
    }
    if(logLevelMap.find(loglevel) == logLevelMap.end()) {
        currentLevel_ = LogLevel::INFO;
    } else {
        currentLevel_ = logLevelMap[loglevel];        
    }
    running_ = true;
    writerThread_ = std::thread(&AsyncLogger::processLogs, this);
    return 0;
}

// 停止日志系统
void AsyncLogger::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    condition_.notify_all();
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
    if (logFile_.is_open()) {
        logFile_.close();
    }
}

// 日志记录函数（支持可变参数）
void AsyncLogger::log(LogLevel level, const char* file, int line, const char* format, ...)
{
    if (level < currentLevel_) return;

        // 获取当前时间
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

        // 格式化日志消息
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // 构建完整日志条目
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S")
    << '.' << std::setfill('0') << std::setw(3) << ms.count()
    << "[" << levelToString(level) << "]["
    << file //std::filesystem::path(file).filename().string()  // 不可重入函数，在多线程环境下会崩溃
    << ":" << line << "] " << buffer;

        // 添加到队列
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        logQueue_.push(ss.str());
    }
    condition_.notify_one();
}

// 日志级别转字符串
const char* AsyncLogger::levelToString(LogLevel level)
{
    switch(level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
        case LogLevel::FATAL:   return "FATAL";
        default:                return "UNKNOWN";
    }
}

// 后台线程处理日志
void AsyncLogger::processLogs()
{
    while (true) {
        std::unique_lock<std::mutex> lock(queueMutex_);
        condition_.wait(lock, [this] {
            return !logQueue_.empty() || !running_;
        });

        if (!running_ && logQueue_.empty()) break;

            // 批量处理日志（减少I/O操作）
        std::vector<std::string> batch;
        while (!logQueue_.empty()) {
            batch.push_back(std::move(logQueue_.front()));
            logQueue_.pop();
        }
        lock.unlock();

            // 写入文件
        std::lock_guard<std::mutex> fileLock(mutex_);
        for (const auto& msg : batch) {
#ifdef LOG_TO_FILE
            logFile_ << msg << std::endl;
        }
        logFile_.flush();
#else
            std::cout << msg << std::endl;
        }        
#endif
    }
}

// 生成日志文件名： [PID]_[程序名].log
std::string AsyncLogger::genLogFilename()
{
    std::ostringstream oss;
    
    // 1. 获取程序名称（不含路径和扩展名）
#ifdef __linux__
    // Linux 方案：通过 /proc/self/exe 获取可执行文件路径 [3,7](@ref)
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len != -1) {
        exePath[len] = '\0';
        std::filesystem::path p(exePath);
        oss << p.stem().string();  // 移除路径和扩展名（如 /usr/bin/app → app）
    } else {
        oss << "unknown";
    }
#elif _WIN32
    // Windows 方案（兼容性处理）
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    oss << p.stem().string();
#endif
    // 2. 获取进程ID (PID) [1,8](@ref)
    oss << "_" << getpid();  // Linux/Unix 标准 API


    // 3. 组合成日志文件名
    oss << ".log";
    return oss.str();
}


void __sig_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    if (!buf || !fmt || size == 0) return;
    
    char *p = buf;
    const char *s = fmt;
    size_t remaining = size - 1;
    
    va_list args;
    // 注意：在信号处理函数中使用va_list可能不是完全安全的，
    // 但在大多数实现中，这应该是安全的
    va_start(args, fmt);
    
    while (*s && remaining > 0) {
        if (*s != '%') {
            *p++ = *s++;
            remaining--;
            continue;
        }
        
        s++;
        
        // 处理格式说明符
        switch (*s) {
            case 'd': {
                int num = va_arg(args, int);
                char num_buf[12];
                int i = 0;
                
                if (num < 0) {
                    if (remaining > 0) {
                        *p++ = '-';
                        remaining--;
                    }
                    num = -num;
                }
                
                // 转换数字
                do {
                    num_buf[i++] = (char)((num % 10) + '0');
                    num /= 10;
                } while (num > 0 && i < 11);
                
                // 输出数字
                while (i > 0 && remaining > 0) {
                    *p++ = num_buf[--i];
                    remaining--;
                }
                s++;
                break;
            }
            
            case 's': {
                const char *str = va_arg(args, const char*);
                if (!str) str = "(null)";
                
                while (*str && remaining > 0) {
                    *p++ = *str++;
                    remaining--;
                }
                s++;
                break;
            }
            
            // 其他格式可以在这里添加
            
            default:
                // 不是格式说明符，原样输出
                if (remaining > 0) {
                    *p++ = '%';
                    remaining--;
                }
                if (remaining > 0 && *s) {
                    *p++ = *s;
                    remaining--;
                }
                if (*s) s++;
                break;
        }
    }
    
    va_end(args);
    
    // 输出到终端
    if (p > buf) {
        write(STDOUT_FILENO, buf, p - buf);
    }
}
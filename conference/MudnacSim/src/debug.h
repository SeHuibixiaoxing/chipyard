#ifndef DEBUG_H
#define DEBUG_H

#include <iostream>
#include <string>
#include <unordered_set>
#include <mutex>
#include <sstream>
#include <memory>
#include <cstdarg>  // 用于可变参数
#include <cstdio>   // 用于 vsnprintf

class DebugLogger {
public:
    // 启用调试标志
    static void enable(const std::string& flag) {
        enabled_flags_.insert(flag);
    }

    // 禁用调试标志
    static void disable(const std::string& flag) {
        enabled_flags_.erase(flag);
    }

    // 检查标志是否启用
    static bool isEnabled(const std::string& flag) {
        return enabled_flags_.find(flag) != enabled_flags_.end();
    }

    // 日志流类
    class LogStream {
    public:
        LogStream(const std::string& flag, bool enabled) 
            : flag_(flag), enabled_(enabled) {
            if (enabled_) {
                stream_ = std::make_unique<std::ostringstream>();
            }
        }

        ~LogStream() {
            if (enabled_ && stream_) {
                std::cout << "[" << flag_ << "] " << stream_->str() << std::endl;
            }
        }

        // 重载 << 运算符（流式输出）
        template <typename T>
        LogStream& operator<<(const T& value) {
            if (enabled_ && stream_) {
                *stream_ << value;
            }
            return *this;
        }

        // 处理 std::endl 等操纵符
        LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
            if (enabled_ && stream_) {
                manip(*stream_);
            }
            return *this;
        }

        // printf 风格格式化输出
        void printf(const char* format, ...) {
            if (!enabled_ || !stream_) return;
            
            va_list args;
            va_start(args, format);
            
            // 确定需要的缓冲区大小
            va_list args_copy;
            va_copy(args_copy, args);
            int size = vsnprintf(nullptr, 0, format, args_copy) + 1; // +1 for '\0'
            va_end(args_copy);
            
            if (size <= 0) {
                va_end(args);
                return;
            }
            
            // 创建缓冲区并格式化
            std::unique_ptr<char[]> buf(new char[size]);
            vsnprintf(buf.get(), size, format, args);
            va_end(args);
            
            // 添加到流
            *stream_ << buf.get();
        }

    private:
        std::string flag_;
        bool enabled_;
        std::unique_ptr<std::ostringstream> stream_;
    };

    // 获取日志流
    static LogStream log(const std::string& flag) {
        return LogStream(flag, isEnabled(flag));
    }

    // printf 风格日志（直接输出）
    static void logf(const std::string& flag, const char* format, ...) {
        if (!isEnabled(flag)) return;
        
        va_list args;
        va_start(args, format);
        
        // 确定需要的缓冲区大小
        va_list args_copy;
        va_copy(args_copy, args);
        int size = vsnprintf(nullptr, 0, format, args_copy) + 1; // +1 for '\0'
        va_end(args_copy);
        
        if (size <= 0) {
            va_end(args);
            return;
        }
        
        // 创建缓冲区并格式化
        std::unique_ptr<char[]> buf(new char[size]);
        vsnprintf(buf.get(), size, format, args);
        va_end(args);
        
        // 输出带标志前缀的日志
        std::cout << "[" << flag << "] " << buf.get() << std::endl;
    }

private:
    inline static std::unordered_set<std::string> enabled_flags_;
};

// 宏定义简化使用
#define LOG(flag) DebugLogger::log(flag)
#define LOGF(flag, ...) DebugLogger::logf(flag, __VA_ARGS__)

#endif //DEBUG_H

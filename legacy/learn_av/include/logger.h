#pragma once

// 在 #include <spdlog/spdlog.h> 之前定义此宏，
// 解除预处理器对 TRACE 和 DEBUG 级别的编译期屏蔽
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE

#include <spdlog/spdlog.h>

#include <memory>

namespace av {

class Logger {
public:
    Logger() = delete;
    ~Logger() = delete;

    static void Init();
    static void Shutdown();

    inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger_; }

private:
    static std::shared_ptr<spdlog::logger> s_CoreLogger_;
};

} // namespace av

// 官方宏，自动捕获 Source Location 并支持编译期级别裁剪
#define LOG_TRACE(...) SPDLOG_LOGGER_TRACE(::av::Logger::GetCoreLogger(), __VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(::av::Logger::GetCoreLogger(), __VA_ARGS__)
#define LOG_INFO(...) SPDLOG_LOGGER_INFO(::av::Logger::GetCoreLogger(), __VA_ARGS__)
#define LOG_WARN(...) SPDLOG_LOGGER_WARN(::av::Logger::GetCoreLogger(), __VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_ERROR(::av::Logger::GetCoreLogger(), __VA_ARGS__)
#define LOG_FATAL(...) SPDLOG_LOGGER_CRITICAL(::av::Logger::GetCoreLogger(), __VA_ARGS__)
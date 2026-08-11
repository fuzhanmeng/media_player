/**
 * @file logger.h
 * @brief 封装的 spdlog 的全局共享的一个日志接口
 * @details
 * Logger 是进程级服务，main 函数入口会进行初始化，UI 模块和 mediacore 模块通过 LOG_* 宏记录日志
 * spdlog 负责记录源码文件、行号、函数名、线程id、日志级别以及日志消息
 */
#pragma once

#include <spdlog/spdlog.h>

#include <memory>

namespace mediacore {

/**
 * @class Logger
 * @brief app and mediacore 共享的 log
 *
 * 该类不可实例化，Init() 和 Shutdown() 在实现中由内部互斥锁保护，实际写日志由 spdlog 的多线程 sink 负责
 */
class Logger final {
public:
    Logger() = delete;
    ~Logger() = delete;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief 初始化控制台日志和滚动日志文件
     * @details
     * 该函数是幂等的，如果初始化失败，需要回到控制台输出，不让应用启动因为日志失败而直接中断
     */
    static void Init();

    /**
     * @brief 刷新并关闭日志系统
     * @details
     * 该函数是幂等的，如果析构函数需要写日志，需要在对象被销毁后调用
     */
    static void Shutdown();

    /**
     * @brief 返回当前可用的 logger，如果尚未初始化，则返回空操作
     * @details
     * 如果 Init() 已经成功调用，返回真正写到控制台/文件的核心 logger；
     * 如果尚未初始化或已经 Shutdown()，返回空操作兜底 logger
     *
     * 返回 shared_ptr 的原因是让一次日志调用期间保持 logger 存活：
     * 调用线程拿到返回值后，即使另一个线程马上 Shutdown()，当前这次写日志也不会悬空
     */
    static std::shared_ptr<spdlog::logger> GetLogger();
};

}  // namespace mediacore

// 使用 spdlog 的官方宏，保证源码位置捕获和编译期级别裁剪都能正常工作
// LOG_INFO(...) -> SPDLOG_LOGGER_INFO(...) -> Logger::GetLogger() -> spdlog::logger -> sinks
// TRACE/DEBUG 是否参与编译由 CMake 里的 SPDLOG_ACTIVE_LEVEL 决定
#define LOG_TRACE(...) SPDLOG_LOGGER_TRACE(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_INFO(...) SPDLOG_LOGGER_INFO(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_WARN(...) SPDLOG_LOGGER_WARN(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_ERROR(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_FATAL(...) SPDLOG_LOGGER_CRITICAL(::mediacore::Logger::GetLogger(), __VA_ARGS__)

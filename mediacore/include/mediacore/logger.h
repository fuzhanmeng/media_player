/**
 * @file logger.h
 * @brief 基于 spdlog 封装的进程级日志接口
 * @details
 * Logger 是进程级服务，由 main 函数入口负责初始化，UI 模块和 mediacore 模块统一通过 LOG_* 宏记录日志
 * spdlog 负责记录源码文件、行号、函数名、线程 id、日志级别以及日志正文
 */
#pragma once

#include <spdlog/spdlog.h>

#include <memory>

namespace mediacore {

/**
 * @class Logger
 * @brief app 与 mediacore 共享的日志入口
 * @details
 * 该类不可实例化，只提供静态接口；Init() 和 Shutdown() 内部由互斥锁保护，
 * 真正写日志由 spdlog 的多线程 sink 负责，Logger 本身不持有可写状态
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
     * 该函数是幂等的，重复调用直接返回；文件 sink 创建失败时会回退为只输出到控制台，
     * 不让应用启动因为日志初始化失败而中断
     */
    static void Init();

    /**
     * @brief 刷新并关闭日志系统
     * @details
     * 该函数是幂等的，需要在其他对象析构、可能还会写日志的最后阶段调用
     */
    static void Shutdown();

    /**
     * @brief 返回当前可用的 logger
     * @details
     * Init() 已成功调用时返回真正写到控制台/文件的核心 logger；
     * 尚未初始化或已经 Shutdown() 时返回只接收不输出的兜底 logger
     *
     * 返回 shared_ptr 而不是裸指针，是为了让调用线程在一次日志写入期间保持 logger 存活：
     * 即使写入过程中另一个线程执行了 Shutdown()，当前这次调用也不会访问已销毁的对象
     */
    static std::shared_ptr<spdlog::logger> GetLogger();
};

}  // namespace mediacore

// 使用 spdlog 官方宏，保证源码位置捕获和编译期级别裁剪都能正常工作
// LOG_INFO(...) -> SPDLOG_LOGGER_INFO(...) -> Logger::GetLogger() -> spdlog::logger -> sinks
// TRACE/DEBUG 是否参与编译由 CMake 里的 SPDLOG_ACTIVE_LEVEL 决定
#define LOG_TRACE(...) SPDLOG_LOGGER_TRACE(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_INFO(...) SPDLOG_LOGGER_INFO(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_WARN(...) SPDLOG_LOGGER_WARN(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_ERROR(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_FATAL(...) SPDLOG_LOGGER_CRITICAL(::mediacore::Logger::GetLogger(), __VA_ARGS__)

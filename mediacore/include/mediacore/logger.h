/**
 * @file logger.h
 * @brief 基于 spdlog 高性能异步库封装的进程级日志系统接口
 * @details
 * 负责为整个 mediacore 核心引擎与 UI 表现层提供统一的日志门面：
 * 1. 进程级单例服务：在 main() 入口最先完成引导，退出时统筹刷新并关闭；
 * 2. 线程安全与生命周期保障：GetLogger() 返回 std::shared_ptr<spdlog::logger> 快照，
 *    确保即使在写入中途触发了 Shutdown()，正在执行的日志写入任务仍能安全完成；
 * 3. 宏展开零开销抽象：通过包装 SPDLOG_LOGGER_* 系列宏，保证文件名、代码行号、
 *    函数名的编译期无损捕获，并支持通过 SPDLOG_ACTIVE_LEVEL 在 Release 编译期彻底裁剪低优先级日志。
 */

#pragma once

#include <spdlog/spdlog.h>

#include <memory>

namespace mediacore {

/**
 * @class Logger
 * @brief 进程级共享日志核心静态服务门面
 * @details 该类为纯静态工具类，不可实例化或被继承；底层的多线程安全由 spdlog 的线程安全 Sink 机制保证。
 */
class Logger final {
public:
    // 纯静态类：严禁构造、析构、拷贝与移动
    Logger() = delete;
    ~Logger() = delete;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    /**
     * @brief 初始化全局日志核心配置（控制台彩色输出与滚动日志文件落盘）
     * @details
     * 1. 具备幂等性（Idempotent），多次重复调用直接安全返回；
     * 2. 具备自动降级容错机制：当磁盘文件 Sink 因权限或路径原因创建失败时，
     *    自动回退至仅控制台输出，确保上层应用不会因日志故障引发崩溃。
     */
    static void Init();

    /**
     * @brief 强制刷新所有 Sink 缓冲区并优雅关闭日志子系统
     * @details
     * 1. 具备幂等性，通常在 main() 退出前的最终资源回收阶段显式调用；
     * 2. 关闭后内部切换为兜底空实现（Null Sink），后续遗留的日志写入不会产生崩溃或异常。
     */
    static void Shutdown();

    /**
     * @brief 获取当前可用的核心 spdlog::logger 实例
     * @return 托管日志器生命周期的共享智能指针
     * @details
     * 生命周期保护设计：
     * - 当 Init() 成功时，返回配置有多 Sink 输出的核心实例；
     * - 当尚未初始化或处于已注销状态时，返回兜底的空 Sink 日志器；
     * - 通过返回值拷贝递增引用计数，规避跨线程并发 Shutdown 时访问已析构对象的悬空风险。
     */
    static std::shared_ptr<spdlog::logger> GetLogger();
};

}  // namespace mediacore

/**
 * @name 日志格式化宏定义
 * @brief 映射至 spdlog 官方核心宏，保障代码位置自动捕获与编译期静态剥离
 * @details
 * 展开路径：LOG_INFO(...) -> SPDLOG_LOGGER_INFO(Logger::GetLogger(), __VA_ARGS__)
 * 若编译期 SPDLOG_ACTIVE_LEVEL 高于对应级别，对应宏将在预处理阶段展开为空，实现运行期零开销。
 * @{
 */
#define LOG_TRACE(...) SPDLOG_LOGGER_TRACE(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_INFO(...) SPDLOG_LOGGER_INFO(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_WARN(...) SPDLOG_LOGGER_WARN(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_LOGGER_ERROR(::mediacore::Logger::GetLogger(), __VA_ARGS__)
#define LOG_FATAL(...) SPDLOG_LOGGER_CRITICAL(::mediacore::Logger::GetLogger(), __VA_ARGS__)
/** @} */
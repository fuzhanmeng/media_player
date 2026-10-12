/**
 * @file logger.cpp
 * @brief 基于 spdlog 的多线程安全、进程级日志系统实现
 * @details
 * 负责创建、配置、访问及安全释放项目全局 Logger。核心设计如下：
 * 1. 独立解耦：纯 C++11/17 与 spdlog 实现，不依赖 Qt 或 UI 框架，可在命令行工具、测试套件中无缝复用；
 * 2. 线程安全（_mt）：使用 multi-threaded 版本的 Sink，保障 demux、decode、render 等流水线多线程并发写日志的安全；
 * 3. 多路输出分发：同时挂载控制台彩色 Sink（开发调试）与滚动文件 Sink（持久化追踪）；
 * 4. 空操作兜底（Fallback Null Sink）：Init() 之前或 Shutdown() 之后调用 LOG_* 不会产生空指针解引用崩溃；
 * 5. 优雅退出防竞争：通过 std::shared_ptr 浅拷贝延长生命周期，避免写日志线程与 Shutdown() 发生 Use-After-Free。
 */

#include "mediacore/logger.h"

#include <spdlog/sinks/null_sink.h>  // 安全接收日志并丢弃，用于兜底
#include <spdlog/sinks/rotating_file_sink.h>  // 滚动文件日志，超出配额后自动切分保留
#include <spdlog/sinks/stdout_color_sinks.h>  // 控制台彩色输出

#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mediacore {
namespace {

constexpr const char* kLoggerName = "AI_MEDIA_PLAYER";
constexpr const char* kFallbackLoggerName = "AI_MEDIA_PLAYER_NULL";
constexpr const char* kLogDirectory = "logs";
constexpr const char* kLogFilePath = "logs/ai_media_player.log";

// 滚动日志文件大小及数量配额（单文件最大 10 MiB，保留 5 个轮转历史，防止占满磁盘）
constexpr std::size_t kMaxLogFileSizeBytes = 10 * 1024 * 1024;
constexpr std::size_t kMaxRotatingFiles = 5;

// 全局互斥锁：仅用于保护 g_core_logger 和 g_fallback_logger 的创建、获取和释放
// 实际执行写日志时不持有该锁，避免业务线程在日志输出上发生串行争锁
std::mutex g_logger_mutex;

// 业务主 Logger：Init() 成功后构建，同时分发到控制台和文件
std::shared_ptr<spdlog::logger> g_core_logger;

// 兜底 Logger：在 Init() 之前或 Shutdown() 之后生效，丢弃所有输入，保障调用安全
std::shared_ptr<spdlog::logger> g_fallback_logger;

/**
 * @brief 创建或获取空操作兜底 Logger（单例模式）
 * @note 调用方必须已持有 g_logger_mutex；该函数专供内部锁内逻辑调用
 * @return 指向静音 Logger 的 shared_ptr 句柄
 */
std::shared_ptr<spdlog::logger> GetFallbackLoggerLocked() {
    if (!g_fallback_logger) {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        g_fallback_logger = std::make_shared<spdlog::logger>(kFallbackLoggerName, std::move(null_sink));
        g_fallback_logger->set_level(spdlog::level::off);
    }
    return g_fallback_logger;
}

}  // namespace

/**
 * @brief 初始化进程级日志系统
 * @details
 * 1. 构建控制台彩色输出端（stderr_color_sink_mt）；
 * 2. 尝试创建目标目录 logs/ 并挂载滚动文件输出端（rotating_file_sink_mt）；
 * 3. 注册为 spdlog 全局默认 Logger，并设置刷新水位线与运行期过滤级别；
 * 4. 线程安全：内部加锁，支持重复调用或并发调用（首次调用生效，后续调用幂等跳过）。
 *
 * @note 源码定位（%s:%# / %!）需配合 SPDLOG_LOGGER_* 宏才能生效，请优先使用 LOG_* 宏而非原生接口。
 */
void Logger::Init() {
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    // 幂等保护：已初始化则直接返回，避免重复创建 Sink
    if (g_core_logger) {
        return;
    }

    std::vector<spdlog::sink_ptr> sinks;

    // 配置控制台彩色 Sink
    // 字段说明：[%Y-%m-%d %H:%M:%S.%e] 时间戳 | [thread %t] 线程ID | [%n] Logger名
    //           [%s:%#] 源码文件名与行号 | [%!] 函数名 | [%^%l%$] 彩色日志级别 | %v 日志内容
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%n] [%s:%#] [%!] [%^%l%$] %v");
    sinks.emplace_back(console_sink);

    // 配置文件持久化 Sink：优先通过 noexcept 重载创建目录，避免目录异常中断应用启动
    std::string file_sink_error;
    std::error_code directory_error;
    std::filesystem::create_directories(kLogDirectory, directory_error);

    if (directory_error) {
        file_sink_error =
            std::string("failed to create log directory '") + kLogDirectory + "': " + directory_error.message();
    } else {
        try {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                kLogFilePath, kMaxLogFileSizeBytes, kMaxRotatingFiles, true);

            // 文件输出无需终端 ANSI 颜色转义字符，级别字段采用标准 [%l]
            file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%n] [%s:%#] [%!] [%l] %v");
            sinks.emplace_back(std::move(file_sink));
        } catch (const std::exception& error) {
            file_sink_error = error.what();
        }
    }

    // 组合全部 Sink 并构建主 Logger
    auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());

    // 设定运行时过滤级别（注：SPDLOG_ACTIVE_LEVEL 负责编译期裁剪，set_level 负责运行期过滤）
    logger->set_level(spdlog::level::info);

    // 设定自动刷新级别：达到 ERROR 及以上时立即强制 flush 到磁盘
    logger->flush_on(spdlog::level::err);

    // 清理旧同名项，保障测试用例重复 Init/Shutdown 时的确定性
    spdlog::drop(kLoggerName);

    // 注册到全局注册表并设为默认 Logger，便于依赖库或未传 logger 句柄的地方复用
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    g_core_logger = std::move(logger);

    // 若文件日志创建失败，记录警告通知
    // 此处禁止使用外层 LOG_WARN(...)，因其会再次进入 GetLogger() 导致对 g_logger_mutex 的重入死锁
    if (!file_sink_error.empty()) {
        SPDLOG_LOGGER_WARN(g_core_logger, "File logging is disabled: {}", file_sink_error);
    }

    SPDLOG_LOGGER_INFO(g_core_logger, "Logger initialized");
}

/**
 * @brief 关闭并释放日志系统
 * @details
 * 采用“两阶段析构”模式：
 * 1. 锁内：通过 std::move 取走 g_core_logger 所有权并置空全局指针；
 * 2. 锁外：执行最后的关闭日志输出、flush 刷盘、drop 注销与 spdlog 资源回收；
 * 有效防止写最后一条日志或注销 Hook 时由于嵌套获取 g_logger_mutex 导致死锁。
 */
void Logger::Shutdown() {
    std::shared_ptr<spdlog::logger> logger_to_shutdown;

    {
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        logger_to_shutdown = std::move(g_core_logger);
        g_core_logger.reset();
    }

    if (logger_to_shutdown) {
        // 使用 SPDLOG_LOGGER_INFO 确保保留关闭动作触发时的文件名与行号
        SPDLOG_LOGGER_INFO(logger_to_shutdown, "Logger shutting down");
        logger_to_shutdown->flush();
    }

    // 从 spdlog 内部注册表中注销并完全释放底层 Sink 资源
    spdlog::drop(kLoggerName);
    spdlog::shutdown();
}

/**
 * @brief 获取当前可用的 Logger 实例
 * @details
 * 1. Init() 成功后返回 g_core_logger；Init() 前或 Shutdown() 后返回 g_fallback_logger；
 * 2. 返回 std::shared_ptr 值拷贝：写日志的调用线程会在调用栈内维持一次强引用计数，
 *    即使并发的后台线程同时调用了 Shutdown()，正在写日志的线程也绝不会访问已销毁的野指针（防 UAF）。
 *
 * @return 保证非空的 spdlog::logger 智能指针实例
 */
std::shared_ptr<spdlog::logger> Logger::GetLogger() {
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    if (g_core_logger) {
        return g_core_logger;
    }

    return GetFallbackLoggerLocked();
}

}  // namespace mediacore
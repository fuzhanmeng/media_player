/**
 * @file logger.cpp
 * @brief 基于 spdlog 的项目级 logger 实现
 * @details
 * 该文件负责创建、保存、关闭进程级 logger，实现不依赖 Qt，因此 mediacore 可以在命令行测试、
 * 后续工具程序以及非 Qt 环境中复用
 *
 * spdlog 中 logger 和 sink 的关系：logger 是日志入口，LOG_INFO(...) 最终会进入一个
 * spdlog::logger；sink 是日志的出口，负责把日志写到具体位置，例如终端或 xxx.log 文件
 *
 * 本实现使用三种 sink：
 * - stdout_color_sink_mt：控制台彩色输出，用于开发时直接观察
 * - rotating_file_sink_mt：滚动文件输出，用于保存历史日志
 * - null_sink_mt：空输出，用于 Init() 前或 Shutdown() 后的兜底
 *
 * 后缀 _mt 表示 multi-threaded。播放器后续会有 demux / decode / render 等多个线程同时写日志，
 * 因此这里统一使用线程安全 sink
 */
#include "mediacore/logger.h"

#include <spdlog/sinks/null_sink.h>  // 安全接收日志但不输出
#include <spdlog/sinks/rotating_file_sink.h>  // 文件日志使用，超过大小上限后自动轮转
#include <spdlog/sinks/stdout_color_sinks.h>  // 控制台日志使用，支持彩色级别字段

#include <exception>  // 捕获创建文件 sink 时可能抛出的 std::exception
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

// 文件 sink 的轮转策略：单个日志文件最大 10 MiB，最多保留 5 个旧文件
// 这样长期运行时日志不会无限增长把磁盘占满
constexpr std::size_t kMaxLogFileSizeBytes = 10 * 1024 * 1024;
constexpr std::size_t kMaxRotatingFiles = 5;

// 这把锁只保护 g_core_logger / g_fallback_logger 两个 shared_ptr 的创建、替换和读取
// 真正写日志时不持有这把锁，避免所有线程都在日志调用入口上串行等待
std::mutex g_logger_mutex;

// 真正的项目 logger：Init() 成功后创建，写到控制台和文件
std::shared_ptr<spdlog::logger> g_core_logger;

// 兜底 logger：Init() 之前或 Shutdown() 之后使用，只接收日志但不输出
// 它不是第二套业务日志系统，只是为了让 LOG_* 在生命周期边界也不会因为空指针崩溃
std::shared_ptr<spdlog::logger> g_fallback_logger;

/**
 * @brief 返回空操作兜底 logger
 * @details
 * 调用方必须已经持有 g_logger_mutex，这个函数只在 GetLogger() 内部使用
 * null_sink_mt 会安全接收日志并直接丢弃，因此 Init() 前或 Shutdown() 后调用 LOG_* 也是安全的
 *
 * @return 指向空操作 logger 的 shared_ptr
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
 * @brief 初始化项目级 logger
 * @details
 * 该函数创建控制台 sink 和滚动文件 sink，然后把它们挂到同一个 spdlog::logger 上
 * 后续 LOG_* 调用进入这个 logger 后，spdlog 会把同一条日志分发给所有 sink
 *
 * 日志格式中的 %s:%# 和 %! 依赖 SPDLOG_LOGGER_* 宏捕获源码位置
 * 因此业务代码要使用 LOG_INFO(...) 这类宏；如果直接调用 logger->info(...)，
 * spdlog 就没有调用点信息，文件名、行号和函数名会为空或无意义
 *
 * 线程安全：内部使用 g_logger_mutex 保护全局 logger 指针，重复调用是安全的
 */
void Logger::Init() {
    // Init() / Shutdown() / GetCoreLogger() 都会访问同一组全局 shared_ptr
    // 这里加锁，保证重复 Init() 或多个线程同时初始化时只会创建出一个 logger
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    if (g_core_logger) {
        return;
    }

    std::vector<spdlog::sink_ptr> sinks;
    // pattern 里的关键字段：
    // - %t：线程 id，用来区分 UI、解封装、解码等线程
    // - %n：logger 名称，这里固定为 AI_MEDIA_PLAYER
    // - %s:%# / %!：源码文件:行号 / 函数名，来自 LOG_* 宏捕获的调用点
    // - %^ / %$：只对控制台有意义，包住需要上色的字段
    // - %v：用户传给 LOG_INFO(...) 的正文
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%n] [%s:%#] [%!] [%^%l%$] %v");
    sinks.emplace_back(console_sink);

    // 文件 sink 需要先保证 logs/ 目录存在
    // 这里用 error_code 版本，避免目录创建失败时直接抛出异常中断应用启动
    std::string file_sink_error;
    std::error_code directory_error;
    std::filesystem::create_directories(kLogDirectory, directory_error);

    if (directory_error) {
        file_sink_error =
            std::string("failed to create log directory '") + kLogDirectory + "': " + directory_error.message();
    } else {
        try {
            // 滚动文件 sink：适合长期运行
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                kLogFilePath, kMaxLogFileSizeBytes, kMaxRotatingFiles, true);

            // 文件日志不需要控制台颜色控制码，所以这里用普通的 [%l]
            file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%n] [%s:%#] [%!] [%l] %v");

            sinks.emplace_back(std::move(file_sink));
        } catch (const std::exception& error) {
            file_sink_error = error.what();
        }
    }

    // 一个 logger 可以挂多个 sink，后续 LOG_* 调用进入这个 logger 后，spdlog 会把同一条日志
    // 分发给 sinks 里的每一个输出端
    auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());

    // 运行期日志级别，和 CMake 里的 SPDLOG_ACTIVE_LEVEL 不是一回事：
    // - SPDLOG_ACTIVE_LEVEL 决定 TRACE/DEBUG 调用是否被编译进二进制
    // - set_level 决定已经编译进去的日志在运行时是否输出
    logger->set_level(spdlog::level::info);

    // error 级以上立即 flush
    logger->flush_on(spdlog::level::err);

    // 注册这个进程级 logger 之前先清掉旧的注册项，这样测试里反复 Init()/Shutdown()/Init() 时行为更可预测
    spdlog::drop(kLoggerName);

    // register_logger 让这个 logger 进入 spdlog 的全局注册表
    // set_default_logger 让直接使用 spdlog 默认接口的第三方代码也能走同一个 logger
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    g_core_logger = std::move(logger);

    if (!file_sink_error.empty()) {
        // Logger 内部不能用 LOG_WARN(...)，因为 Init() 此时仍持有 g_logger_mutex
        // 直接用 SPDLOG_LOGGER_WARN 仍会捕获当前源码位置，但不会再调用 GetLogger() 抢同一把锁
        SPDLOG_LOGGER_WARN(g_core_logger, "File logging is disabled: {}", file_sink_error);
    }

    SPDLOG_LOGGER_INFO(g_core_logger, "Logger initialized");
}

/**
 * @brief 关闭 logger
 * @details
 * 关闭顺序是：先在锁内取走并清空 g_core_logger，再在锁外写关闭日志、flush、drop 和 shutdown，
 * 避免关闭日志本身还要获取同一把锁
 */
void Logger::Shutdown() {
    std::shared_ptr<spdlog::logger> logger_to_shutdown;

    {
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        logger_to_shutdown = std::move(g_core_logger);
        g_core_logger.reset();
    }

    if (logger_to_shutdown) {
        // 这里使用 SPDLOG_LOGGER_INFO 而不是 logger_to_shutdown->info(...)，
        // 目的是让关闭日志也带上 logger.cpp 的文件名、行号和函数名
        SPDLOG_LOGGER_INFO(logger_to_shutdown, "Logger shutting down");
        logger_to_shutdown->flush();
    }

    // 从 spdlog 全局注册表中移除项目 logger，并释放其内部资源
    spdlog::drop(kLoggerName);
    spdlog::shutdown();
}

/**
 * @brief 返回当前日志入口
 * @details
 * Init() 成功后返回 g_core_logger；Init() 前或 Shutdown() 后返回 g_fallback_logger
 * 返回 shared_ptr 而不是裸指针，是为了让调用线程在一次日志写入期间保持 logger 存活：
 * 即使另一个线程随后调用了 Shutdown()，当前这次写日志也不会访问已销毁的对象
 *
 * @return 当前可用的 spdlog logger
 */
std::shared_ptr<spdlog::logger> Logger::GetLogger() {
    std::lock_guard<std::mutex> lock(g_logger_mutex);

    if (g_core_logger) {
        return g_core_logger;
    }

    return GetFallbackLoggerLocked();
}
}  // namespace mediacore

#include "logger.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace av {

// 初始化静态成员变量
std::shared_ptr<spdlog::logger> Logger::s_CoreLogger_;

void Logger::Init() {
    if (s_CoreLogger_ != nullptr) {
        return;
    }
    // 1. 控制台 sink
    auto console_sink_ = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    // 【修改点 1】：加入 [%s:%#] (文件名:行号) 和 [%!] (函数名) 占位符
    console_sink_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%s:%#] [%!] [%^%l%$] %v");

    // 2. 文件 sink
    auto file_sink_ = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/ai_media_player.log", true);

    // 同步更新文件 sink 的格式化字符串
    file_sink_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%s:%#] [%!] [%l] %v");

    // 3. sink 集合
    std::vector<spdlog::sink_ptr> sinks{console_sink_, file_sink_};

    // 4. 创建 logger
    s_CoreLogger_ = std::make_shared<spdlog::logger>("AV_CORE", sinks.begin(), sinks.end());

    // 5. 设置日志级别
    s_CoreLogger_->set_level(spdlog::level::trace);

    // 6. err 时立即 flush
    s_CoreLogger_->flush_on(spdlog::level::err);

    // 7. 注册 logger
    spdlog::register_logger(s_CoreLogger_);
}

} // namespace av
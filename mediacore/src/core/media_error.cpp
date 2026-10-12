/**
 * @file media_error.cpp
 * @brief 项目统一结构化错误类型 MediaError 的具体实现与字符串序列化
 * @details
 * 负责各层底层错误到统一领域错误对象的转换与适配：
 * 1. 依赖隔离设计：将 FFmpeg 专属头文件（<libavutil/error.h>）严格限制在当前实现文件内，
 *    上层头文件 mediacore/media_error.h 保持纯粹，不向外界暴露第三方库的宏与符号定义；
 * 2. 错误码可读化：在工厂创建阶段完成数字错误码（FFmpeg 负数错误码、POSIX errno）向人类可读文本的翻译；
 * 3. 统一序列化：提供紧凑且信息完备的 ToString() 输出，便于诊断日志记录与测试断言。
 */

#include "mediacore/media_error.h"

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace mediacore {

namespace {

/**
 * @brief 将 FFmpeg 底层数字错误码转换为人类可读的文字描述
 * @param code FFmpeg 负数错误码（如 AVERROR(EINVAL)、AVERROR_EOF）
 * @return 解析后的错误描述文本；若无法识别则附带原始数值作为兜底
 */
std::string FfmpegErrorToString(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    const int result = av_strerror(code, buffer.data(), buffer.size());

    if (result < 0) {
        return "unknown FFmpeg error (" + std::to_string(code) + ")";
    }

    return buffer.data();
}

/**
 * @brief 将操作系统底层错误码转换为标准描述
 * @param code 系统 errno 错误数值
 * @return 标准系统错误描述字符串
 */
std::string SystemErrorToString(int code) {
    if (code == 0) {
        return {};
    }

    return std::error_code(code, std::generic_category()).message();
}

/**
 * @brief 合并调用方上下文提示文本与底层错误详细描述
 * @param message 业务层上下文提示（例如 "open input failed"）
 * @param detail 底层库翻译出的错误详情（例如 "No such file or directory"）
 * @return 拼接后的格式化说明："<message>: <detail>"
 */
std::string JoinMessageAndDetail(std::string_view message, const std::string& detail) {
    if (detail.empty()) {
        return std::string(message);
    }

    if (message.empty()) {
        return detail;
    }

    return std::string(message) + ": " + detail;
}

}  // namespace

/**
 * @brief 将错误分类枚举转换为字符串常量
 * @param category 错误类别枚举
 * @return 静态字符串常量指针，生命周期贯穿程序全程，无需调用方释放
 */
const char* MediaErrorCategoryToString(MediaErrorCategory category) noexcept {
    switch (category) {
        case MediaErrorCategory::kNone:
            return "None";
        case MediaErrorCategory::kFfmpeg:
            return "FFmpeg";
        case MediaErrorCategory::kSystem:
            return "System";
        case MediaErrorCategory::kIo:
            return "IO";
        case MediaErrorCategory::kState:
            return "State";
        case MediaErrorCategory::kInternal:
            return "Internal";
    }
    return "Unknown";
}

/**
 * @brief 工厂方法：基于 FFmpeg 错误码构造 MediaError
 * @param module 发生错误的子系统或组件模块名
 * @param code FFmpeg 原始错误码
 * @param message 业务层补充上下文说明
 * @param recoverable 错误是否可通过重试或降级恢复
 * @return 填充完整的 MediaError 实例
 */
MediaError MediaError::FromFfmpeg(std::string module, int code, std::string_view message, bool recoverable) {
    return MediaError(MediaErrorCategory::kFfmpeg,
                      std::move(module),
                      code,
                      JoinMessageAndDetail(message, FfmpegErrorToString(code)),
                      recoverable);
}

/**
 * @brief 工厂方法：基于操作系统 errno 构造 MediaError
 * @param module 发生错误的子系统或组件模块名
 * @param code 操作系统原生错误码（errno）
 * @param message 业务层补充上下文说明
 * @param recoverable 错误是否可恢复
 * @return 填充完整的 MediaError 实例
 */
MediaError MediaError::FromSystem(std::string module, int code, std::string_view message, bool recoverable) {
    return MediaError(MediaErrorCategory::kSystem,
                      std::move(module),
                      code,
                      JoinMessageAndDetail(message, SystemErrorToString(code)),
                      recoverable);
}

/**
 * @brief 工厂方法：构造文件读取、URL 连接或网络 I/O 相关错误
 * @param module 发生错误的子系统或组件模块名
 * @param code 原始底层错误代码；若无具体代码可传 0
 * @param message 错误上下文描述
 * @param recoverable 错误是否可恢复（如网络瞬时波动重试）
 * @return 填充完整的 MediaError 实例
 */
MediaError MediaError::FromIo(std::string module, int code, std::string_view message, bool recoverable) {
    return MediaError(MediaErrorCategory::kIo,
                      std::move(module),
                      code,
                      JoinMessageAndDetail(message, SystemErrorToString(code)),
                      recoverable);
}

/**
 * @brief 工厂方法：构造管线或节点状态机流转非法错误（默认不可恢复）
 * @param module 发生错误的子系统或组件模块名
 * @param message 错误上下文描述
 * @return 填充完整的 MediaError 实例
 */
MediaError MediaError::FromState(std::string module, std::string message) {
    return MediaError(MediaErrorCategory::kState, std::move(module), 0, std::move(message), false);
}

/**
 * @brief 工厂方法：构造框架内部逻辑缺陷或断言失败错误（不可恢复）
 * @param module 发生错误的子系统或组件模块名
 * @param message 错误描述文本
 * @return 填充完整的 MediaError 实例
 */
MediaError MediaError::FromInternal(std::string module, std::string message) {
    return MediaError(MediaErrorCategory::kInternal, std::move(module), 0, std::move(message), false);
}

/**
 * @brief 将错误信息序列化为便于诊断与日志记录的结构化文本
 * @return 包含类别、模块名、原始代码、可恢复性标志与说明的完整字符串
 */
std::string MediaError::ToString() const {
    if (!HasError()) {
        return "MediaError{category=None}";
    }

    std::ostringstream stream;
    stream << "MediaError{category=" << MediaErrorCategoryToString(category_)
           << ", module=" << module_
           << ", code=" << code_
           << ", recoverable=" << (recoverable_ ? "true" : "false")
           << ", message=\"" << message_ << "\"}";
    return stream.str();
}

/**
 * @brief 私有核心构造函数
 * @param category 错误大类分类
 * @param module 模块名称
 * @param code 原生错误代码
 * @param message 已经完成格式化组装的完整描述
 * @param recoverable 是否可恢复
 * @note 仅由内部工厂方法调用，确保字段一致性
 */
MediaError::MediaError(MediaErrorCategory category,
                       std::string module,
                       int code,
                       std::string message,
                       bool recoverable)
    : category_(category),
      module_(std::move(module)),
      code_(code),
      message_(std::move(message)),
      recoverable_(recoverable) {}

}  // namespace mediacore
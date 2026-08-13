/**
 * @file media_error.cpp
 * @brief MediaError 实现和字符串转换
 * @details
 * FFmpeg 错误码的转换放在这里，避免公共头文件暴露 FFmpeg 头
 */

#include "mediacore/media_error.h"

#include <array>
#include <system_error>
#include <sstream>
#include <string_view>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace mediacore {

namespace {

/**
 * @brief 把 FFmpeg 负数错误码转换为可读文本
 * @param code FFmpeg API 返回的错误码
 * @return std::string 可读错误文本，转换失败时返回 fallback 文本
 */
std::string FfmpegErrorToString(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    const int result = av_strerror(code, buffer.data(), buffer.size());

    if (result < 0) {
        return "unknown FFmpeg error";
    }

    return buffer.data();
}

/**
 * @brief 把系统错误码转成可读文本
 * @param code errno 或类似系统错误码
 * @return 可读错误文本
 */
std::string SystemErrorToString(int code) {
    if (code == 0) {
        return {};
    }

    return std::error_code(code, std::generic_category()).message();
}

/**
 * @brief 拼接调用方补充说明和底层错误文本
 * @param message 调用方自己写的说明，比如 "open input failed"
 * @param detail 底层库返回的可读错误文本，比如 "No such file or directory"
 * @return 合并后的错误说明
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
 * @brief 把错误类别转换为日志可读的字符串
 * @param category  要转换的错误类别
 * @return 静态字符串，调用方不需要释放
 * @details
 * 此函数处理错误类别到字符串的转换，日志输出时不需要在重写switch，返回的是静态字符串，不会产生额外的动态内存
 */
const char* MediaErrorCategoryToString(MediaErrorCategory category) {
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
 * @brief 根据 FFmpeg 错误码创建错误对象
 * @param module 发生错误的模块名
 * @param code FFmpeg API 返回的错误码
 * @param message 调用方补充的错误信息
 * @param recoverable true 表示上层可以尝试恢复错误，false 表示此错误无法恢复
 * @return FFmpeg 错误文本的 MediaError 对象
 *
 * @details
 * FFmpeg 的错误码通常是负数，单独保存数字不方便排查问题
 * 这里在创建错误对象时调用 FfmpegErrorToString()，把数字错误码转换成可读文本，
 */
MediaError MediaError::FromFfmpeg(std::string module, int code, std::string_view message, bool recoverable) {
    return MediaError(MediaErrorCategory::kFfmpeg,
                      std::move(module),
                      code,
                      JoinMessageAndDetail(message, FfmpegErrorToString(code)),
                      recoverable);
}

/**
 * @brief 根据系统错误码创建错误对象
 * @param module 发生错误的模块名
 * @param code errno 或类似系统错误码
 * @param message 可读错误说明
 * @param recoverable true 表示上层可以尝试恢复错误，false 表示此错误无法恢复
 * @return 系统错误MediaError 对象
 */
MediaError MediaError::FromSystem(std::string module, int code, std::string_view message, bool recoverable) {
    return MediaError(MediaErrorCategory::kSystem,
                      std::move(module),
                      code,
                      JoinMessageAndDetail(message, SystemErrorToString(code)),
                      recoverable);
}

/**
 * @brief 创建文件、URL 或网络读写错误
 * @param module 发生错误的模块名
 * @param code 原始错误码；没有错误码时可以传 0
 * @param message 可读错误说明
 * @param recoverable true 表示上层可以尝试恢复错误，false 表示此错误无法恢复
 * @return IO 错误文本的 MediaError 对象
 */
MediaError MediaError::FromIo(std::string module, int code, std::string_view message, bool recoverable) {
    return MediaError(MediaErrorCategory::kIo,
                      std::move(module),
                      code,
                      JoinMessageAndDetail(message, SystemErrorToString(code)),
                      recoverable);
}

/**
 * @brief 创建状态错误
 * @param module 发生错误的模块名
 * @param message 调用方补充的错误信息
 * @return 状态错误的 MediaError 对象
 */
MediaError MediaError::FromState(std::string module, std::string message) {
    return MediaError(MediaErrorCategory::kState, std::move(module), 0, std::move(message), false);
}

/**
 * @brief 创建内部错误
 * @param module 发生错误的模块名
 * @param message 可读错误说明
 * @return MediaError
 */
MediaError MediaError::FromInternal(std::string module, std::string message) {
    return MediaError(MediaErrorCategory::kInternal, std::move(module), 0, std::move(message), false);
}

/**
 * @brief 拼接适合日志输出的完整错误文本
 * @return 包含类别、模块、错误码和说明的字符串
 * @details
 * 该函数只负责组装文本，不负责写日志
 * 这样调用方可以根据需要写入日志、测试输出，或者显示给上层界面
 */
std::string MediaError::ToString() const {
    if (!HasError()) {
        return "MediaError Category = None";
    }

    std::ostringstream stream;
    stream << "MediaError{category=" << MediaErrorCategoryToString(category_) << ", module=" << module_
           << ", code=" << code_ << ", recoverable=" << (recoverable_ ? "true" : "false") << ", message=" << message_
           << "}";
    return stream.str();
}

/**
 * @brief 按工厂函数传入的字段创建错误对象
 * @param category 错误类别
 * @param module 发生错误的模块名称
 * @param code 错误码
 * @param message 可读错误信息
 * @param recoverable 该错误是否可恢复
 *
 * @details
 * 该构造函数只有在本类的工厂函数内部使用
 * 工厂函数负责选择正确的错误类别，避免调用方随意组合出不一致的字段
 */
MediaError::MediaError(MediaErrorCategory category, std::string module, int code, std::string message, bool recoverable)
    : category_(category),
      module_(std::move(module)),
      code_(code),
      message_(std::move(message)),
      recoverable_(recoverable)

{}

}  // namespace mediacore

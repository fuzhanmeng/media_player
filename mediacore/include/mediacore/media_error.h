/**
 * @file media_error.h
 * @brief 播放器内部使用的错误码及其对应错误信息
 * @details
 * MediaError 用来说明本次操作为什么失败，用于替代 bool，更清晰的知道错误类别，原始错误码，错误原因
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mediacore {

/**
 * @enum MediaErrorCategory
 * @brief 错误来源分类
 * @details
 * 分类的目的是让上层分清是哪个模块出的问题，FFmpeg 打不开文件和播放状态不允许 Seek 处理方式通常不一样
 */
enum class MediaErrorCategory : std::uint8_t {
    kNone,  ///< 没有错误
    kFfmpeg,  ///< FFmpeg API 返回的错误
    kSystem,  ///< 系统调用或 C/C++ 标准库错误
    kIo,  ///< 文件、URL、网络读写相关错误
    kState,  ///< 当前对象状态不允许执行该操作
    kInternal,  ///< 代码内部逻辑错误或不应发生的情况
};

/**
 * @brief 把错误类别转换为日志可读的字符串
 * @param category 错误类别
 * @return 静态字符串，不需要调用方释放
 */
const char* MediaErrorCategoryToString(MediaErrorCategory category);

/**
 * @class MediaError
 * @brief 保存一次媒体操作失败的原因
 * @details 默认构造的 MediaError 表示没有错误
 * 真正失败时，通过 FromFfmpeg / FromSystem / FromIo / FromState / FromInternal 创建
 *
 * 线程安全：
 * MediaError 创建完成后按值传递和只读访问是安全的；
 * 多个线程不要同时修改同一个 MediaError 对象
 */
class MediaError {
public:
    /**
     * @brief 构造一个 "没有错误" 对象
     */
    MediaError() = default;

    /**
     * @brief 根据 FFmpeg 错误码创建错误对象
     * @param module 发生错误的模块名
     * @param code FFmpeg API 返回的错误码
     * @param message 调用方补充的错误信息
     * @param recoverable true 表示上层可以尝试恢复错误，false 表示此错误无法恢复
     * @return FFmpeg 错误文本的 MediaError 对象
     */
    static MediaError FromFfmpeg(std::string module, int code, std::string_view message, bool recoverable = false);

    /**
     * @brief 根据系统错误码创建错误对象
     * @param module 发生错误的模块名
     * @param code errno 或类似系统错误码
     * @param message 可读错误说明
     * @param recoverable true 表示上层可以尝试恢复错误，false 表示此错误无法恢复
     * @return 系统错误MediaError 对象
     */
    static MediaError FromSystem(std::string module, int code, std::string_view message, bool recoverable = false);

    /**
     * @brief 创建文件、URL 或网络读写错误
     * @param module 发生错误的模块名
     * @param code 原始错误码；没有错误码时可以传 0
     * @param message 可读错误说明
     * @param recoverable true 表示上层可以尝试恢复错误，false 表示此错误无法恢复
     * @return IO 错误文本的 MediaError 对象
     */
    static MediaError FromIo(std::string module, int code, std::string_view message, bool recoverable = false);

    /**
     * @brief 创建状态错误
     * @param module 发生错误的模块名
     * @param message 调用方补充的错误信息
     * @return 状态错误的 MediaError 对象
     */
    static MediaError FromState(std::string module, std::string message);

    /**
     * @brief 创建内部错误
     * @param module 发生错误的模块名
     * @param message 可读错误说明
     * @return MediaError
     */
    static MediaError FromInternal(std::string module, std::string message);

    /**
     * @brief 判断一个对象是否保存了一个真实错误
     * @return true 表示错误，false 表示没有错误
     */
    bool HasError() const { return category_ != MediaErrorCategory::kNone; }

    /**
     * @brief 获取错误类别
     * @return MediaErrorCategory
     */
    MediaErrorCategory GetCategory() const { return category_; }

    /**
     * @brief 获取原始错误码
     * @details
     * FFmpeg 错误：avformat_open_input 返回的负数错误码
     * 系统错误：errno，例如 ENOENT
     * 状态错误：没有底层错误码，所以用 0
     * 内部错误：通常也没有底层错误码，所以用 0
     * @return FFmpeg 错误码 / errno， 或者 0
     */
    int Code() const { return code_; }

    /**
     * @brief 获取发生错误的模块名
     * @return 模块名字符串
     */
    const std::string& Module() const { return module_; }

    /**
     * @brief 获取可读错误说明
     * @return 错误说明字符串引用
     */
    const std::string& Message() const { return message_; }

    /**
     * @brief 判断这个错误是否可恢复
     * @return true 这个错误可以恢复，false 这个错误无法恢复
     */
    bool IsRecoverable() const { return recoverable_; }

    /**
     * @brief 拼接适合日志输出的完整错误文本
     * @return 包含类别、模块、错误码和说明的字符串
     */
    std::string ToString() const;

private:
    /**
     * @brief 工厂函数调用的私有构造函数
     */
    MediaError(MediaErrorCategory category, std::string module, int code, std::string message, bool recoverable);

    MediaErrorCategory category_{MediaErrorCategory::kNone};
    std::string module_{};
    int code_{};
    std::string message_{};
    bool recoverable_{};
};
}  // namespace mediacore

/**
 * @file media_error.h
 * @brief 播放器核心流水线统一结构化错误类型定义
 * @details
 * MediaError 用于准确描述一次音视频媒体操作失败的完整上下文：
 * 1. 结构化分类：涵盖错误类别（Category）、发生模块（Module）、底层原始错误码（Raw Code）、
 *    人类可读的综合描述（Message）以及可恢复性标志（Recoverable）；
 * 2. 跨模块通用契约：供 Demuxer、Decoder、VideoSink、AudioSink 以及 Pipeline 控制层统一传递；
 * 3. 依赖解耦：本头文件纯基于标准库构建，严禁引入 FFmpeg 等具体依赖库的头文件。
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mediacore {

/**
 * @enum MediaErrorCategory
 * @brief 错误来源大类分类
 */
enum class MediaErrorCategory : std::uint8_t {
    kNone,  ///< 正常状态，无错误
    kFfmpeg,  ///< FFmpeg 底层 API 返回的错误（如解封装、编解码、swscale 失败）
    kSystem,  ///< 操作系统调用或 C/C++ 标准库引发的底层错误（基于 errno）
    kIo,  ///< 本地文件读写、URL 解析或网络套接字 I/O 错误
    kState,  ///< 状态机冲突：当前对象或管线运行状态不允许执行请求的操作
    kInternal,  ///< 框架内部逻辑缺陷、防御性断言失败或不应出现的分支
};

/**
 * @brief 将错误分类枚举转换为字符串常量
 * @param category 错误类别枚举
 * @return 静态字符串常量指针（无需释放）
 */
const char* MediaErrorCategoryToString(MediaErrorCategory category) noexcept;

/**
 * @class MediaError
 * @brief 封装媒体操作失败原因的领域错误值对象
 * @details
 * 1. 默认构造表示无错误（kNone）；
 * 2. 发生真实错误时，统一通过静态命名工厂方法（FromFfmpeg / FromSystem / FromIo / FromState / FromInternal）创建。
 */
class MediaError final {
public:
    /**
     * @brief 默认构造函数：构造一个表示“无错误”的默认对象（HasError() == false）
     */
    MediaError() = default;

    /**
     * @brief 工厂方法：基于 FFmpeg 错误码构造 MediaError
     * @param module 发生错误的子系统或组件模块名（如 "FfmpegDemuxer"、"FfmpegVideoDecoder"）
     * @param code FFmpeg API 返回的原生负数错误码
     * @param message 业务层补充上下文说明
     * @param recoverable true 表示上层可尝试容错降级或重试，false 表示致命不可逆错误
     * @return 携带格式化错误描述的 MediaError 实例
     */
    static MediaError FromFfmpeg(std::string module, int code, std::string_view message, bool recoverable = false);

    /**
     * @brief 工厂方法：基于操作系统 errno 构造 MediaError
     * @param module 发生错误的子系统或组件模块名
     * @param code errno 或系统原生错误码
     * @param message 可读业务错误说明
     * @param recoverable 错误是否可恢复
     * @return 携带系统错误文本的 MediaError 实例
     */
    static MediaError FromSystem(std::string module, int code, std::string_view message, bool recoverable = false);

    /**
     * @brief 工厂方法：构造文件、URL 或网络传输相关的 I/O 错误
     * @param module 发生错误的子系统或组件模块名
     * @param code 原始底层错误码（无底层错误码时传 0）
     * @param message 错误上下文描述
     * @param recoverable 错误是否可通过重试恢复
     * @return 携带 I/O 错误文本的 MediaError 实例
     */
    static MediaError FromIo(std::string module, int code, std::string_view message, bool recoverable = false);

    /**
     * @brief 工厂方法：构造管线或节点状态机流转冲突错误（默认不可恢复）
     * @param module 发生错误的子系统或组件模块名
     * @param message 状态冲突详细说明
     * @return 状态错误的 MediaError 实例
     */
    static MediaError FromState(std::string module, std::string message);

    /**
     * @brief 工厂方法：构造框架内部逻辑缺陷或断言失败错误（不可恢复）
     * @param module 发生错误的子系统或组件模块名
     * @param message 错误原因描述
     * @return 内部错误的 MediaError 实例
     */
    static MediaError FromInternal(std::string module, std::string message);

    /**
     * @brief 查询当前对象是否包含真实错误
     * @return true 表示存在有效错误；false 表示无错误（kNone）
     */
    bool HasError() const noexcept { return category_ != MediaErrorCategory::kNone; }

    /**
     * @brief 获取错误的大类分类
     */
    MediaErrorCategory GetCategory() const noexcept { return category_; }

    /**
     * @brief 获取底层原生错误码
     * @details
     * - FFmpeg 错误：返回 avformat/avcodec 返回的负数错误码；
     * - 系统/IO 错误：返回标准 errno（例如 ENOENT、ECONNREFUSED）；
     * - 状态/内部错误：无底层码，返回 0。
     * @return 原生错误数值
     */
    int Code() const noexcept { return code_; }

    /**
     * @brief 获取发生报错的组件模块名
     */
    const std::string& Module() const noexcept { return module_; }

    /**
     * @brief 获取详细错误描述文本
     */
    const std::string& Message() const noexcept { return message_; }

    /**
     * @brief 查询该错误是否被标注为可恢复
     */
    bool IsRecoverable() const noexcept { return recoverable_; }

    /**
     * @brief 将错误信息序列化为便于日志追踪和诊断的结构化字符串
     * @return 格式化后的错误全景文本
     */
    std::string ToString() const;

private:
    /**
     * @brief 私有核心构造函数，供工厂方法统一组装
     */
    MediaError(MediaErrorCategory category, std::string module, int code, std::string message, bool recoverable);

    MediaErrorCategory category_{MediaErrorCategory::kNone};  ///< 错误分类
    std::string module_{};  ///< 触发错误的模块标识
    int code_{0};  ///< 原始数字错误码
    std::string message_{};  ///< 格式化后的综合错误描述
    bool recoverable_{false};  ///< 是否属于可恢复类型错误
};

}  // namespace mediacore
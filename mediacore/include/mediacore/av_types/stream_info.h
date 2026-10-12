/**
 * @file stream_info.h
 * @brief 媒体容器与音视频流元数据自包含值类型定义
 * @details
 * 本文件定义打开媒体资源后探测到的只读描述元数据结构：
 *
 * 数据链路来源：
 * 本地文件 / 网络流 URI
 *    ↓
 * avformat_open_input()
 *    ↓
 * avformat_find_stream_info()
 *    ↓
 * Demuxer 提取 AVStream / AVCodecParameters
 *    ↓
 * 深拷贝字段填充至 MediaInfo 结构体
 *    ↓
 * 启动正式数据流驱动（av_read_frame）
 *
 * 核心架构原则：
 * 1. 强依赖隔离：本头文件纯基于 C++ 标准库构建，严禁引入 FFmpeg 头文件，亦不持有底层 AVFormatContext / AVStream 等 C
 * 指针；
 * 2. 独立生命周期：底层 FFmpeg 句柄关闭或发生错误析构后，本文件的只读值对象仍可被 Pipeline、UI 与业务层安全持有。
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mediacore {

/**
 * @struct Rational
 * @brief 表示时间基（TimeBase）或显示帧率（FrameRate）的有理数分数结构体
 * @details 替代并隔离 FFmpeg 的 AVRational，避免直接对外暴露第三方底层类型。
 */
struct Rational final {
    std::int64_t numerator_{0};  ///< 分子（Numerator）
    std::int64_t denominator_{0};  ///< 分母（Denominator，不可为 0）

    /**
     * @brief 校验分子与分母是否构成有效的正有理数
     * @return 分子与分母均大于 0 时返回 true，否则返回 false
     */
    bool IsValid() const noexcept { return numerator_ > 0 && denominator_ > 0; }

    /**
     * @brief 将有理数计算转换为双精度浮点数
     * @return 分母非 0 时返回浮点除法结果；分母为 0 时安全兜底返回 0.0
     * @note 仅用于 UI 帧率展示与日志打印，严禁用于严苛的 PTS 时间戳基准换算（防止浮点精度丢失）。
     */
    double ToDouble() const noexcept {
        if (denominator_ == 0) {
            return 0.0;
        }
        return static_cast<double>(numerator_) / static_cast<double>(denominator_);
    }
};

/**
 * @enum VideoPixelFormat
 * @brief 视频流原生像素格式枚举
 * @details 自定义项目独立枚举，解耦 FFmpeg 的 AVPixelFormat。
 */
enum class VideoPixelFormat : std::uint8_t {
    kUnknown,  ///< 尚未识别或当前项目管线暂不支持的像素格式
    kYuv420p,  ///< 平面型 YUV420P（Y/U/V 三个独立内存平面）
    kNv12,  ///< 半平面型 NV12（连续 Y 平面 + UV 交叉打包平面）
    kYuv422p,  ///< 平面型 YUV422P
    kYuv444p,  ///< 平面型 YUV444P
    kRgba,  ///< 打包型 RGBA（连续 32-bit packed 像素）
};

/**
 * @brief 将视频像素格式枚举转换为日志可读字符串
 * @param format 待转换的像素格式
 * @return 对应的常量字符串指针
 */
inline const char* VideoPixelFormatToString(VideoPixelFormat format) noexcept {
    switch (format) {
        case VideoPixelFormat::kUnknown:
            return "Unknown";
        case VideoPixelFormat::kYuv420p:
            return "YUV420P";
        case VideoPixelFormat::kNv12:
            return "NV12";
        case VideoPixelFormat::kYuv422p:
            return "YUV422P";
        case VideoPixelFormat::kYuv444p:
            return "YUV444P";
        case VideoPixelFormat::kRgba:
            return "RGBA";
    }
    return "Unknown";
}

/**
 * @enum AudioSampleFormat
 * @brief 音频流原生采样精度与排布格式枚举
 * @details
 * 自定义项目独立枚举，解耦 FFmpeg 的 AVSampleFormat：
 * - Planar：各声道音频数据独立在不同内存块中排布；
 * - Packed：各声道音频采样点按时序紧凑交错排布。
 */
enum class AudioSampleFormat : std::uint8_t {
    kUnknown,  ///< 尚未识别或暂不支持的采样格式
    kU8,  ///< Packed 无符号 8-bit 整数
    kS16,  ///< Packed 有符号 16-bit 整数
    kS32,  ///< Packed 有符号 32-bit 整数
    kS64,  ///< Packed 有符号 64-bit 整数
    kFloat,  ///< Packed 32-bit 单精度浮点数
    kDouble,  ///< Packed 64-bit 双精度浮点数
    kU8Planar,  ///< Planar 无符号 8-bit 整数
    kS16Planar,  ///< Planar 有符号 16-bit 整数
    kS32Planar,  ///< Planar 有符号 32-bit 整数
    kS64Planar,  ///< Planar 有符号 64-bit 整数
    kFloatPlanar,  ///< Planar 32-bit 单精度浮点数
    kDoublePlanar,  ///< Planar 64-bit 双精度浮点数
};

/**
 * @brief 将音频采样格式枚举转换为日志可读字符串
 * @param format 待转换的音频采样格式
 * @return 对应的常量字符串指针
 */
inline const char* AudioSampleFormatToString(AudioSampleFormat format) noexcept {
    switch (format) {
        case AudioSampleFormat::kUnknown:
            return "Unknown";
        case AudioSampleFormat::kU8:
            return "U8";
        case AudioSampleFormat::kS16:
            return "S16";
        case AudioSampleFormat::kS32:
            return "S32";
        case AudioSampleFormat::kS64:
            return "S64";
        case AudioSampleFormat::kFloat:
            return "Float";
        case AudioSampleFormat::kDouble:
            return "Double";
        case AudioSampleFormat::kU8Planar:
            return "U8Planar";
        case AudioSampleFormat::kS16Planar:
            return "S16Planar";
        case AudioSampleFormat::kS32Planar:
            return "S32Planar";
        case AudioSampleFormat::kS64Planar:
            return "S64Planar";
        case AudioSampleFormat::kFloatPlanar:
            return "FloatPlanar";
        case AudioSampleFormat::kDoublePlanar:
            return "DoublePlanar";
    }
    return "Unknown";
}

/**
 * @struct VideoStreamInfo
 * @brief 单条视频流的核心元数据描述结构体
 */
struct VideoStreamInfo {
    int stream_index_{-1};  ///< 在多路复用容器中的流索引序号
    int codec_id_{0};  ///< 编解码器代号（映射自 AVCodecID）
    int width_{0};  ///< 图像画面物理宽度（像素）
    int height_{0};  ///< 图像画面物理高度（像素）

    VideoPixelFormat pixel_format_{VideoPixelFormat::kUnknown};  ///< 视频像素格式
    Rational frame_rate_{};  ///< 标称平均显示帧率（FPS）
    Rational time_base_{};  ///< 容器层时间基准（用于微秒 PTS 转换）
    std::optional<std::int64_t> duration_us_{};  ///< 视频流总时长（微秒）；未知或无界流时为空

    std::vector<std::uint8_t> extradata_{};  ///< 解码器头部私有数据（如 H.264 SPS/PPS、HEVC VPS）

    /**
     * @brief 校验视频流的基础关键元数据是否完备有效
     * @return 关键参数满足起播条件时返回 true，否则返回 false
     */
    bool IsValid() const noexcept {
        if (stream_index_ < 0 || codec_id_ <= 0) {
            return false;
        }
        if (width_ <= 0 || height_ <= 0) {
            return false;
        }
        if (!time_base_.IsValid()) {
            return false;
        }
        if (duration_us_.has_value() && duration_us_.value() < 0) {
            return false;
        }
        return true;
    }
};

/**
 * @struct AudioStreamInfo
 * @brief 单条音频流的核心元数据描述结构体
 */
struct AudioStreamInfo {
    int stream_index_{-1};  ///< 在多路复用容器中的流索引序号
    int codec_id_{0};  ///< 编解码器代号（映射自 AVCodecID）
    int sample_rate_hz_{0};  ///< 采样频率（Hz，例如 44100、48000）
    int channel_count_{0};  ///< 物理声道总数（如 1、2、6）

    std::string channel_layout_{};  ///< 声道布局描述（如 "mono"、"stereo"、"5.1"）
    AudioSampleFormat sample_format_{AudioSampleFormat::kUnknown};  ///< 原始音频采样量化格式
    Rational time_base_{};  ///< 容器层时间基准
    std::optional<std::int64_t> duration_us_{};  ///< 音频流总时长（微秒）；未知或直播流时为空

    std::vector<std::uint8_t> extradata_{};  ///< 解码器头部私有初始化数据（如 AAC AudioSpecificConfig）

    /**
     * @brief 校验音频流的基础关键元数据是否完备有效
     * @return 关键参数满足播放配置要求时返回 true，否则返回 false
     */
    bool IsValid() const noexcept {
        if (stream_index_ < 0 || codec_id_ <= 0) {
            return false;
        }
        if (sample_rate_hz_ <= 0 || channel_count_ <= 0) {
            return false;
        }
        if (!time_base_.IsValid()) {
            return false;
        }
        if (duration_us_.has_value() && duration_us_.value() < 0) {
            return false;
        }
        return true;
    }
};

/**
 * @struct MediaInfo
 * @brief 解封装探测得到的完整媒体容器与轨道元数据聚合结构体
 * @details
 * 使用 std::optional 表达音视频轨道的物理存在性：
 * - 纯视频文件：video_stream_ 有值，audio_stream_ 为 std::nullopt；
 * - 纯音频文件：audio_stream_ 有值，video_stream_ 为 std::nullopt；
 * - 标准音视频流：两字段均有值。
 */
struct MediaInfo {
    std::string container_name_{};  ///< 封装容器格式标识（如 "mov,mp4,m4a,3gp"、"matroska,webm"）
    std::optional<std::int64_t> duration_us_{};  ///< 全局总呈现时长（微秒）；无界直播流或未知时为空
    bool can_seek_{false};  ///< 媒体源是否声明支持 Seek 定位
    std::optional<VideoStreamInfo> video_stream_{};  ///< 选定激活的视频流；不存在视频轨时为空
    std::optional<AudioStreamInfo> audio_stream_{};  ///< 选定激活的音频流；不存在音频轨时为空

    /**
     * @brief 校验媒体信息是否至少包含一条合法的可播放轨道
     * @return 至少存在一条有效视频轨或有效音频轨且时长非负时返回 true
     */
    bool IsValid() const noexcept {
        if (duration_us_.has_value() && duration_us_.value() < 0) {
            return false;
        }

        const bool has_valid_video = video_stream_.has_value() && video_stream_->IsValid();
        const bool has_valid_audio = audio_stream_.has_value() && audio_stream_->IsValid();

        return has_valid_audio || has_valid_video;
    }
};

}  // namespace mediacore
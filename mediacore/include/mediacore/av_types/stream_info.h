/**
 * @file stream_info.h
 * @brief 保存媒体容器和音视频流基本信息的纯 C++ 数据结构
 * @details 这里的数据结构保存的是探测阶段从媒体文件或网络流中读取出来的结果
 *
 * 数据来源：
 * 文件 / URL
 *    ↓
 * avformat_open_input()
 *    ↓
 * avformat_find_stream_info()
 *    ↓
 * Demuxer 读取 AVStream / AVCodecParameters
 *    ↓
 * 复制字段到 MediaInfo
 *    ↓
 * 开始正式 av_read_frame()
 *
 * 本文件不包含 FFmpeg 头文件，也不保存 AVFormatContext / AVStream 或 AVCodecParameters 指针
 * FFmpeg 对象关闭以后，这些结构仍然可以继续使用
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace mediacore {

/**
 * @struct Rational
 * @brief 分数（有理数）数据结构，分子和分母表示的一个比例值
 * @details
 * 时间基和帧率都不是普通的整数
 * 1 / 90000 常见于视频时间基
 * 30000 / 1001 约等于 29.97 帧/秒
 *
 * 这里使用自己的结构，不直接使用 FFmpeg 的 AVRational
 */
struct Rational {
    std::uint64_t numerator_{0};  ///< 分子
    std::uint64_t denominator_{0};  ///< 分母，不能为 0

    /**
     * @brief 判断 Rational 是否合法
     * @return 分子和分母都大于 0 返回 true，否则返回 false
     */
    bool IsValid() const { return numerator_ > 0 && denominator_ > 0; }

    /**
     * @brief 把比例值转换为 double
     * @return 分母不为 0 时返回计算结果，否则返回 0.0
     * @details
     * 这个函数主要用于帧率显示和测试，时间戳换算不可用
     */
    double ToDouble() const {
        if (denominator_ == 0) {
            return 0.0;
        }

        return static_cast<double>(numerator_) / static_cast<double>(denominator_);
    }
};

/**
 * @enum VideoPixelFormat
 * @brief 视频流的像素格式
 * @details
 * 使用项目自己的枚举，不直接暴露 FFmpeg 的 AVPixelFormat
 */
enum class VideoPixelFormat : std::uint8_t {
    kUnknown,  ///< 尚未识别或当前项目不支持的像素格式
    kYuv420p,  ///< YUV420P，三个平面
    kNv12,  ///< NV12，Y 平面加交错 UV 平面
    kYuv422p,  ///< YUV422P，三个平面
    kYuv444p,  ///< YUV444P，三个平面
    kRgba,  ///< RGBA packed 像素
};

/**
 * @enum AudioSampleFormat
 * @brief 音频流的样本格式
 * @details
 * 使用项目自己的枚举，不直接暴露 FFmpeg 的 AVSampleFormat
 * planar 表示左右声道分开存储，packed 表示左右声道交错存储
 */
enum class AudioSampleFormat {
    kUnknown,  ///< 尚未识别或当前项目不支持的格式
    kU8,  ///< packed unsigned 8-bit
    kS16,  ///< packed signed 16-bit
    kS32,  ///< packed signed 32-bit
    kS64,  ///< packed signed 64-bit
    kFloat,  ///< packed 32-bit floating point
    kDouble,  ///< packed 64-bit floating point
    kU8Planar,  ///< planar unsigned 8-bit
    kS16Planar,  ///< planar signed 16-bit
    kS32Planar,  ///< planar signed 32-bit
    kS64Planar,  ///< planar signed 64-bit
    kFloatPlanar,  ///< planar 32-bit floating point
    kDoublePlanar,  ///< planar 64-bit floating point
};

/**
 * @struct VideoStreamInfo
 * @brief 一条视频流的基本信息
 * @details
 * 这些字段用于保存探测阶段探测到的视频流信息，不保存解码对象
 * Decoder 后续可以直接使用这些值创建 AVCodecContext，但 AVCodecContext 本身不属于此结构体
 */
struct VideoStreamInfo {
    int stream_index_{-1};  ///< 在容器中的视频流索引编号，不能用负数表示有效流
    int codec_id_{0};  ///< 编码器编号，由 FFmpeg 适配层转换后写入
    int width_{0};  ///< 视频流的宽度，单位像素
    int height_{0};  ///< 视频流的高度，单位像素

    VideoPixelFormat pixel_format_{VideoPixelFormat::kUnknown};  ///< 视频流的像素格式
    Rational frame_rate_{};  ///< 此视频流的帧率
    Rational time_base_{};  ///< 此视频流的时间基
    std::optional<std::int64_t> duration_us_{};  ///< 此视频流的时长，单位微秒，未知时没有值

    /**
     * @brief 判断此视频流的基本字段是否可用
     * @return stream index / width / height 和 time_base 有效时都返回 true
     * @details
     * 帧率和像素格式有时候可能探测不到，所以它们不是当前判断的核心
     * codec_id_ 为 0 时表示还没有写入真实编码格式，不认为是有效流
     */
    bool IsValid() const {
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
 * @brief 一条音频流的基本信息
 * @details
 * 这里描述的是输入媒体中的音频流信息，通常是 AAC / MP3 等压缩格式
 * 它和 AudioFrame 不同，AudioFrame 描述的是解码后的音频帧信息，这里保存的是探测阶段拿到的流信息
 */
struct AudioStreamInfo {
    int stream_index_{-1};  ///< 在容器中的音流编号，不能用负数表示有效流
    int codec_id_{0};  ///< 编码器编号，由 FFmpeg 适配层转换后写入
    int sample_rate_hz_{0};  ///< 采样率，单位 Hz
    int channel_count_{0};  ///< 声道数量

    std::string channel_layout_{};  ///< 声道布局方式，如mono / stereo 5.1
    AudioSampleFormat sample_format_{AudioSampleFormat::kUnknown};  ///< 输入音频的格式
    Rational time_base_{};  ///< 音频流的时间基
    std::optional<std::int64_t> duration_us_{};  ///< 音频流的时长，单位微秒，直播流等无时长显示

    /**
     * @brief 判断音频流的基本字段是否可用
     * @return stream_index / codec_id / sample_rate_hz / channel_count / time_base 有效时返回 true
     * 声道布局名称和样本格式在某些输入中可能缺失，因此当前不把它们作为必填项
     * AudioDecoder 如果需要重采样，应在创建 SwrContext 前再次检查完整格式
     */
    bool IsValid() const {
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
 * @brief 保存整个媒体输入的基本信息
 * @details
 * MediaInfo 不是某一帧的信息，是一次 Open / 探测到的媒体描述
 * optional 用来表示输入媒体中是存在音频流 / 视频流
 * - 没有视频轨时 video_stream_ 没有值；
 * - 没有音频轨时 audio_stream_ 没有值；
 * - 音视频都有时两个字段都有值
 */
struct MediaInfo {
    std::string container_name_{};
    std::optional<std::int64_t> duration_us_{};
    bool can_seek_{false};
    std::optional<VideoStreamInfo> video_stream_{};
    std::optional<AudioStreamInfo> audio_stream_{};

    /**
     * @brief 判断媒体信息是否至少包含有一条可用的音视频流
     * @return 至少有一条有效的音频流或者视频流时，返回 true
     */
    bool IsValid() const {
        if (duration_us_.has_value() && duration_us_.value() < 0) {
            return false;
        }

        const bool has_valid_video = video_stream_.has_value() && video_stream_->IsValid();
        const bool has_valid_audio = audio_stream_.has_value() && audio_stream_->IsValid();

        return has_valid_audio || has_valid_video;
    }
};
}  // namespace mediacore

/**
 * @file stream_info.h
 * @brief 探测阶段读取到的媒体容器和音视频流基本信息
 * @details
 * 数据来自 avformat_open_input() / avformat_find_stream_info() 读取到的 AVStream
 * AVCodecParameters，可以实现与 FFmpeg 的解耦
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
#include <vector>

namespace mediacore {

/**
 * @struct Rational
 * @brief 分数（有理数），用于表示时间基和帧率等非整数比例值
 * @details
 * 不直接使用 FFmpeg 的 AVRational，避免对外暴露 FFmpeg 类型
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
 * @details 使用项目自己的枚举，不直接暴露 FFmpeg 的 AVPixelFormat
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
 * planar 表示各声道分开存储，packed 表示各声道交错存储
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
 * 保存探测阶段得到的视频流描述，不持有解码对象；Decoder 可用这些值创建 AVCodecContext
 * 但 AVCodecContext 本身不属于此结构体
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
     * @brief 解码器初始化所需的私有数据，例如 H.264/HEVC 的 SPS/PPS
     * @details
     * 数据来源于 AVCodecParameters::extradata
     * MediaInfo 对该数据进行值拷贝并自行管理生命周期，避免依赖 FFmpeg 对象
     * Decoder 创建 AVCodecContext 时根据需要将其复制到 codec context
     * 空 vector 表示当前媒体没有提供 extradata
     */
    std::vector<std::uint8_t> extradata_{};

    /**
     * @brief 判断此视频流的基本字段是否可用
     * @return stream index / width / height 和 time_base 有效时都返回 true
     * @details 帧率和像素格式有时探测不到，因此不作为判断的核心；
     * codec_id_ 为 0 表示尚未写入真实编码格式，不认为是有效流
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
 * @details 描述输入媒体中的压缩音频流（如 AAC / MP3），与解码后的 AudioFrame 是不同的概念
 */
struct AudioStreamInfo {
    int stream_index_{-1};  ///< 在容器中的音流编号，不能用负数表示有效流
    int codec_id_{0};  ///< 编码器编号，由 FFmpeg 适配层转换后写入
    int sample_rate_hz_{0};  ///< 采样率，单位 Hz
    int channel_count_{0};  ///< 声道数量

    std::string channel_layout_{};  ///< 声道布局方式，如 mono / stereo / 5.1
    AudioSampleFormat sample_format_{AudioSampleFormat::kUnknown};  ///< 输入音频的格式
    Rational time_base_{};  ///< 音频流的时间基
    std::optional<std::int64_t> duration_us_{};  ///< 音频流的时长，单位微秒，直播流等无时长显示

    /**
     * @brief 解码器初始化需要的私有数据
     * @details
     * 数据来源于 AVCodecParameters::extradata
     * MediaInfo 对该数据进行拷贝并管理其生命周期，避免对 FFmpeg 的依赖
     * Docoder 创建 AVCodecContext 时根据需要将其复制到 codec context，
     * 空 vector 表示当前音频流没有提供 extradata
     */
    std::vector<uint8_t*> extradata_{};

    /**
     * @brief 判断音频流的基本字段是否可用
     * @return stream_index / codec_id / sample_rate_hz / channel_count / time_base 有效时返回 true
     * @details 声道布局名称和样本格式在部分输入中可能缺失，因此不作为必填项；
     * AudioDecoder 如需重采样，应在创建 SwrContext 前再次检查完整格式
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
 * @brief 一次 Open / 探测得到的整个媒体输入的基本信息
 * @details
 * video_stream_ / audio_stream_ 用 optional 表示对应轨道是否存在：
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
     * @brief 判断媒体信息是否至少包含一条可用的音视频流
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

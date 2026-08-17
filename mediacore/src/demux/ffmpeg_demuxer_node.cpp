/**
 * @file ffmpeg_demuxer_node.cpp
 * @brief FFmpeg Decoder Node
 */

#include "demux/ffmpeg_demuxer_node.h"

#include <array>
#include <cstddef>  ///< 大小、内存、指针相关 size_t ptrdiff_t
#include <cstdint>  ///< 固定宽度整数
#include <optional>
#include <string>
#include <string_view>

#include "mediacore/logger.h"

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace mediacore {

/**
 * @namespace 匿名命名空间
 * @brief 此匿名命名空间内函数只用于 ffmpeg_decoder_node 内部，不对外暴露
 */
namespace {

constexpr const char* kModuleName = "FfmpegDemuxerNode";
constexpr std::size_t kChannelLayoutBufferSize = 256;

/**
 * @brief 将 AVRational 转换为 自己封装的 Rational
 * @param value FFmpeg 返回的 AVRational，例如 1/90000 或者 30000/1001
 * @return 分子和分母都大于 0 时返回对应的 Rational，否则返回 0 / 0
 * @details 时间基 和 帧率 都用分数表示
 */
Rational ConvertRational(AVRational value) {
    if (value.num <= 0 || value.den <= 0) {
        return {};
    }

    return {static_cast<std::uint64_t>(value.num), static_cast<std::uint64_t>(value.den)};
}

/**
 * @brief 从视频流中选择一个可用的帧率
 * @param stream ffmpeg video stream
 * @param codec_parameters 这条视频流的编码参数
 * @return 返回 视频流的帧率
 * @details FFmpeg 中可能有多个帧率，我们顺序是：先去找平均帧率 avg_frame_rate,再去找 FFmpeg 推测
 * 出来的r_frame_rate，最后找一编码参数中记录的帧率 framerate
 */
Rational ConvertVideoFrameRate(const AVStream& stream, const AVCodecParameters& codec_parameters) {
    Rational frame_rate;

    // 1. find avg_frame_rate
    frame_rate = ConvertRational(stream.avg_frame_rate);
    if (frame_rate.IsValid()) {
        return frame_rate;
    }

    // 2. find r_frame_rate
    frame_rate = ConvertRational(stream.r_frame_rate);
    if (frame_rate.IsValid()) {
        return frame_rate;
    }

    // 3. find framerate
    return ConvertRational(codec_parameters.framerate);
}

/**
 * @brief 把 FFmpeg 流时长换算为微秒
 * @param duration AVStream::duration，表示这条流占用了多少个时间刻度，不是真实的秒数
 * 例如 duration = 450000，time_base = 1 / 90000，那么真实时间是 450000 * 1 / 90000 = 5 秒
 * @param time_base AVStream::time_base，表示的是一个时间刻度等于多少秒
 * @return 返回时长（微秒），FFmpeg 无时长、duration 为负数，time_base不合法时返回空值
 */
std::optional<std::int64_t> ConvertDurationToMicroseconds(std::int64_t duration, AVRational time_base) {
    // 1. AV_NOPTS_VALUE 表示 FFmpeg 没有拿到这个时间戳或时长，直播流、损坏文件或部分封装格式没有时长
    if (duration == AV_NOPTS_VALUE || duration < 0) {
        return std::nullopt;
    }

    // 2. 确认 time_base 可用
    const Rational project_time_base = ConvertRational(time_base);
    if (!project_time_base.IsValid()) {
        return std::nullopt;
    }

    // 3. 将 duration 转化为 微秒后返回
    return av_rescale_q(duration, time_base, AV_TIME_BASE_Q);
}

/**
 * @brief 把 FFmpeg 视频像素格式映射到项目枚举
 * @param format AVCodecParameters::format，对视频流来讲，这个整数实际表示 AVPixelFormat
 * @return 项目支持的视频格式，位支持时返回 kUnknown。
 * @details
 * AV_PIX_FMT_YUV420P AV_PIX_FMT_NV12 是FFmpeg的枚举值，所以需要转化为自定义的VideoPixelFormat
 */
VideoPixelFormat ConvertVideoPixelFormat(int format) {
    switch (static_cast<AVPixelFormat>(format)) {
        case AV_PIX_FMT_YUV420P:
            return VideoPixelFormat::kYuv420p;
        case AV_PIX_FMT_NV12:
            return VideoPixelFormat::kNv12;
        case AV_PIX_FMT_YUV422P:
            return VideoPixelFormat::kYuv422p;
        case AV_PIX_FMT_YUV444P:
            return VideoPixelFormat::kYuv444p;
        case AV_PIX_FMT_RGBA:
            return VideoPixelFormat::kRgba;
        default:
            return VideoPixelFormat::kUnknown;
    }
}

/**
 * @brief 把 FFmpeg 音频样本格式映射到项目枚举
 * @param format AVCodecParameters::format，对音频流来说，这个整数实际上表示 AVSampleFormat
 * @return 项目支持的音频流样本格式，未支持的格式返 kUnknown
 * @details
 * packed 表示多个声道交错存储，如 LRLRLR
 * planar 表示多个声道分开存储，如 LLL...  RRR...
 */
AudioSampleFormat ConvertAudioSampleFormat(int format) {
    switch (static_cast<AVSampleFormat>(format)) {
        case AV_SAMPLE_FMT_U8:
            return AudioSampleFormat::kU8;
        case AV_SAMPLE_FMT_S16:
            return AudioSampleFormat::kS16;
        case AV_SAMPLE_FMT_S32:
            return AudioSampleFormat::kS32;
        case AV_SAMPLE_FMT_S64:
            return AudioSampleFormat::kS64;
        case AV_SAMPLE_FMT_FLT:
            return AudioSampleFormat::kFloat;
        case AV_SAMPLE_FMT_DBL:
            return AudioSampleFormat::kDouble;
        case AV_SAMPLE_FMT_U8P:
            return AudioSampleFormat::kU8Planar;
        case AV_SAMPLE_FMT_S16P:
            return AudioSampleFormat::kS16Planar;
        case AV_SAMPLE_FMT_S32P:
            return AudioSampleFormat::kS32Planar;
        case AV_SAMPLE_FMT_S64P:
            return AudioSampleFormat::kS64Planar;
        case AV_SAMPLE_FMT_FLTP:
            return AudioSampleFormat::kFloatPlanar;
        case AV_SAMPLE_FMT_DBLP:
            return AudioSampleFormat::kDoublePlanar;
        default:
            return AudioSampleFormat::kUnknown;
    }
}

/**
 * @brief 读取 FFmpeg 声道布局
 * @param layout AVCodecParameters::ch_layout，它描述这条音频流有几个声道，以及这些声道是什么位置
 * @return 能识别时返回布局名称，识别失败时返回空字符串
 * @details
 * mono 表示单声道
 * stereo 表示左声道+ 右声道
 * 5.1 表示 前左、前右、中置、低音、后左、后右声道
 * AVChannelLayout 是 FFmpeg 的结构体，不是字符串，av_channel_layout_describe() 会把结构体
 * 转换成 stereo 这类可读字符串
 */
std::string ConvertChannelLayout(const AVChannelLayout& layout) {
    // 用于存储布局名称
    std::array<char, kChannelLayoutBufferSize> buffer{};

    const int result = av_channel_layout_describe(&layout, buffer.data(), buffer.size());

    if (result < 0) {
        LOG_WARN("failed to describe audio channel layout, error code: {}", result);
        return {};
    }

    return buffer.data();
}

/**
 * @brief 创建 "流信息字段不对" 的 MediaError
 * @param message 写清楚是哪个字段不对，例如宽高为 0，采样率为 0，没有可用流
 * @return 一个已经填好的模块名、错误类型、FFmpeg 错误码 和错误说明的 MediaError
 * MediaError 是错误对象的类型；真正失败时仍然需要创建一个具体对象返回给调用方。
 * 这个小函数只是避免重复写 MediaError::FromFfmpeg(kModuleName, AVERROR_INVALIDDATA, message)
 */
MediaError InvalidMetadataError(std::string_view message) {
    return MediaError::FromFfmpeg(kModuleName, AVERROR_INVALIDDATA, message);
}
}  // namespace

/**
 * @brief 获取并转换视频流信息到 VideoStreamInfo
 * @param stream avformat_find_stream_info() 之后得到的 AVStream 视频流
 * @param output 成功后写入的流编号、编码格式、长度、宽度、像素格式、帧率、time_base和duration
 * @return 返回本次的转换结果 HasError 为 true 表示有错误，为 false 表示没有错误
 */
MediaError ConvertVideoStreamInfo(const AVStream* stream, VideoStreamInfo& output) {
    // 1. 先清空 output
    output = {};

    // 2. 判断 stream 合法性
    if (stream == nullptr) {
        // 内部错误
        return MediaError::FromInternal(kModuleName, "video stream pointer is null");
    }

    if (stream->codecpar == nullptr) {
        // 内部错误
        return MediaError::FromInternal(kModuleName, "video stream codec parameters are null");
    }

    const AVCodecParameters& codec_parameters = *stream->codecpar;
    if (codec_parameters.codec_type != AVMEDIA_TYPE_VIDEO) {
        return InvalidMetadataError("stream codec type is not video");
    }

    // 3. 开始复制
    VideoStreamInfo converted;
    converted.stream_index_ = stream->index;
    converted.codec_id_ = static_cast<int>(codec_parameters.codec_id);
    converted.width_ = codec_parameters.width;
    converted.height_ = codec_parameters.height;
    converted.pixel_format_ = ConvertVideoPixelFormat(codec_parameters.format);  // 像素格式
    converted.frame_rate_ = ConvertVideoFrameRate(*stream, codec_parameters);
    converted.time_base_ = ConvertRational(stream->time_base);
    converted.duration_us_ = ConvertDurationToMicroseconds(stream->duration, stream->time_base);

    // 4. 判断 converted 是否合法
    if (!converted.IsValid()) {
        return InvalidMetadataError("video stream metadata is incomplete");
    }

    output = converted;
    LOG_DEBUG("Converted video stream: index {}, codec {}, size {}x{}",
              output.stream_index_,
              output.codec_id_,
              output.width_,
              output.height_);

    return {};
}

/**
 * @brief 获取并转化视频流信息到 AudioStreamInfo
 * @param stream avformat_find_stream_info() 获取到的音频流 AVStream
 * @param output 成功后写入流编号、编码格式、采样率、声道数、声道布局、样本格式、时间基和时长
 * @return 返回本次的转换结果 HasError 为 true 表示有错误，为 false 表示没有错误
 */
MediaError ConvertAudioStreamInfo(const AVStream* stream, AudioStreamInfo& output) {
    // 1. 先清空 output
    output = {};

    // 2. 判断 stream 合法性
    if (stream == nullptr) {
        return MediaError::FromInternal(kModuleName, "audio stream pointer is null");
    }

    if (stream->codecpar == nullptr) {
        return MediaError::FromInternal(kModuleName, "audio stream codec parameters are null");
    }

    const AVCodecParameters& codec_parameters = *stream->codecpar;
    if (codec_parameters.codec_type != AVMEDIA_TYPE_AUDIO) {
        return InvalidMetadataError("stream codec type is not audio");
    }

    // 3. 开始填充/转换信息
    AudioStreamInfo converted;

    converted.stream_index_ = stream->index;
    converted.codec_id_ = static_cast<int>(codec_parameters.codec_id);
    converted.sample_rate_hz_ = codec_parameters.sample_rate;
    converted.channel_count_ = codec_parameters.ch_layout.nb_channels;
    converted.channel_layout_ = ConvertChannelLayout(codec_parameters.ch_layout);
    converted.sample_format_ = ConvertAudioSampleFormat(codec_parameters.format);
    converted.time_base_ = ConvertRational(stream->time_base);
    converted.duration_us_ = ConvertDurationToMicroseconds(stream->duration, stream->time_base);

    if (!converted.IsValid()) {
        return InvalidMetadataError("audio stream metadata is incomplete");
    }

    output = converted;

    LOG_DEBUG("Converted audio stream: index {}, codec {}, sample {}, channels {}",
              output.stream_index_,
              output.codec_id_,
              output.sample_rate_hz_,
              output.channel_count_);

    return {};
}

/**
 * @brief 从 AVFormatContext 里读取整个输入文件的基本信息
 * @param format_context avformat_open_input() 和 av_find_stream_info() 成功后的对象，
 * 它里面有容器格式、总时长、AVStream 数组等信息
 * @param output 成功后写入容器名、总时长、是否可 Seek，以及第一条可用的音频流和视频流
 * @return 返回本次的转换结果，HasError 为 true 表示转换失败，为 false 表示转换成功
 * @details
 * AVFormatContext
 *   ↓
 * 读取容器信息：格式、总时长、是否可 seek
 *   ↓
 * 遍历 AVStream 数组
 *   ↓
 * 遇到视频流：调用 ConvertVideoStreamInfo()
 *   ↓
 * 遇到音频流：调用 ConvertAudioStreamInfo()
 *   ↓
 * 填充 MediaInfo
 */
MediaError ConvertMediaInfo(const AVFormatContext* format_context, MediaInfo& output) {
    // 1. 清空 output
    output = {};

    // 2. 判断 AVFormatContext 合法性
    if (format_context == nullptr) {
        return MediaError::FromInternal(kModuleName, "format context pointer is null");
    }

    // nb_streams 这个 媒体文件中有几条流 / streams 保存流的数组不存在
    if (format_context->nb_streams == 0 || format_context->streams == nullptr) {
        return InvalidMetadataError("media contains no streams");
    }

    // 3. 开始填充并转化
    MediaInfo converted;

    // iformat 是 FFmpeg 识别出来的封装格式，如 mov / mp4 / m4a
    if (format_context->iformat != nullptr && format_context->iformat->name != nullptr) {
        converted.container_name_ = format_context->iformat->name;
    }

    // 保存媒体时长
    if (format_context->duration != AV_NOPTS_VALUE && format_context->duration >= 0) {
        converted.duration_us_ = format_context->duration;
    }

    // pb 是底层 IO 对象，seekable 带 AVIO_SEEKABLE_NORMAL 标志时，说明输入支持普通 seek
    // 本地媒体文件一般支持 seek，但是网络直播流不支持
    if (format_context->pb != nullptr) {
        // 检查 seekable 里面有没有 AVIO_SEEKABLE_NORMAL 这个标志
        converted.can_seek_ = (format_context->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    }

    // 保存第一条流转换失败的原因，如果一条有用的音视频流都没有找到，就把这个错误返回给调用方
    MediaError first_conversion_error;
    for (unsigned index = 0; index < format_context->nb_streams; ++index) {
        const AVStream* stream = format_context->streams[index];
        if (stream == nullptr || stream->codecpar == nullptr) {
            continue;
        }

        // codecpar 是这条流的编码参数，类型是 AVCodecParameters*
        // !converted.video_stream_.has_value() 说明现在还没有保存视频流
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !converted.video_stream_.has_value()) {
            VideoStreamInfo video_info;
            MediaError error = ConvertVideoStreamInfo(stream, video_info);

            // 如果视频流信息保存成功，error = false
            if (!error.HasError()) {
                // 如果没有错误，就把 video_info 保存到 converted.video_stream_ 里
                converted.video_stream_ = video_info;
            }
            // 如果 first_conversion_error 目前还没有保存过任何失败原因
            else if (!first_conversion_error.HasError()) {
                first_conversion_error = error;
            }
        }

        // 如果 stream 是音频流 但是还没有保存过音频流
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !converted.audio_stream_.has_value()) {
            AudioStreamInfo audio_info;
            MediaError error = ConvertAudioStreamInfo(stream, audio_info);

            // 如果音频流保存成功
            if (!error.HasError()) {
                // 那么就把 audio_info 保存到 convert.audio_stream
                converted.audio_stream_ = audio_info;
            }

            // 如果保存失败，那么就判断 是否保存过失败原因
            else if (!first_conversion_error.HasError()) {
                first_conversion_error = error;
            }
        }
    }

    // 播放器要求至少要有一条有效的视频流或者音频流
    if (!converted.IsValid()) {
        // 如果之前保存过错误，就返回
        if (first_conversion_error.HasError()) {
            return first_conversion_error;
        }

        // 否则返回自定义的错误原因
        return MediaError::FromFfmpeg(kModuleName, AVERROR_STREAM_NOT_FOUND, "no valid audio or video stream found");
    }

    output = converted;
    LOG_INFO("Media info converted: Container is {}, video {}, audio {}",
             output.container_name_,
             output.video_stream_.has_value(),
             output.audio_stream_.has_value());
    return {};
}
}  // namespace mediacore
/**
 * @file ffmpeg_demuxer.cpp
 * @brief 基于 FFmpeg 实现的媒体解复用核心逻辑
 * @details
 * 负责媒体输入源的打开、流信息探测、音视频流筛选，以及压缩音视频数据包的读取和时间戳处理
 *
 * 主要机制：
 * 1. 可中断 I/O：通过 AVIOInterruptCB 响应外部 Stop 和 Seek 请求，
 * 在 FFmpeg 执行阻塞 I/O 时检查是否需要中断
 * 2. 时间轴归一化：根据媒体源的共同起始时间调整数据包时间戳，
 *    使输出时间戳统一到播放器使用的零起点时间轴
 * 3. 资源安全管理：使用 RAII 管理 AVFormatContext，
 *    确保打开、探测失败或提前退出时能够正确释放资源
 */

#include "ffmpeg_demuxer.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "mediacore/logger.h"
#include "mediacore/media_error.h"
#include "utils/ffmpeg_error.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

namespace mediacore {

/**
 * @struct DemuxerInterruptHandle::State
 * @brief 共享中断状态机的底层数据结构（跨控制线程与解封装工作线程）
 * @details
 * 采用 C++11 无锁原子变量实现。外部控制线程写入意图，FFmpeg 内部 C 回调读取判定。
 */
struct DemuxerInterruptHandle::State final {
    /// 永久停止标志；置为 true 后不再恢复，用于要求正在执行或即将执行的可中断 I/O 退出
    std::atomic<bool> stop_requested_{false};

    /// 全局Seek 请求代数；每次新的 Seek 请求都会递增，用于区分新旧 I/O 操作
    std::atomic<std::uint64_t> seek_cancel_generation_{0};

    /// 当前 I/O 操作开始前记录的 Seek 请求代数；与请求代数不一致时表示有新的 Seek 请求
    std::atomic<std::uint64_t> armed_generation_{0};
};

namespace {

// 常量标识定义：用于日志与错误码跟踪上报
constexpr const char* kOpenInputOperation = "avformat_open_input";
constexpr const char* kFindStreamInfoOperation = "avformat_find_stream_info";
constexpr const char* kFindStreamsOperation = "av_find_best_stream(audio/video)";
constexpr const char* kReadFrameOperation = "av_read_frame";
constexpr const char* kSeekFrameOperation = "av_seek_frame";
constexpr const char* kOpenCancelledOperation = "FfmpegDemuxer::Open cancelled";
constexpr const char* kModuleName = "FfmpegDemuxer";
constexpr const char* kTimelineOriginOperation = "determine playback timeline origin";
constexpr const char* kNormalizeTimestampOperation = "normalize packet timestamp";
constexpr const char* kBuildMediaInfoOperation = "build audio/video media information";

// FFmpeg 的 AVIOInterruptCB 回调约定：返回 0 继续 I/O，返回 1 请求中断当前 I/O
constexpr int kContinueFfmpegIo = 0;
constexpr int kAbortFfmpegIo = 1;

/**
 * @struct FormatContextCloser
 * @brief unique_ptr 的自定义删除器，负责关闭并释放 AVFormatContext
 * @note 当持有它的 RAII 对象析构时，调用 avformat_close_input 释放输入上下文
 */
struct FormatContextCloser final {
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

/// @brief AVFormatContext 临时独占持有容器别名
using FormatContextOwner = std::unique_ptr<AVFormatContext, FormatContextCloser>;

/**
 * @brief 将 FFmpeg 底层错误码转换为项目标准的 MediaError
 * @param operation 触发错误的 FFmpeg 函数或操作名
 * @param code FFmpeg 返回的负数错误码（如 AVERROR(EINVAL) / AVERROR_EOF 等）
 * @return 构造完成的标准 MediaError
 */
MediaError MakeFfmpegError(const char* operation, int code) {
    return ffmpeg_detail::MakeFfmpegMediaError(kModuleName, code, operation);
}

/**
 * @brief 校验 FFmpeg 时间基（AVRational）有效性
 * @param time_base 待校验的时间基结构体
 * @return 若分子分母均为正数返回 true，否则返回 false
 */
bool IsValidTimeBase(AVRational time_base) noexcept {
    return time_base.den > 0 && time_base.num > 0;
}

/**
 * @brief 传递本次打开操作选中的候选音视频流。
 * @details
 * 视频和音频在计算共同时间原点时只是一个候选集合；将它们封装在一个结构体中，明确函数处理的是整个集合
 * 计算结果不依赖两个轨道的顺序
 */
struct SelectedStreams final {
    const AVStream* video_stream_{nullptr};
    const AVStream* audio_stream_{nullptr};
};

/**
 * @brief 将基于流时间基的时间戳换算为微秒（us）
 * @param value 原始时间戳刻度（基于 time_base）
 * @param time_base 该时间戳所依据的流时间基
 * @return 转换后的微秒值；若时间戳为 AV_NOPTS_VALUE 或时间基无效则返回 std::nullopt
 * @details 内部调用 av_rescale_q 采用 64 位整数运算，内置了防溢出阶乘优化。
 */
std::optional<std::int64_t> TimestampToMicroseconds(std::int64_t value, AVRational time_base) {
    if (value == AV_NOPTS_VALUE || time_base.num <= 0 || time_base.den <= 0) {
        return std::nullopt;
    }

    const std::int64_t converted = av_rescale_q(value, time_base, AV_TIME_BASE_Q);
    if (converted == AV_NOPTS_VALUE) {
        return std::nullopt;
    }

    return converted;
}

/**
 * @brief 计算所选音视频流共同使用的播放时间原点，单位为微秒
 * @param av_format_ctx 解封装全局上下文
 * @param video_stream 目标视频流指针
 * @param audio_stream 目标音频流指针
 * @return 转换后的起始原点刻度；若上下文/流非法则返回 std::nullopt
 * @details
 * 用于消除媒体源原始时间戳的起始偏移，使播放器能够建立统一时间轴
 * 1. 优先使用 AVFormatContext::start_time 进行全局跨流时间轴对齐；
 * 2. 若容器未提供全局 start_time，降级使用视频流自身的 AVStream::start_time；
 * 3. 若均未提供，默认归零为 0
 */
std::optional<std::int64_t> DetermineTimeLineOriginTs(const AVFormatContext* av_format_ctx,
                                                      SelectedStreams streams) noexcept {
    // 1. 检验解复用上下文
    if (av_format_ctx == nullptr) {
        return std::nullopt;
    }

    if (av_format_ctx->start_time != AV_NOPTS_VALUE) {
        return av_format_ctx->start_time;
    }

    std::optional<std::int64_t> origin;
    for (const AVStream* stream : {streams.video_stream_, streams.audio_stream_}) {
        if (!stream || stream->start_time == AV_NOPTS_VALUE) {
            continue;
        }

        const auto start_us = TimestampToMicroseconds(stream->start_time, stream->time_base);

        if (!start_us) {
            return std::nullopt;
        }

        origin = origin ? std::min(*origin, *start_us) : *start_us;
    }

    return origin.value_or(0);
}

/**
 * @brief 将 FFmpeg 的 AVRational 转换为项目自定义的 Rational 结构体
 * @param value FFmpeg 原生有理数
 * @return 对应的 Rational 实例
 */
Rational ToRational(AVRational value) noexcept {
    if (value.num <= 0 || value.den <= 0) {
        return {};
    }
    return Rational{static_cast<std::int64_t>(value.num), static_cast<std::int64_t>(value.den)};
}

/**
 * @brief 将 FFmpeg AVPixelFormat 枚举映射为播放器内部的解耦枚举
 * @param format FFmpeg 原生像素格式（int）
 * @return 对应的 VideoPixelFormat 枚举值
 */
VideoPixelFormat ConvertVideoPixelFormat(int format) noexcept {
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
 * @brief 智能选取最适合用于 UI 界面展示的视频流名义帧率
 * @param context 解复用上下文
 * @param stream 视频流指针
 * @return 最佳候选帧率有理数
 * @details
 * 探测优先级：
 * 1. av_guess_frame_rate：综合容器头部与码流内部 SPS 参数推导的最佳实际帧率；
 * 2. avg_frame_rate：流统计平均帧率；
 * 3. r_frame_rate：容器基准最小时间刻度帧率。
 * 注意：该值仅供 UI 呈现或缺失 PTS 时的平滑兜底，严禁用于音画同步的物理节拍调度。
 */
AVRational SelectFrameRate(AVFormatContext* context, AVStream* stream) noexcept {
    // 1. 优先使用 FFmpeg 根据容器和流信息推断出的帧率
    const AVRational guessed = av_guess_frame_rate(context, stream, nullptr);
    if (guessed.num > 0 && guessed.den > 0) {
        return guessed;
    }

    // 2. 推断结果不可用时，使用流的 avg_frame_rate 平均帧率
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        return stream->avg_frame_rate;
    }

    // 3. 前两项不可用时，使用流提供的 r_frame_rate 作为回退值
    return stream->r_frame_rate;
}

/**
 * @brief 从底层 FFmpeg 结构体中复制提取播放器所需的解耦元数据
 * @details
 * 将容器、视频流和音频流的相关信息复制到 MediaInfo 中，
 * 避免 MediaInfo 依赖 AVFormatContext、AVStream 等 FFmpeg 对象的生命周期
 * 函数返回后，即使关闭 AVFormatContext，MediaInfo 中已复制的数据仍可使用
 */
MediaInfo BuildMediaInfo(AVFormatContext* context, AVStream* video_stream, AVStream* audio_stream) {
    MediaInfo info;

    // 1. 复制容器格式名称等容器级信息
    if (context->iformat != nullptr && context->iformat->name != nullptr) {
        info.container_name_ = context->iformat->name;
    }

    if (context->duration != AV_NOPTS_VALUE && context->duration >= 0) {
        info.duration_us_ = context->duration;
    }

    // 2. 根据 FFmpeg 标志和底层 I/O 能力，判断输入源是否具备普通 Seek 能力
    info.can_seek_ = (context->ctx_flags & AVFMTCTX_UNSEEKABLE) == 0 && context->pb != nullptr &&
        (context->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;

    // 3. 提取视频流基本属性
    if (video_stream) {
        VideoStreamInfo video_info;
        video_info.stream_index_ = video_stream->index;
        video_info.frame_rate_ = ToRational(SelectFrameRate(context, video_stream));
        video_info.time_base_ = ToRational(video_stream->time_base);

        if (video_stream->duration != AV_NOPTS_VALUE && video_stream->duration >= 0) {
            video_info.duration_us_ = TimestampToMicroseconds(video_stream->duration, video_stream->time_base);
        }

        // 3.1 复制视频编解码器参数，供 UI 展示和后续解码器配置使用
        const AVCodecParameters& parameters = *video_stream->codecpar;
        video_info.codec_id_ = static_cast<int>(parameters.codec_id);
        video_info.width_ = parameters.width;
        video_info.height_ = parameters.height;
        video_info.pixel_format_ = ConvertVideoPixelFormat(parameters.format);

        // 3.2 深拷贝关键配置头（如 H.264/H.265 的 SPS/PPS 头 extradata）
        if (parameters.extradata != nullptr && parameters.extradata_size > 0) {
            video_info.extradata_.assign(parameters.extradata, parameters.extradata + parameters.extradata_size);
        }

        info.video_stream_ = std::move(video_info);
    }

    // 4. 提取音频流基本属性
    if (audio_stream) {
        AudioStreamInfo audio_info;
        const AVCodecParameters& parameters = *audio_stream->codecpar;
        audio_info.stream_index_ = audio_stream->index;
        audio_info.time_base_ = ToRational(audio_stream->time_base);
        audio_info.codec_id_ = static_cast<int>(parameters.codec_id);
        audio_info.sample_rate_hz_ = parameters.sample_rate;
        audio_info.channel_count_ = parameters.ch_layout.nb_channels;

        // 4.1 将 FFmpeg 的声道布局信息转换为可读字符串，例如 "mono" "stereo" 或 "5.1"
        char layout[256]{};

        // 4.2 转换成功后，将声道布局字符串保存到项目自己的音频元数据中
        // 参数依次为：待转换的声道布局、存放结果的缓冲区、缓冲区大小
        if (av_channel_layout_describe(&parameters.ch_layout, layout, sizeof(layout)) >= 0) {
            audio_info.channel_layout_ = layout;
        }

        // 4.3 将 FFmpeg 的采样格式映射为项目内部的 AudioSampleFormat 枚举
        switch (static_cast<AVSampleFormat>(parameters.format)) {
            case AV_SAMPLE_FMT_U8:
                audio_info.sample_format_ = AudioSampleFormat::kU8;
                break;
            case AV_SAMPLE_FMT_S16:
                audio_info.sample_format_ = AudioSampleFormat::kS16;
                break;
            case AV_SAMPLE_FMT_S32:
                audio_info.sample_format_ = AudioSampleFormat::kS32;
                break;
            case AV_SAMPLE_FMT_S64:
                audio_info.sample_format_ = AudioSampleFormat::kS64;
                break;
            case AV_SAMPLE_FMT_FLT:
                audio_info.sample_format_ = AudioSampleFormat::kFloat;
                break;
            case AV_SAMPLE_FMT_DBL:
                audio_info.sample_format_ = AudioSampleFormat::kDouble;
                break;
            case AV_SAMPLE_FMT_U8P:
                audio_info.sample_format_ = AudioSampleFormat::kU8Planar;
                break;
            case AV_SAMPLE_FMT_S16P:
                audio_info.sample_format_ = AudioSampleFormat::kS16Planar;
                break;
            case AV_SAMPLE_FMT_S32P:
                audio_info.sample_format_ = AudioSampleFormat::kS32Planar;
                break;
            case AV_SAMPLE_FMT_S64P:
                audio_info.sample_format_ = AudioSampleFormat::kS64Planar;
                break;
            case AV_SAMPLE_FMT_FLTP:
                audio_info.sample_format_ = AudioSampleFormat::kFloatPlanar;
                break;
            case AV_SAMPLE_FMT_DBLP:
                audio_info.sample_format_ = AudioSampleFormat::kDoublePlanar;
                break;
            default:
                break;
        }

        if (audio_stream->duration != AV_NOPTS_VALUE && audio_stream->duration >= 0) {
            audio_info.duration_us_ = TimestampToMicroseconds(audio_stream->duration, audio_stream->time_base);
        }

        if (parameters.extradata && parameters.extradata_size > 0) {
            audio_info.extradata_.assign(parameters.extradata, parameters.extradata + parameters.extradata_size);
        }

        info.audio_stream_ = std::move(audio_info);
    }
    return info;
}

/**
 * @brief 在执行 Seek 定位前清除 AVIOContext 上一轮残留的 EOF 与错误标志
 * @param context 解复用上下文
 * @note Seek 操作重置了读取指针，必须复位老状态，避免上一轮的 EOF 阻止新位置的正常读取
 */
void ResetAvioReadStateForSeek(AVFormatContext* context) noexcept {
    if (context == nullptr || context->pb == nullptr) {
        return;
    }

    context->pb->error = 0;
    context->pb->eof_reached = 0;
}

}  // namespace

/**
 * @brief 构造函数：初始化中断状态机
 */
DemuxerInterruptHandle::DemuxerInterruptHandle() : state_(std::make_shared<State>()) {}

/**
 * @brief 发起永久停止请求
 */
void DemuxerInterruptHandle::RequestStop() noexcept {
    if (state_) {
        state_->stop_requested_.store(true, std::memory_order_release);
    }
}

/**
 * @brief 检查是否已经发起永久停止请求
 */
bool DemuxerInterruptHandle::IsStopRequested() const noexcept {
    return state_ && state_->stop_requested_.load(std::memory_order_acquire);
}

/**
 * @brief 请求中断当前阻塞 I/O，以便工作线程尽快处理新的 Seek 请求
 * @details 通过递增请求代数，使当前 I/O 保存的代数基准失效
 */
void DemuxerInterruptHandle::RequestSeekInterrupt() noexcept {
    if (state_) {
        state_->seek_cancel_generation_.fetch_add(1, std::memory_order_release);
    }
}

/**
 * @brief 在开始下一次可中断 I/O 前，记录当前 Seek 请求代数
 * @details 后续若有新的 Seek 请求，代数发生变化，FFmpeg 中断回调便会请求中断该次 I/O
 */
void DemuxerInterruptHandle::PrepareNextIoForSeekInterrupt() noexcept {
    if (state_) {
        const std::uint64_t generation = state_->seek_cancel_generation_.load(std::memory_order_acquire);
        state_->armed_generation_.store(generation, std::memory_order_release);
    }
}

/**
 * @brief 查询当前 I/O 是否需要中断，并区分 Stop 和 Seek 原因
 * @details
 * Stop 优先级高于 Seek；若请求代数与当前 I/O 的基准代数不同，则表示有新的 Seek 请求
 */
DemuxerInterruptReason DemuxerInterruptHandle::GetInterruptReason() const noexcept {
    if (!state_) {
        return DemuxerInterruptReason::kNone;
    }

    if (state_->stop_requested_.load(std::memory_order_acquire)) {
        return DemuxerInterruptReason::kStop;
    }

    const std::uint64_t requested = state_->seek_cancel_generation_.load(std::memory_order_acquire);
    const std::uint64_t armed = state_->armed_generation_.load(std::memory_order_acquire);

    return (requested == armed) ? DemuxerInterruptReason::kNone : DemuxerInterruptReason::kSeek;
}

/**
 * @brief 私有构造函数：正式接管已完全打开的解封装上下文
 */
FfmpegDemuxer::FfmpegDemuxer(AVFormatContext* av_format_ctx,
                             MediaInfo media_info,
                             SelectedStreamIndices stream_indices,
                             DemuxerInterruptHandle interrupt,
                             std::int64_t timeline_origin_us) noexcept
    : av_format_ctx_(av_format_ctx),
      media_info_(std::move(media_info)),
      video_stream_index_(stream_indices.video_stream_index_),
      audio_stream_index_(stream_indices.audio_stream_index_),
      interrupt_(std::move(interrupt)),
      timeline_origin_us_(timeline_origin_us) {}

/**
 * @brief 析构函数：释放底层 AVFormatContext 资源
 */
FfmpegDemuxer::~FfmpegDemuxer() {
    LOG_INFO("FfmpegDemuxer destroyed, closing input");
    if (av_format_ctx_ != nullptr) {
        avformat_close_input(&av_format_ctx_);
    }
}

/**
 * @brief 挂载至 FFmpeg AVIOInterruptCB 的 C 接口静态中断回调
 * @param opaque 注册时传入的用户指针（指向 DemuxerInterruptHandle::State）
 * @return 0 继续 I/O 操作；1 立即中断当前阻塞调用
 * @details
 * 该函数在 FFmpeg 内部频繁被底层 Socket、文件读取轮询调用。
 * 必须保持极简与无锁，严禁在此执行任何耗时加锁操作。
 */
int FfmpegDemuxer::InterruptCallback(void* opaque) noexcept {
    const auto* state = static_cast<const DemuxerInterruptHandle::State*>(opaque);
    if (state == nullptr) {
        return kContinueFfmpegIo;
    }

    // 1. 永久停止拥有绝对最高优先级
    if (state->stop_requested_.load(std::memory_order_acquire)) {
        return kAbortFfmpegIo;
    }

    // 2. 检测是否存在更新的 Seek 发生（请求世代号与武装世代号不一致）
    const bool new_seek_requested = state->seek_cancel_generation_.load(std::memory_order_acquire) !=
        state->armed_generation_.load(std::memory_order_acquire);

    return new_seek_requested ? kAbortFfmpegIo : kContinueFfmpegIo;
}

/**
 * @brief 打开媒体输入源，探测音视频流信息并创建 FfmpegDemuxer
 * @details
 * 执行打开、探测流信息、音视频流选择、参数复制和元数据构建等操作，上层可以在线程中调用本函数实现异步打开
 */
FfmpegDemuxerOpenResult FfmpegDemuxer::Open(const std::string& uri, DemuxerInterruptHandle interrupt) {
    FfmpegDemuxerOpenResult result;

    // 1. 入参合法性校验：拒绝空 URI
    if (uri.empty()) {
        LOG_WARN("FfmpegDemuxer::Open rejected an empty URI");
        result.error_ = MakeFfmpegError(kOpenInputOperation, AVERROR(EINVAL));
        return result;
    }

    // 2. 前置中断检测：避免在已经触发停止的情况下盲目发起阻塞网络握手
    if (interrupt.IsStopRequested()) {
        LOG_DEBUG("FfmpegDemuxer::Open cancelled before opening '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    // 3. 分配基础的解封装上下文
    AVFormatContext* format_context = avformat_alloc_context();
    if (format_context == nullptr) {
        LOG_ERROR("avformat_alloc_context failed while opening '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenInputOperation, AVERROR(ENOMEM));
        return result;
    }

    // 4. 注册中断回调，使打开网络输入或执行其他阻塞 I/O 时能够响应外部 Stop/Seek 请求
    format_context->interrupt_callback = AVIOInterruptCB{&FfmpegDemuxer::InterruptCallback, interrupt.state_.get()};

    // 5. 打开输入源文件/流（读取文件头部，可能发生网络握手阻塞）
    const int open_result = avformat_open_input(&format_context, uri.c_str(), nullptr, nullptr);

    // 6. 检查打开阶段是否被异步打断
    if (interrupt.IsStopRequested()) {
        if (format_context != nullptr) {
            avformat_close_input(&format_context);
        }
        LOG_DEBUG("FfmpegDemuxer::Open cancelled while opening '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    // 7. 处理打开失败错误
    if (open_result < 0) {
        if (format_context != nullptr) {
            avformat_close_input(&format_context);
        }
        LOG_ERROR("avformat_open_input failed for '{}', code {}", uri, open_result);
        result.error_ = MakeFfmpegError(kOpenInputOperation, open_result);
        return result;
    }

    // 8. 移交临时 RAII 对象管理：从此处起，后续任意提前 return 均会自动调用 avformat_close_input
    FormatContextOwner context_owner(format_context);

    if (interrupt.IsStopRequested()) {
        LOG_DEBUG("FfmpegDemuxer::Open cancelled before reading stream information for '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    // 9. 探测码流信息（解码少量数据包以确定码率、分辨率、CodecID、TimeBase 等）
    const int stream_info_result = avformat_find_stream_info(context_owner.get(), nullptr);

    if (interrupt.IsStopRequested()) {
        LOG_DEBUG("FfmpegDemuxer::Open cancelled while reading stream information for '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    if (stream_info_result < 0) {
        LOG_ERROR("avformat_find_stream_info failed for '{}', code {}", uri, stream_info_result);
        result.error_ = MakeFfmpegError(kFindStreamInfoOperation, stream_info_result);
        return result;
    }

    // 10. 分别选择 FFmpeg 认为最合适的一条视频流和一条音频流；媒体可以只包含其中一种流
    const int video_stream_index = av_find_best_stream(context_owner.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    const int audio_stream_index = av_find_best_stream(context_owner.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // 音频和视频流都没有找到，当前媒体不具备可播放的音视频内容
    if (video_stream_index < 0 && audio_stream_index < 0) {
        LOG_ERROR("Neither a usable video stream nor a usable audio stream was found in '{}'", uri);
        result.error_ = MakeFfmpegError(kFindStreamsOperation, AVERROR_STREAM_NOT_FOUND);
        return result;
    }

    // 获取媒体中的流总数
    const auto stream_count = context_owner->nb_streams;
    // 判断选中的索引是否越界
    const bool selected_index_out_of_range =
        (video_stream_index >= 0 && static_cast<unsigned>(video_stream_index) >= stream_count) ||
        (audio_stream_index >= 0 && static_cast<unsigned>(audio_stream_index) >= stream_count);

    // 如果越界
    if (selected_index_out_of_range) {
        LOG_ERROR(
            "FFmpeg returned an out-of-range selected audio/video stream index in '{}' (video {}, audio {}, "
            "stream_count {})",
            uri,
            video_stream_index,
            audio_stream_index,
            stream_count);
        result.error_ = MakeFfmpegError("validate selected stream index", AVERROR_INVALIDDATA);
        return result;
    }

    // 11. 只有确认索引在容器范围内后，才从 streams[] 取得对应指针；缺少某条轨道时保留 nullptr
    AVStream* video_stream = video_stream_index >= 0 ? context_owner->streams[video_stream_index] : nullptr;

    AVStream* audio_stream = audio_stream_index >= 0 ? context_owner->streams[audio_stream_index] : nullptr;

    if ((video_stream_index >= 0 && !video_stream) || (audio_stream_index >= 0 && !audio_stream)) {
        LOG_ERROR("Selected audio/video stream pointer is null in '{}' (video {}, audio {})",
                  uri,
                  video_stream_index,
                  audio_stream_index);

        result.error_ = MakeFfmpegError("validate selected stream", AVERROR_INVALIDDATA);

        return result;
    }

    for (const AVStream* stream : {video_stream, audio_stream}) {
        if (stream && (!stream->codecpar || !IsValidTimeBase(stream->time_base))) {
            LOG_ERROR("Selected audio/video stream has missing codec parameters or invalid time base in '{}'", uri);

            result.error_ = MakeFfmpegError("validate selected stream", AVERROR_INVALIDDATA);

            return result;
        }
    }

    // 12. 两路使用同一个微秒原点，避免分别归零后丢失源文件已有的音视频时间偏移
    const std::optional<std::int64_t> timeline_origin_us =
        DetermineTimeLineOriginTs(context_owner.get(), SelectedStreams{video_stream, audio_stream});

    if (!timeline_origin_us.has_value()) {
        LOG_ERROR("Cannot determine common audio/video playback timeline origin in '{}' (video {}, audio {})",
                  uri,
                  video_stream_index,
                  audio_stream_index);
        result.error_ = MakeFfmpegError(kTimelineOriginOperation, AVERROR(ERANGE));
        return result;
    }

    // 13. 拷贝编解码器配置参数供外部 Decoder 初始化使用
    // VideoCodecParameters::CopyFrom() 同时负责分配、深拷贝（包括 extradata）和 RAII 释放
    // 因而这里不再把裸 AVCodecParameters 指针暴露给参数容器
    VideoCodecParameters video_parameters_owner;
    AudioCodecParameters audio_parameters_owner;

    if (video_stream) {
        video_parameters_owner = VideoCodecParameters::CopyFrom(video_stream->codecpar);
    }

    if (audio_stream) {
        audio_parameters_owner = AudioCodecParameters::CopyFrom(audio_stream->codecpar);
    }

    // 14. MediaInfo::IsValid() 只要求至少一轨有效
    MediaInfo media_info = BuildMediaInfo(context_owner.get(), video_stream, audio_stream);

    if (!media_info.IsValid() || media_info.video_stream_ && !media_info.video_stream_->IsValid()) {
        LOG_ERROR("Failed to build valid audio/video media information in '{}' (video {}, audio {})",
                  uri,
                  video_stream_index,
                  audio_stream_index);
        result.error_ = MakeFfmpegError(kBuildMediaInfoOperation, AVERROR_INVALIDDATA);
        return result;
    }

    if (interrupt.IsStopRequested()) {
        LOG_DEBUG("FfmpegDemuxer::Open cancelled after reading stream information for '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    // 15. 实例化 FfmpegDemuxer 托管句柄
    try {
        result.demuxer_.reset(
            new FfmpegDemuxer(context_owner.get(),
                              std::move(media_info),
                              FfmpegDemuxer::SelectedStreamIndices{video_stream ? video_stream_index : -1,
                                                                   audio_stream ? audio_stream_index : -1},
                              std::move(interrupt),
                              *timeline_origin_us));
    } catch (const std::bad_alloc&) {
        LOG_ERROR("Failed to create FfmpegDemuxer for '{}' due to insufficient memory", uri);
        result.error_ = MakeFfmpegError(kOpenInputOperation, AVERROR(ENOMEM));
        return result;
    }

    // 16.  移交所有权给 FfmpegDemuxer，放弃临时 RAII 对象的析构管理
    static_cast<void>(context_owner.release());
    result.video_codec_parameters_ = std::move(video_parameters_owner);
    result.audio_codec_parameters_ = std::move(audio_parameters_owner);

    const MediaInfo& opened_media_info = result.demuxer_->GetMediaInfo();
    if (opened_media_info.video_stream_) {
        const VideoStreamInfo& video_stream_info = opened_media_info.video_stream_.value();

        LOG_INFO("Demuxer opened successfully, video stream {}: {}x{}, codec {}, pixel format {}, {} fps",
                 video_stream_index,
                 video_stream_info.width_,
                 video_stream_info.height_,
                 avcodec_get_name(static_cast<AVCodecID>(opened_media_info.video_stream_->codec_id_)),
                 VideoPixelFormatToString(video_stream_info.pixel_format_),
                 video_stream_info.frame_rate_.ToDouble());
    }
    if (opened_media_info.audio_stream_) {
        const auto& audio = *opened_media_info.audio_stream_;
        LOG_INFO("Demuxer opened audio stream {}: {} Hz, {} channels, codec {}, common origin {} us",
                 audio.stream_index_,
                 audio.sample_rate_hz_,
                 audio.channel_count_,
                 avcodec_get_name(static_cast<AVCodecID>(audio.codec_id_)),
                 *timeline_origin_us);
    }

    return result;
}

/**
 * @brief 从解复用器中读取一个压缩数据包
 * @param packet 输出的独立数据包容器引用
 * @return FfmpegReadResult 包含读取状态与错误详情
 */
FfmpegReadResult FfmpegDemuxer::Read(Packet& packet) {
    // 1. 内部状态合法性检查
    if (av_format_ctx_ == nullptr || !packet.IsValid() || (video_stream_index_ < 0 && audio_stream_index_ < 0)) {
        LOG_ERROR(
            "FfmpegDemuxer::Read rejected: invalid state "
            "(av_format_ctx={}, packet_valid={}, video_stream={}, audio_stream={}, stream_count={})",
            av_format_ctx_ != nullptr,
            packet.IsValid(),
            video_stream_index_,
            audio_stream_index_,
            av_format_ctx_ != nullptr ? av_format_ctx_->nb_streams : 0U);

        return {FfmpegReadStatus::kError, MakeFfmpegError(kReadFrameOperation, AVERROR(EINVAL))};
    }

    // 2. 复位输出 Packet，确保其持有干净的原生 AVPacket 缓冲
    AVPacket* raw_packet = packet.GetAVPacket();
    av_packet_unref(raw_packet);
    packet.ResetDemuxMetadata();

    // 3. 读取前中断检测
    const DemuxerInterruptReason before_read = interrupt_.GetInterruptReason();
    if (before_read == DemuxerInterruptReason::kStop) {
        return {FfmpegReadStatus::kStopInterrupted, {}};
    }
    if (before_read == DemuxerInterruptReason::kSeek) {
        return {FfmpegReadStatus::kSeekInterrupted, {}};
    }
    if (eof_reached_) {
        return {FfmpegReadStatus::kEndOfStream, {}};
    }

    // 4. 执行底层解包读取（可能涉及阻塞 I/O）
    const int read_result = av_read_frame(av_format_ctx_, raw_packet);

    // 5. 校验阻塞返回后的中断状态
    const DemuxerInterruptReason after_read = interrupt_.GetInterruptReason();
    if (after_read == DemuxerInterruptReason::kStop) {
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kStopInterrupted, {}};
    }
    if (after_read == DemuxerInterruptReason::kSeek) {
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kSeekInterrupted, {}};
    }

    // 6. 流结束处理
    if (read_result == AVERROR_EOF) {
        eof_reached_ = true;
        return {FfmpegReadStatus::kEndOfStream, {}};
    }

    // 7. 底层读取错误处理
    if (read_result < 0) {
        LOG_ERROR("av_read_frame failed for video stream {}, code {}", video_stream_index_, read_result);
        return {FfmpegReadStatus::kError, MakeFfmpegError(kReadFrameOperation, read_result)};
    }

    // 8. 检查数据包的流索引是否有效，非法索引属于异常数据包，不能当作字幕等非目标流直接跳过
    if (raw_packet->stream_index < 0 ||
        static_cast<unsigned int>(raw_packet->stream_index) >= av_format_ctx_->nb_streams) {
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kError, MakeFfmpegError(kReadFrameOperation, AVERROR_INVALIDDATA)};
    }

    // 9. 向已经选中的两路解码器发送数据，字幕和其他音频轨不会占用它们的队列
    if (raw_packet->stream_index != video_stream_index_ && raw_packet->stream_index != audio_stream_index_) {
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kSkipped, {}};
    }

    const AVStream* stream = av_format_ctx_->streams[raw_packet->stream_index];
    if (!stream || !IsValidTimeBase(stream->time_base)) {
        av_packet_unref(raw_packet);

        return {FfmpegReadStatus::kError, MakeFfmpegError(kReadFrameOperation, AVERROR_INVALIDDATA)};
    }

    // AVPacket 的 PTS/DTS 仍以所属流的 tick 为单位；先把共同微秒原点换算到该流再相减
    // 视频和音频的 time_base 可以不同，不能直接复用视频流的原点 tick
    const std::int64_t timeline_origin_ts = av_rescale_q(timeline_origin_us_, AV_TIME_BASE_Q, stream->time_base);

    if (timeline_origin_ts == AV_NOPTS_VALUE) {
        av_packet_unref(raw_packet);

        return {FfmpegReadStatus::kError, MakeFfmpegError(kNormalizeTimestampOperation, AVERROR(EAGAIN))};
    }

    // 10. 将底层 AVPacket 的原始 PTS/DTS 平移归一化到播放器 0 基准时间轴
    const auto normalize_timestamp = [timeline_origin_ts](std::int64_t value,
                                                          std::int64_t& normalized) noexcept -> bool {
        if (value == AV_NOPTS_VALUE) {
            normalized = AV_NOPTS_VALUE;
            return true;
        }

        // 防御减法向下溢出
        if (timeline_origin_ts > 0 && value < std::numeric_limits<std::int64_t>::min() + timeline_origin_ts) {
            return false;
        }
        // 防御减负数（相当于加法）向上溢出
        if (timeline_origin_ts < 0 && value > std::numeric_limits<std::int64_t>::max() + timeline_origin_ts) {
            return false;
        }

        const std::int64_t shifted = value - timeline_origin_ts;
        if (shifted == AV_NOPTS_VALUE) {
            return false;
        }

        normalized = shifted;
        return true;
    };

    // 11. 分别准备归一化后的 PTS 和 DTS
    std::int64_t normalized_pts = AV_NOPTS_VALUE;
    std::int64_t normalized_dts = AV_NOPTS_VALUE;

    // 任意一个时间戳发生溢出或得到非法结果，都不能继续使用当前数据包
    if (!normalize_timestamp(raw_packet->pts, normalized_pts) ||
        !normalize_timestamp(raw_packet->dts, normalized_dts)) {
        LOG_ERROR("Packet timestamp overflow while normalizing stream {}", raw_packet->stream_index);
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kError, MakeFfmpegError(kNormalizeTimestampOperation, AVERROR(ERANGE))};
    }

    // 12. 将归一化后的 PTS/DTS 写回底层 AVPacket
    raw_packet->pts = normalized_pts;
    raw_packet->dts = normalized_dts;

    // 13. 计算数据包的持续时间
    const std::optional<std::int64_t> packet_duration_us =
        raw_packet->duration > 0 ? TimestampToMicroseconds(raw_packet->duration, stream->time_base) : std::nullopt;

    // 14. 保存供播放器上层使用的解复用元数据
    packet.SetDemuxMetadata(Packet::DemuxMetadata{raw_packet->stream_index,
                                                  raw_packet->size > 0 ? static_cast<std::size_t>(raw_packet->size) : 0,
                                                  (raw_packet->flags & AV_PKT_FLAG_KEY) != 0,
                                                  TimestampToMicroseconds(raw_packet->pts, stream->time_base),
                                                  TimestampToMicroseconds(raw_packet->dts, stream->time_base),
                                                  packet_duration_us});

    return {FfmpegReadStatus::kPacket, {}};
}

/**
 * @brief 判断当前媒体输入是否运行尝试 Seek
 * @return false 表示解封装上下文不存在或 FFmpeg 已经明确标记当前输入不可 Seek
 * 返回 true 表示没有发现明确不允许 Seek 的标记，可以继续尝试 Seek
 */
bool FfmpegDemuxer::CanAttemptSeek() const noexcept {
    // 如果没有有效的解封装上下文，不允许 Seek
    if (av_format_ctx_ == nullptr) {
        return false;
    }

    // ctx_flags 是 FFmpeg 解封装上下文中的标志位集合
    // 结果非零：已经明确标记为不可定位，结果为零：没有发现明确的不可定位标记，可以继续尝试 Seek
    return (av_format_ctx_->ctx_flags & AVFMTCTX_UNSEEKABLE) == 0;
}

/**
 * @brief 将播放器零起点时间轴上的目标位置转换为 FFmpeg 时间戳并执行定位
 * @param target_us 基于 0 原点的目标微秒位置（必须 >= 0）
 * @return FfmpegSeekResult
 * kSucceeded：FFmpeg 底层定位成功
 * kError：参数、上下文或底层定位发生错误
 * kStopInterrupted：定位过程被 Stop 请求打断
 * kSeekInterrupted：定位过程又被更新的 Seek 请求打断
 *
 */
FfmpegSeekResult FfmpegDemuxer::Seek(std::int64_t target_us) {
    // 1. 选择用于 Seek 的参考流：优先使用视频流，涉及到关键帧
    const int seek_stream_index = video_stream_index_ >= 0 ? video_stream_index_ : audio_stream_index_;

    // 2. 检查解复用上下文和参考流索引
    if (av_format_ctx_ == nullptr || seek_stream_index < 0 ||
        static_cast<unsigned int>(seek_stream_index) >= av_format_ctx_->nb_streams) {
        LOG_ERROR(
            "FfmpegDemuxer::Seek to {} us rejected: invalid state "
            "(av_format_ctx = {}, seek_stream_index = {}, "
            "video_stream_index = {}, audio_stream_index = {}, stream_count = {})",
            target_us,
            av_format_ctx_ != nullptr,
            seek_stream_index,
            video_stream_index_,
            audio_stream_index_,
            av_format_ctx_ != nullptr ? av_format_ctx_->nb_streams : 0U);

        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(EAGAIN))};
    }

    // 3. 检查目标播放时间是否合法：播放器采用从 0 开始的时间轴因此，传入的目标时间不能是负数
    if (target_us < 0) {
        LOG_ERROR("FfmpegDemuxer::Seek rejected negative target {} us", target_us);

        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(EINVAL))};
    }

    // 4. 检查定位前的中断请求:Stop：播放器正在关闭或停止，当前定位不应继续,
    // Seek：已经有更新的 Seek 请求到达，当前请求已经失去处理价值,应当交给上层处理更新的定位请求
    const DemuxerInterruptReason before_seek = interrupt_.GetInterruptReason();

    if (before_seek == DemuxerInterruptReason::kStop) {
        return {FfmpegSeekStatus::kStopInterrupted, std::nullopt, {}};
    }

    if (before_seek == DemuxerInterruptReason::kSeek) {
        return {FfmpegSeekStatus::kSeekInterrupted, std::nullopt, {}};
    }

    // 5. 获取参考流并检查时间基：取得前面选中的参考流，之后所有目标时间转换都使用这条流的 time_base
    const AVStream* stream = av_format_ctx_->streams[seek_stream_index];

    if (stream == nullptr || !IsValidTimeBase(stream->time_base)) {
        LOG_ERROR("FfmpegDemuxer::Seek cannot use stream {}: stream is null or time_base is invalid",
                  seek_stream_index);

        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR_INVALIDDATA)};
    }

    // 6. 将播放器目标时间转换为参考流的时间戳,target_us 是播放器零起点时间轴上的目标位置，单位为微秒
    const std::int64_t target_offset_ts = av_rescale_q(target_us, AV_TIME_BASE_Q, stream->time_base);

    // timeline_origin_us_ 是媒体源原始时间轴的共同起始时间
    const std::int64_t timeline_origin_ts = av_rescale_q(timeline_origin_us_, AV_TIME_BASE_Q, stream->time_base);

    // 检查两个时间戳换算结果,timeline_origin_ts 也必须是有效的时间戳，否则无法恢复媒体源原始时间轴上的定位位置
    if (target_offset_ts == AV_NOPTS_VALUE || target_offset_ts < 0 || timeline_origin_ts == AV_NOPTS_VALUE) {
        LOG_ERROR("FfmpegDemuxer::Seek cannot convert target {} us to stream timestamp", target_us);

        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(EINVAL))};
    }

    // 7. 检查恢复原始时间戳时是否发生整数溢出，最终需要计算：target_ts = timeline_origin_ts + target_offset_ts
    if (timeline_origin_ts > std::numeric_limits<std::int64_t>::max() - target_offset_ts) {
        LOG_ERROR("FfmpegDemuxer::Seek target {} us is outside timestamp range", target_us);

        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(ERANGE))};
    }

    // 8. 恢复媒体源原始时间轴上的目标时间戳:将播放器的零起点目标时间换算回媒体源原来的绝对时间戳
    // 举例：媒体原始起始时间 = 10 秒，播放器希望跳转到 = 2 秒
    // 那么底层 FFmpeg 实际需要定位到媒体原始时间轴上的 12 秒
    const std::int64_t target_ts = timeline_origin_ts + target_offset_ts;

    // 9. 清理上一次读取留下的 AVIO 状态
    ResetAvioReadStateForSeek(av_format_ctx_);

    // 10. 调用 FFmpeg 执行真正的底层定位
    const int seek_result = av_seek_frame(av_format_ctx_, seek_stream_index, target_ts, AVSEEK_FLAG_BACKWARD);

    // 11. 检查定位过程中是否出现新的中断请求：av_seek_frame() 也可能执行阻塞 I/O
    const DemuxerInterruptReason after_seek = interrupt_.GetInterruptReason();

    if (after_seek == DemuxerInterruptReason::kStop) {
        return {FfmpegSeekStatus::kStopInterrupted, std::nullopt, {}};
    }

    if (after_seek == DemuxerInterruptReason::kSeek) {
        return {FfmpegSeekStatus::kSeekInterrupted, std::nullopt, {}};
    }

    // 12. 处理 FFmpeg 底层定位失败：av_seek_frame() 返回负数表示定位失败
    if (seek_result < 0) {
        LOG_ERROR("av_seek_frame failed for stream {}; target {} us, raw stream timestamp {}, code {}",
                  seek_stream_index,
                  target_us,
                  target_ts,
                  seek_result);

        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, seek_result)};
    }

    // 13. 更新解复用器的 EOF 状态
    eof_reached_ = false;

    LOG_INFO("Demuxer accepted seek for stream {}; target {} us, raw stream timestamp {}",
             seek_stream_index,
             target_us,
             target_ts);
    return {FfmpegSeekStatus::kSucceeded, std::nullopt, {}};
}

}  // namespace mediacore

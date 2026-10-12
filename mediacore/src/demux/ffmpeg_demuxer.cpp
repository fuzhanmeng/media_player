/**
 * @file ffmpeg_demuxer.cpp
 * @brief 基于 FFmpeg 实现的媒体解封装核心逻辑（Open / Read / Seek / Interrupt）
 * @details
 * 负责输入文件的打开探测、多流筛选以及音视频数据包解封装。
 * 核心机制包括：
 * 1. 协作式中断检查：将 DemuxerInterruptHandle::State 挂入 AVIOInterruptCB，支持精准打断阻塞 I/O；
 * 2. 时间轴归一化：消除源文件原生非零 start_time 偏移，统一输出基于 0 起点的播放器时间轴；
 * 3. RAII 异常安全：使用 FormatContextCloser 保证打开与探测中途出错时安全调用 avformat_close_input。
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
 * @brief 共享中断状态内部实现结构体
 */
struct DemuxerInterruptHandle::State final {
    /// 永久停止请求标志位；置为 true 后不可逆，促使所有正在阻塞或后续的 FFmpeg 调用快速退出
    std::atomic<bool> stop_requested_{false};

    /// 全局累计的 Seek 请求序号；每次外部触发新 Seek 时原子递增
    std::atomic<std::uint64_t> seek_cancel_generation_{0};

    /// 工作线程在当前 I/O 操作启动前记录并武装的 Seek 序号基准
    std::atomic<std::uint64_t> armed_generation_{0};
};

namespace {

constexpr const char* kOpenInputOperation = "avformat_open_input";
constexpr const char* kFindStreamInfoOperation = "avformat_find_stream_info";
constexpr const char* kFindVideoStreamOperation = "av_find_best_stream(video)";
constexpr const char* kCloneCodecParametersOperation = "avcodec_parameters_copy(video)";
constexpr const char* kReadFrameOperation = "av_read_frame";
constexpr const char* kSeekFrameOperation = "av_seek_frame";
constexpr const char* kOpenCancelledOperation = "FfmpegDemuxer::Open cancelled";
constexpr const char* kModuleName = "FfmpegDemuxer";
constexpr const char* kTimelineOriginOperation = "determine playback timeline origin";
constexpr const char* kNormalizeTimestampOperation = "normalize packet timestamp";

// FFmpeg AVIOInterruptCB 约定返回值：0 表示继续 I/O，1 表示立即中断
constexpr int kContinueFfmpegIo = 0;
constexpr int kAbortFfmpegIo = 1;

/**
 * @struct FormatContextCloser
 * @brief 自定义删除器：确保 AVFormatContext 离开作用域时调用 avformat_close_input
 */
struct FormatContextCloser final {
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

using FormatContextOwner = std::unique_ptr<AVFormatContext, FormatContextCloser>;

/**
 * @brief 将 FFmpeg 底层错误码转换为项目标准的 MediaError
 * @param operation 触发错误的 FFmpeg 函数或操作名
 * @param code FFmpeg 返回的负数错误码
 */
MediaError MakeFfmpegError(const char* operation, int code) {
    return ffmpeg_detail::MakeFfmpegMediaError(kModuleName, code, operation);
}

/**
 * @brief 校验 FFmpeg 时间基（AVRational）有效性
 */
bool IsValidTimeBase(AVRational time_base) noexcept {
    return time_base.den > 0 && time_base.num > 0;
}

/**
 * @brief 将基于流时间基的时间戳换算为微秒（us）
 * @param value 原始时间戳刻度
 * @param time_base 该时间戳所依据的流时间基
 * @return 转换后的微秒值；若时间戳非法或时间基无效则返回 std::nullopt
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
 * @brief 计算播放时间 0 在视频流原始时间基下对应的起始时间戳
 * @details
 * 消除媒体源非零起始时间的偏移（例如 TS 流、网络直播分段）：
 * 1. 优先使用 AVFormatContext::start_time 进行统一全局时间对齐；
 * 2. 若容器未提供，退回使用视频流自身的 AVStream::start_time；
 * 3. 均未提供时退回为 0。
 */
std::optional<std::int64_t> DetermineTimeLineOriginTs(const AVFormatContext* av_format_ctx,
                                                      const AVStream* video_stream) noexcept {
    if (av_format_ctx == nullptr || video_stream == nullptr || !IsValidTimeBase(video_stream->time_base)) {
        return std::nullopt;
    }

    if (av_format_ctx->start_time != AV_NOPTS_VALUE) {
        const std::int64_t origin_ts =
            av_rescale_q(av_format_ctx->start_time, AV_TIME_BASE_Q, video_stream->time_base);
        if (origin_ts == AV_NOPTS_VALUE) {
            return std::nullopt;
        }
        return origin_ts;
    }

    if (video_stream->start_time != AV_NOPTS_VALUE) {
        return video_stream->start_time;
    }

    return std::int64_t{0};
}

/**
 * @brief 将 FFmpeg 的 AVRational 转换为项目自定义的 Rational 结构体
 */
Rational ToRational(AVRational value) noexcept {
    if (value.num <= 0 || value.den <= 0) {
        return {};
    }
    return Rational{static_cast<std::uint64_t>(value.num), static_cast<std::uint64_t>(value.den)};
}

/**
 * @brief 将 FFmpeg AVPixelFormat 转换为播放器项目内部的枚举
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
 * @brief 择优选取用于 UI 显示的视频流帧率
 * @details
 * 优先级：av_guess_frame_rate > avg_frame_rate > r_frame_rate。
 * 该数值仅供展示与元数据汇报，严禁用于实际 PTS 时间戳计算。
 */
AVRational SelectFrameRate(AVFormatContext* context, AVStream* stream) noexcept {
    // 1. FFmpeg 容器与编解码器综合智能推断
    const AVRational guessed = av_guess_frame_rate(context, stream, nullptr);
    if (guessed.num > 0 && guessed.den > 0) {
        return guessed;
    }

    // 2. avg_frame_rate 平均帧率
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        return stream->avg_frame_rate;
    }

    // 3. r_frame_rate 容器基准最小时间刻度帧率
    return stream->r_frame_rate;
}

/**
 * @brief 从底层 FFmpeg 结构体中复制提取播放器所需的解耦元数据
 */
MediaInfo BuildMediaInfo(AVFormatContext* context, AVStream* video_stream) {
    MediaInfo info;

    // 1. 复制容器层信息
    if (context->iformat != nullptr && context->iformat->name != nullptr) {
        info.container_name_ = context->iformat->name;
    }

    if (context->duration != AV_NOPTS_VALUE && context->duration >= 0) {
        info.duration_us_ = context->duration;
    }

    // 判断底层 I/O 是否支持定位
    info.can_seek_ = (context->ctx_flags & AVFMTCTX_UNSEEKABLE) == 0 && context->pb != nullptr &&
                     (context->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;

    // 2. 复制流级参数
    VideoStreamInfo video_info;
    video_info.stream_index_ = video_stream->index;
    video_info.frame_rate_ = ToRational(SelectFrameRate(context, video_stream));
    video_info.time_base_ = ToRational(video_stream->time_base);

    if (video_stream->duration != AV_NOPTS_VALUE && video_stream->duration >= 0) {
        video_info.duration_us_ = TimestampToMicroseconds(video_stream->duration, video_stream->time_base);
    }

    // 3. 复制编解码器参数
    const AVCodecParameters& parameters = *video_stream->codecpar;
    video_info.codec_id_ = static_cast<int>(parameters.codec_id);
    video_info.width_ = parameters.width;
    video_info.height_ = parameters.height;
    video_info.pixel_format_ = ConvertVideoPixelFormat(parameters.format);

    // 4. 深拷贝 extradata 关键头（如 SPS/PPS）
    if (parameters.extradata != nullptr && parameters.extradata_size > 0) {
        video_info.extradata_.assign(parameters.extradata, parameters.extradata + parameters.extradata_size);
    }

    info.video_stream_ = std::move(video_info);
    return info;
}

/**
 * @brief 清除 AVIOContext 上一轮残留的 EOF 与错误标志
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
 * @brief 检查是否已发起永久停止
 */
bool DemuxerInterruptHandle::IsStopRequested() const noexcept {
    return state_ && state_->stop_requested_.load(std::memory_order_acquire);
}

/**
 * @brief 发起 Seek 打断请求
 */
void DemuxerInterruptHandle::RequestSeekInterrupt() noexcept {
    if (state_) {
        state_->seek_cancel_generation_.fetch_add(1, std::memory_order_release);
    }
}

/**
 * @brief 在执行下一次阻塞 I/O 前锁定当前的 Seek 序号基准
 */
void DemuxerInterruptHandle::PrepareNextIoForSeekInterrupt() noexcept {
    if (state_) {
        const std::uint64_t generation = state_->seek_cancel_generation_.load(std::memory_order_acquire);
        state_->armed_generation_.store(generation, std::memory_order_release);
    }
}

/**
 * @brief 获取当前阻塞打断的具体诱因（Stop 优先于 Seek）
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

    return requested == armed ? DemuxerInterruptReason::kNone : DemuxerInterruptReason::kSeek;
}

/**
 * @brief 私有构造函数：接收已打开的上下文并移交资源所有权
 */
FfmpegDemuxer::FfmpegDemuxer(AVFormatContext* av_format_ctx,
                             MediaInfo media_info,
                             int video_stream_index,
                             DemuxerInterruptHandle interrupt,
                             std::int64_t timeline_origin_ts) noexcept
    : av_format_ctx_(av_format_ctx),
      media_info_(std::move(media_info)),
      video_stream_index_(video_stream_index),
      interrupt_(std::move(interrupt)),
      timeline_origin_ts_(timeline_origin_ts) {}

/**
 * @brief 析构函数：释放底层 AVFormatContext
 */
FfmpegDemuxer::~FfmpegDemuxer() {
    LOG_INFO("FfmpegDemuxer destroyed, closing input");
    if (av_format_ctx_ != nullptr) {
        avformat_close_input(&av_format_ctx_);
    }
}

/**
 * @brief 注入给 FFmpeg AVIOInterruptCB 的 C 语言回调
 */
int FfmpegDemuxer::InterruptCallback(void* opaque) noexcept {
    const auto* state = static_cast<const DemuxerInterruptHandle::State*>(opaque);
    if (state == nullptr) {
        return kContinueFfmpegIo;
    }

    if (state->stop_requested_.load(std::memory_order_acquire)) {
        return kAbortFfmpegIo;
    }

    const bool new_seek_requested =
        state->seek_cancel_generation_.load(std::memory_order_acquire) !=
        state->armed_generation_.load(std::memory_order_acquire);

    return new_seek_requested ? kAbortFfmpegIo : kContinueFfmpegIo;
}

/**
 * @brief 静态工厂：异步/同步打开输入源、探测流信息并创建 Demuxer
 */
FfmpegDemuxerOpenResult FfmpegDemuxer::Open(const std::string& uri, DemuxerInterruptHandle interrupt) {
    FfmpegDemuxerOpenResult result;

    if (uri.empty()) {
        LOG_WARN("FfmpegDemuxer::Open rejected an empty URI");
        result.error_ = MakeFfmpegError(kOpenInputOperation, AVERROR(EINVAL));
        return result;
    }

    if (interrupt.IsStopRequested()) {
        LOG_DEBUG("FfmpegDemuxer::Open cancelled before opening '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    AVFormatContext* format_context = avformat_alloc_context();
    if (format_context == nullptr) {
        LOG_ERROR("avformat_alloc_context failed while opening '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenInputOperation, AVERROR(ENOMEM));
        return result;
    }

    // 绑定中断回调以支持外部打断耗时网络连接
    format_context->interrupt_callback =
        AVIOInterruptCB{&FfmpegDemuxer::InterruptCallback, interrupt.state_.get()};

    const int open_result = avformat_open_input(&format_context, uri.c_str(), nullptr, nullptr);

    if (interrupt.IsStopRequested()) {
        if (format_context != nullptr) {
            avformat_close_input(&format_context);
        }
        LOG_DEBUG("FfmpegDemuxer::Open cancelled while opening '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    if (open_result < 0) {
        if (format_context != nullptr) {
            avformat_close_input(&format_context);
        }
        LOG_ERROR("avformat_open_input failed for '{}', code {}", uri, open_result);
        result.error_ = MakeFfmpegError(kOpenInputOperation, open_result);
        return result;
    }

    // 交由 RAII 对象临时持有，后续提前退出均会自动安全触发 avformat_close_input
    FormatContextOwner context_owner(format_context);

    if (interrupt.IsStopRequested()) {
        LOG_DEBUG("FfmpegDemuxer::Open cancelled before reading stream information for '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    // 探测解封装流信息
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

    // 筛选最佳视频流
    const int video_stream_index =
        av_find_best_stream(context_owner.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (video_stream_index < 0) {
        LOG_ERROR("av_find_best_stream found no video stream in '{}', code {}", uri, video_stream_index);
        result.error_ = MakeFfmpegError(kFindVideoStreamOperation, video_stream_index);
        return result;
    }

    AVStream* video_stream = context_owner->streams[video_stream_index];
    if (video_stream == nullptr || video_stream->codecpar == nullptr) {
        LOG_ERROR("Selected video stream {} in '{}' has no codec parameters", video_stream_index, uri);
        result.error_ = MakeFfmpegError(kFindVideoStreamOperation, AVERROR_INVALIDDATA);
        return result;
    }

    // 计算基准时间戳偏移量
    const std::optional<std::int64_t> timeline_origin_ts =
        DetermineTimeLineOriginTs(context_owner.get(), video_stream);

    if (!timeline_origin_ts.has_value()) {
        LOG_ERROR("Cannot determine playback timeline origin for video stream {} in '{}'", video_stream_index, uri);
        result.error_ = MakeFfmpegError(kTimelineOriginOperation, AVERROR(ERANGE));
        return result;
    }

    // 拷贝编解码器配置参数供外部 Decoder 初始化使用
    AVCodecParameters* video_parameters = avcodec_parameters_alloc();
    if (video_parameters == nullptr) {
        LOG_ERROR("avcodec_parameters_alloc failed while opening '{}'", uri);
        result.error_ = MakeFfmpegError(kCloneCodecParametersOperation, AVERROR(ENOMEM));
        return result;
    }

    VideoCodecParameters parameters_owner(video_parameters);
    const int copy_result = avcodec_parameters_copy(video_parameters, video_stream->codecpar);

    if (copy_result < 0) {
        LOG_ERROR("avcodec_parameters_copy failed for '{}', code {}", uri, copy_result);
        result.error_ = MakeFfmpegError(kCloneCodecParametersOperation, copy_result);
        return result;
    }

    // 组装业务层解耦元数据
    MediaInfo media_info = BuildMediaInfo(context_owner.get(), video_stream);
    if (!media_info.IsValid()) {
        LOG_ERROR("Failed to build valid media information for video stream {} in '{}'", video_stream_index, uri);
        result.error_ = MakeFfmpegError(kFindVideoStreamOperation, AVERROR_INVALIDDATA);
        return result;
    }

    if (interrupt.IsStopRequested()) {
        LOG_DEBUG("FfmpegDemuxer::Open cancelled after reading stream information for '{}'", uri);
        result.error_ = MakeFfmpegError(kOpenCancelledOperation, AVERROR_EXIT);
        return result;
    }

    // 实例化 FfmpegDemuxer 托管句柄
    try {
        result.demuxer_.reset(new FfmpegDemuxer(context_owner.get(),
                                                std::move(media_info),
                                                video_stream_index,
                                                std::move(interrupt),
                                                *timeline_origin_ts));
    } catch (const std::bad_alloc&) {
        LOG_ERROR("Failed to create FfmpegDemuxer for '{}' due to insufficient memory", uri);
        result.error_ = MakeFfmpegError(kOpenInputOperation, AVERROR(ENOMEM));
        return result;
    }

    // 移交所有权给 FfmpegDemuxer，放弃临时 RAII 对象的析构管理
    (void)context_owner.release();
    result.video_codec_parameters_ = std::move(parameters_owner);

    const MediaInfo& opened_media_info = result.demuxer_->GetMediaInfo();
    const VideoStreamInfo& video_stream_info = opened_media_info.video_stream_.value();

    LOG_INFO("Demuxer opened successfully, video stream {}: {}x{}, codec {}, pixel format {}, {} fps",
             video_stream_index,
             video_stream_info.width_,
             video_stream_info.height_,
             avcodec_get_name(static_cast<AVCodecID>(opened_media_info.video_stream_->codec_id_)),
             VideoPixelFormatToString(video_stream_info.pixel_format_),
             video_stream_info.frame_rate_.ToDouble());

    return result;
}

/**
 * @brief 从解复用器中读取一个压缩数据包
 */
FfmpegReadResult FfmpegDemuxer::Read(Packet& packet) {
    if (av_format_ctx_ == nullptr || !packet.IsValid() || video_stream_index_ < 0 ||
        static_cast<unsigned int>(video_stream_index_) >= av_format_ctx_->nb_streams) {
        LOG_ERROR(
            "FfmpegDemuxer::Read rejected: invalid state "
            "(av_format_ctx={}, packet_valid={}, video_stream={}, stream_count={})",
            av_format_ctx_ != nullptr,
            packet.IsValid(),
            video_stream_index_,
            av_format_ctx_ != nullptr ? av_format_ctx_->nb_streams : 0U);

        return {FfmpegReadStatus::kError, MakeFfmpegError(kReadFrameOperation, AVERROR(EINVAL))};
    }

    AVPacket* raw_packet = packet.GetAVPacket();
    av_packet_unref(raw_packet);
    packet.ResetDemuxMetadata();

    const AVStream* video_stream = av_format_ctx_->streams[video_stream_index_];
    if (video_stream == nullptr || !IsValidTimeBase(video_stream->time_base)) {
        LOG_ERROR("FfmpegDemuxer::Read cannot use video stream {}: stream is null or time_base is invalid",
                  video_stream_index_);
        return {FfmpegReadStatus::kError, MakeFfmpegError(kReadFrameOperation, AVERROR_INVALIDDATA)};
    }

    // 优先检查读取前的打断状态
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

    // 执行底层解包读取
    const int read_result = av_read_frame(av_format_ctx_, raw_packet);

    // 校验阻塞读取返回后的中断状态
    const DemuxerInterruptReason after_read = interrupt_.GetInterruptReason();
    if (after_read == DemuxerInterruptReason::kStop) {
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kStopInterrupted, {}};
    }
    if (after_read == DemuxerInterruptReason::kSeek) {
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kSeekInterrupted, {}};
    }

    if (read_result == AVERROR_EOF) {
        eof_reached_ = true;
        return {FfmpegReadStatus::kEndOfStream, {}};
    }

    if (read_result < 0) {
        LOG_ERROR("av_read_frame failed for video stream {}, code {}", video_stream_index_, read_result);
        return {FfmpegReadStatus::kError, MakeFfmpegError(kReadFrameOperation, read_result)};
    }

    // 忽略音频等非目标数据流
    if (raw_packet->stream_index != video_stream_index_) {
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kSkipped, {}};
    }

    // 将底层 AVPacket 的原始 PTS/DTS 平移归一化到播放器 0 基准时间轴
    const auto normalize_timestamp = [this](std::int64_t value, std::int64_t& normalized) noexcept -> bool {
        if (value == AV_NOPTS_VALUE) {
            normalized = AV_NOPTS_VALUE;
            return true;
        }

        // 防御减法向下溢出
        if (timeline_origin_ts_ > 0 && value < std::numeric_limits<std::int64_t>::min() + timeline_origin_ts_) {
            return false;
        }
        // 防御减负数（相当于加法）向上溢出
        if (timeline_origin_ts_ < 0 && value > std::numeric_limits<std::int64_t>::max() + timeline_origin_ts_) {
            return false;
        }

        const std::int64_t shifted = value - timeline_origin_ts_;
        if (shifted == AV_NOPTS_VALUE) {
            return false;
        }

        normalized = shifted;
        return true;
    };

    std::int64_t normalized_pts = AV_NOPTS_VALUE;
    std::int64_t normalized_dts = AV_NOPTS_VALUE;

    if (!normalize_timestamp(raw_packet->pts, normalized_pts) ||
        !normalize_timestamp(raw_packet->dts, normalized_dts)) {
        LOG_ERROR("Packet timestamp overflow while normalizing video stream {}", video_stream_index_);
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kError, MakeFfmpegError(kNormalizeTimestampOperation, AVERROR(ERANGE))};
    }

    raw_packet->pts = normalized_pts;
    raw_packet->dts = normalized_dts;

    const std::optional<std::int64_t> packet_duration_us = raw_packet->duration > 0
        ? TimestampToMicroseconds(raw_packet->duration, video_stream->time_base)
        : std::nullopt;

    packet.SetDemuxMetadata(Packet::DemuxMetadata{
        raw_packet->stream_index,
        raw_packet->size > 0 ? static_cast<std::size_t>(raw_packet->size) : 0,
        (raw_packet->flags & AV_PKT_FLAG_KEY) != 0,
        TimestampToMicroseconds(raw_packet->pts, video_stream->time_base),
        TimestampToMicroseconds(raw_packet->dts, video_stream->time_base),
        packet_duration_us});

    return {FfmpegReadStatus::kPacket, {}};
}

/**
 * @brief 将输入定位至指定的零基播放时间
 */
FfmpegSeekResult FfmpegDemuxer::Seek(std::int64_t target_us) {
    if (av_format_ctx_ == nullptr || video_stream_index_ < 0 ||
        static_cast<unsigned int>(video_stream_index_) >= av_format_ctx_->nb_streams) {
        LOG_ERROR(
            "FfmpegDemuxer::Seek to {} us rejected: invalid state (av_format_ctx = {}, video_stream_index = {}, "
            "stream_count = {})",
            target_us,
            av_format_ctx_ != nullptr,
            video_stream_index_,
            av_format_ctx_ != nullptr ? av_format_ctx_->nb_streams : 0U);

        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(EINVAL))};
    }

    if (target_us < 0) {
        LOG_ERROR("FfmpegDemuxer::Seek rejected negative target {} us", target_us);
        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(EINVAL))};
    }

    const DemuxerInterruptReason before_seek = interrupt_.GetInterruptReason();
    if (before_seek == DemuxerInterruptReason::kStop) {
        return {FfmpegSeekStatus::kStopInterrupted, std::nullopt, {}};
    }
    if (before_seek == DemuxerInterruptReason::kSeek) {
        return {FfmpegSeekStatus::kSeekInterrupted, std::nullopt, {}};
    }

    const AVStream* video_stream = av_format_ctx_->streams[video_stream_index_];
    if (video_stream == nullptr || !IsValidTimeBase(video_stream->time_base)) {
        LOG_ERROR("FfmpegDemuxer::Seek cannot use video stream {}: stream is null or time_base is invalid",
                  video_stream_index_);
        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR_INVALIDDATA)};
    }

    // 换算相对于零基时间轴的目标刻度偏移
    const std::int64_t target_offset_ts = av_rescale_q(target_us, AV_TIME_BASE_Q, video_stream->time_base);
    if (target_offset_ts == AV_NOPTS_VALUE || target_offset_ts < 0) {
        LOG_ERROR("FfmpegDemuxer::Seek cannot convert target {} us to stream timestamp", target_us);
        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(EINVAL))};
    }

    // 防御加法向上溢出
    if (timeline_origin_ts_ > std::numeric_limits<std::int64_t>::max() - target_offset_ts) {
        LOG_ERROR("FfmpegDemuxer::Seek target {} us is outside timestamp range", target_us);
        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(ERANGE))};
    }

    // 换算为传给底层 FFmpeg 的原始绝对时间戳
    const std::int64_t target_ts = timeline_origin_ts_ + target_offset_ts;

    // 复位上一轮读取遗留的 EOF 与错误状态
    ResetAvioReadStateForSeek(av_format_ctx_);

    // 执行定位，AVSEEK_FLAG_BACKWARD 确保回退到目标时间点前最近的关键帧
    const int seek_result =
        av_seek_frame(av_format_ctx_, video_stream_index_, target_ts, AVSEEK_FLAG_BACKWARD);

    const DemuxerInterruptReason after_seek = interrupt_.GetInterruptReason();
    if (after_seek == DemuxerInterruptReason::kStop) {
        return {FfmpegSeekStatus::kStopInterrupted, std::nullopt, {}};
    }
    if (after_seek == DemuxerInterruptReason::kSeek) {
        return {FfmpegSeekStatus::kSeekInterrupted, std::nullopt, {}};
    }

    if (seek_result < 0) {
        LOG_ERROR(
            "av_seek_frame failed for video stream {}; target {} us, raw stream timestamp {}, code {}",
            video_stream_index_,
            target_us,
            target_ts,
            seek_result);
        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, seek_result)};
    }

    // 定位成功后清除流结束标志
    eof_reached_ = false;

    LOG_INFO("Demuxer accepted seek for video stream {}; target {} us, raw stream timestamp {}",
             video_stream_index_,
             target_us,
             target_ts);

    return {FfmpegSeekStatus::kSucceeded, std::nullopt, {}};
}

}  // namespace mediacore
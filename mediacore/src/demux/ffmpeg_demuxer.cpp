/**
 * @file ffmpeg_demuxer.cpp
 * @brief 基于 FFmpeg libavformat 实现的媒体解封装核心逻辑实现
 * @details
 * 负责输入媒体源的生命周期管理、协议探测、音视频流筛选、数据包解包及精准定位：
 * 1. 协作式异步中断（Cooperative Interruption）：
 *    将 DemuxerInterruptHandle 绑入 FFmpeg 底层的 AVIOInterruptCB 回调体系。
 *    支持两级打断机制：永久停止请求（Stop）与基于序号世代（Generation）的快进跳转打断（Seek），
 *    确保即使底层在执行阻塞的网络握手或长 I/O 时，也能毫秒级安全退出；
 * 2. 播放时间轴零基归一化（Timeline Origin Normalization）：
 *    消除容器层（如 MPEG-TS、HLS、FLV 直播）原生非零 start_time 引起的初始时间戳漂移，
 *    所有向上层吐出的 AVPacket 均被归一化为以 0 为起点的微秒绝对时间轴；
 * 3. RAII 异常安全保证：
 *    全链路采用 FormatContextCloser/FormatContextOwner 托管底层 C 结构体，
 *    在探测失败、中断退出或异常抛出时自动安全触发 avformat_close_input，杜绝内存泄漏；
 * 4. 健壮的定点整数溢出防御：
 *    在进行时间基（Time Base）换算与时间戳平移时，显式执行 64 位有符号极值检测，防范时间戳翻转溢出。
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
    /// 永久停止请求标志位；一旦置为 true 即不可逆，促使所有正在阻塞或后续的 FFmpeg 调用快速退出
    std::atomic<bool> stop_requested_{false};

    /// 全局累计的 Seek 请求序号；每次外部触发新 Seek 时原子递增
    std::atomic<std::uint64_t> seek_cancel_generation_{0};

    /// 工作线程在当前 I/O 操作启动前记录并武装的 Seek 序号基准
    /// 若在 I/O 阻塞期间 seek_cancel_generation_ 发生了改变，说明有更新的 Seek 发生，当前 I/O 应立即中断
    std::atomic<std::uint64_t> armed_generation_{0};
};

namespace {

// ============================================================================
// 常量标识定义：用于日志与错误码跟踪上报
// ============================================================================
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

// FFmpeg AVIOInterruptCB 约定的标准返回值：0 继续执行，1 立即打断当前阻塞 I/O
constexpr int kContinueFfmpegIo = 0;
constexpr int kAbortFfmpegIo = 1;

/**
 * @struct FormatContextCloser
 * @brief 自定义 RAII 释放仿函数：安全关闭 AVFormatContext
 * @note avformat_close_input 会在释放内部资源后将传入的指针置空（双重指针语义）。
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
 * @brief 将 FFmpeg 底层负数错误码转换为项目统一的 MediaError 异常对象
 * @param operation 触发错误的 FFmpeg 函数或操作名
 * @param code FFmpeg 返回的负数错误码（如 AVERROR(EINVAL)、AVERROR_EOF 等）
 * @return 构造完成的标准 MediaError
 */
MediaError MakeFfmpegError(const char* operation, int code) {
    return ffmpeg_detail::MakeFfmpegMediaError(kModuleName, code, operation);
}

/**
 * @brief 校验 FFmpeg 时间基（AVRational）的数学有效性
 * @param time_base 待校验的时间基结构体
 * @return 若分子分母均为正数返回 true，否则返回 false
 */
bool IsValidTimeBase(AVRational time_base) noexcept {
    return time_base.den > 0 && time_base.num > 0;
}

/**
 * @brief 将基于流原生时间基的时间戳精确换算为微秒（us）
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
 * @brief 计算媒体播放时间 0 点在目标视频流原生时间基下对应的起始时间戳
 * @param av_format_ctx 解封装全局上下文
 * @param video_stream 目标视频流指针
 * @return 转换后的起始原点刻度；若上下文/流非法则返回 std::nullopt
 * @details
 * 消除媒体源非零起始时间的偏移（例如 TS 切片广播流、RTMP 直播流）：
 * 1. 优先使用 AVFormatContext::start_time 进行全局跨流时间轴对齐；
 * 2. 若容器未提供全局 start_time，降级使用视频流自身的 AVStream::start_time；
 * 3. 若均未提供，默认归零为 0。
 */
std::optional<std::int64_t> DetermineTimeLineOriginTs(const AVFormatContext* av_format_ctx,
                                                      const AVStream* video_stream) noexcept {
    if (av_format_ctx == nullptr || video_stream == nullptr || !IsValidTimeBase(video_stream->time_base)) {
        return std::nullopt;
    }

    // 1. 优先采用容器级全局 start_time 对齐
    if (av_format_ctx->start_time != AV_NOPTS_VALUE) {
        const std::int64_t origin_ts = av_rescale_q(av_format_ctx->start_time, AV_TIME_BASE_Q, video_stream->time_base);
        if (origin_ts == AV_NOPTS_VALUE) {
            return std::nullopt;
        }
        return origin_ts;
    }

    // 2. 降级采用视频流自身 start_time
    if (video_stream->start_time != AV_NOPTS_VALUE) {
        return video_stream->start_time;
    }

    // 3. 兜底为 0
    return std::int64_t{0};
}

/**
 * @brief 将 FFmpeg 的 AVRational 转换为项目自定义的高层 Rational 结构体
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
 * @brief 从底层的 FFmpeg 结构体中提取并深拷贝业务层所需的独立元数据
 * @param context 已成功探测流信息的 AVFormatContext
 * @param video_stream 目标视频流句柄
 * @return 构造完成且与 FFmpeg 原生生命周期解耦的 MediaInfo 结构体
 */
MediaInfo BuildMediaInfo(AVFormatContext* context, AVStream* video_stream) {
    MediaInfo info;

    // 1. 提取封装格式名称与总时长
    if (context->iformat != nullptr && context->iformat->name != nullptr) {
        info.container_name_ = context->iformat->name;
    }

    if (context->duration != AV_NOPTS_VALUE && context->duration >= 0) {
        info.duration_us_ = context->duration;
    }

    // 探测底层协议是否支持物理定位（文件支持 Seek，实时 RTSP/RTMP 通常不可 Seek）
    info.can_seek_ = (context->ctx_flags & AVFMTCTX_UNSEEKABLE) == 0 && context->pb != nullptr &&
        (context->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;

    // 2. 提取视频流基本属性
    VideoStreamInfo video_info;
    video_info.stream_index_ = video_stream->index;
    video_info.frame_rate_ = ToRational(SelectFrameRate(context, video_stream));
    video_info.time_base_ = ToRational(video_stream->time_base);

    if (video_stream->duration != AV_NOPTS_VALUE && video_stream->duration >= 0) {
        video_info.duration_us_ = TimestampToMicroseconds(video_stream->duration, video_stream->time_base);
    }

    // 3. 提取编解码参数（分辨率、编码类型、像素格式）
    const AVCodecParameters& parameters = *video_stream->codecpar;
    video_info.codec_id_ = static_cast<int>(parameters.codec_id);
    video_info.width_ = parameters.width;
    video_info.height_ = parameters.height;
    video_info.pixel_format_ = ConvertVideoPixelFormat(parameters.format);

    // 4. 深拷贝关键配置头（如 H.264/H.265 的 SPS/PPS 头 extradata）
    if (parameters.extradata != nullptr && parameters.extradata_size > 0) {
        video_info.extradata_.assign(parameters.extradata, parameters.extradata + parameters.extradata_size);
    }

    info.video_stream_ = std::move(video_info);
    return info;
}

/**
 * @brief 在执行 Seek 定位前复位底层 AVIOContext 残留的错误与 EOF 标志
 * @param context 解复用上下文
 * @note 当文件读到末尾（触发 EOF）后用户再发起 Seek 时，
 *       若不显式清除 eof_reached，后续调用 av_read_frame 将直接返回 EOF 而拒绝读取新数据。
 */
void ResetAvioReadStateForSeek(AVFormatContext* context) noexcept {
    if (context == nullptr || context->pb == nullptr) {
        return;
    }

    context->pb->error = 0;
    context->pb->eof_reached = 0;
}

}  // namespace

// ============================================================================
// DemuxerInterruptHandle 中断句柄实现
// ============================================================================

/**
 * @brief 构造函数：在堆上创建共享中断状态
 */
DemuxerInterruptHandle::DemuxerInterruptHandle() : state_(std::make_shared<State>()) {}

/**
 * @brief 外部控制线程发起永久停止（Stop）请求
 */
void DemuxerInterruptHandle::RequestStop() noexcept {
    if (state_) {
        state_->stop_requested_.store(true, std::memory_order_release);
    }
}

/**
 * @brief 检查当前是否已被请求永久停止
 */
bool DemuxerInterruptHandle::IsStopRequested() const noexcept {
    return state_ && state_->stop_requested_.load(std::memory_order_acquire);
}

/**
 * @brief 外部控制线程发起打断当前阻塞 I/O 的 Seek 抢占请求
 */
void DemuxerInterruptHandle::RequestSeekInterrupt() noexcept {
    if (state_) {
        state_->seek_cancel_generation_.fetch_add(1, std::memory_order_release);
    }
}

/**
 * @brief 工作线程在发起下一次可能阻塞的 FFmpeg I/O 调用前锁定当前世代基准
 */
void DemuxerInterruptHandle::PrepareNextIoForSeekInterrupt() noexcept {
    if (state_) {
        const std::uint64_t generation = state_->seek_cancel_generation_.load(std::memory_order_acquire);
        state_->armed_generation_.store(generation, std::memory_order_release);
    }
}

/**
 * @brief 查询当前阻塞打断的具体诱因
 * @return DemuxerInterruptReason（Stop 优先级最高，其次为 Seek 打断）
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

// ============================================================================
// FfmpegDemuxer 核心实现
// ============================================================================

/**
 * @brief 私有构造函数：正式接管已完全打开的解封装上下文
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
 * @brief 静态工厂函数：异步/同步打开输入源、探测流信息并实例化 Demuxer
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

    // 4. 注入中断回调句柄
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

    // 10. 智能优选主视频轨
    const int video_stream_index = av_find_best_stream(context_owner.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (video_stream_index < 0) {
        LOG_ERROR("av_find_best_stream found no video stream in '{}', code {}", uri, video_stream_index);
        result.error_ = MakeFfmpegError(kFindVideoStreamOperation, video_stream_index);
        return result;
    }

    // 11. 校验视频流有效性及编解码参数指针
    AVStream* video_stream = context_owner->streams[video_stream_index];
    if (video_stream == nullptr || video_stream->codecpar == nullptr) {
        LOG_ERROR("Selected video stream {} in '{}' has no codec parameters", video_stream_index, uri);
        result.error_ = MakeFfmpegError(kFindVideoStreamOperation, AVERROR_INVALIDDATA);
        return result;
    }

    // 12. 确定播放时间轴原点时间戳（消除 TS / 直播流非零起始偏移）
    const std::optional<std::int64_t> timeline_origin_ts = DetermineTimeLineOriginTs(context_owner.get(), video_stream);

    if (!timeline_origin_ts.has_value()) {
        LOG_ERROR("Cannot determine playback timeline origin for video stream {} in '{}'", video_stream_index, uri);
        result.error_ = MakeFfmpegError(kTimelineOriginOperation, AVERROR(ERANGE));
        return result;
    }

    // 13. 深拷贝视频流编解码参数（彻底解耦 Demuxer 生命周期）
    auto parameters_owner = VideoCodecParameters::CopyFrom(video_stream->codecpar);

    // 14. 提取解耦的媒体元数据
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

    // 15. 实例化核心 Demuxer 实例
    try {
        result.demuxer_.reset(new FfmpegDemuxer(
            context_owner.get(), std::move(media_info), video_stream_index, std::move(interrupt), *timeline_origin_ts));
    } catch (const std::bad_alloc&) {
        LOG_ERROR("Failed to create FfmpegDemuxer for '{}' due to insufficient memory", uri);
        result.error_ = MakeFfmpegError(kOpenInputOperation, AVERROR(ENOMEM));
        return result;
    }

    // 16. 所有权顺利移交：解除临时 RAII 对象的析构职责
    static_cast<void>(context_owner.release());

    // 17. 转移编解码配置给外部调用方
    result.video_codec_parameters_ = std::move(parameters_owner);

    // 18. 打印就绪日志
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
 * @brief 从解复用器中读取一个压缩数据包并执行零基时间轴归一化
 * @param packet 输出的独立数据包容器引用
 * @return FfmpegReadResult 包含读取状态与错误详情
 * @details
 * 完整的读取流转逻辑：
 * 1. 状态有效性防卫性检查；
 * 2. 复位传入 packet 的历史状态并卸载旧 AVPacket；
 * 3. 前置中断检测（Stop 优先，Seek 次之）；
 * 4. 调用 av_read_frame 执行底层解包读取；
 * 5. 后置中断检测；
 * 6. EOF 与非目标流（音频流等）过滤丢弃；
 * 7. 时间轴归一化：将 PTS/DTS 减去 timeline_origin_ts_ 并防范溢出；
 * 8. 填充元数据（PTS/DTS 微秒换算、关键帧标志、字节大小）并返回。
 */
FfmpegReadResult FfmpegDemuxer::Read(Packet& packet) {
    // 1. 内部状态合法性检查
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

    // 2. 复位输出 Packet，确保其持有干净的原生 AVPacket 缓冲
    AVPacket* raw_packet = packet.GetAVPacket();
    av_packet_unref(raw_packet);
    packet.ResetDemuxMetadata();

    const AVStream* video_stream = av_format_ctx_->streams[video_stream_index_];
    if (video_stream == nullptr || !IsValidTimeBase(video_stream->time_base)) {
        LOG_ERROR("FfmpegDemuxer::Read cannot use video stream {}: stream is null or time_base is invalid",
                  video_stream_index_);
        return {FfmpegReadStatus::kError, MakeFfmpegError(kReadFrameOperation, AVERROR_INVALIDDATA)};
    }

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

    // 8. 过滤非目标视频轨（例如当前暂未对接的音频包或字幕包）
    if (raw_packet->stream_index != video_stream_index_) {
        av_packet_unref(raw_packet);
        return {FfmpegReadStatus::kSkipped, {}};
    }

    // 9. 将底层 AVPacket 原始时间戳平移至以 0 为起点的播放器统一时间轴
    const auto normalize_timestamp = [this](std::int64_t value, std::int64_t& normalized) noexcept -> bool {
        if (value == AV_NOPTS_VALUE) {
            normalized = AV_NOPTS_VALUE;
            return true;
        }

        // 防范减法向下溢出：value - origin < MIN
        if (timeline_origin_ts_ > 0 && value < std::numeric_limits<std::int64_t>::min() + timeline_origin_ts_) {
            return false;
        }
        // 防范减负数（相当于加法）向上溢出：value - origin > MAX
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

    // 10. 计算微秒级别的持续时长与绝对呈现时间戳
    const std::optional<std::int64_t> packet_duration_us = (raw_packet->duration > 0)
        ? TimestampToMicroseconds(raw_packet->duration, video_stream->time_base)
        : std::nullopt;

    // 11. 将解析好的元数据注入高层 Packet 结构体
    packet.SetDemuxMetadata(Packet::DemuxMetadata{raw_packet->stream_index,
                                                  raw_packet->size > 0 ? static_cast<std::size_t>(raw_packet->size) : 0,
                                                  (raw_packet->flags & AV_PKT_FLAG_KEY) != 0,
                                                  TimestampToMicroseconds(raw_packet->pts, video_stream->time_base),
                                                  TimestampToMicroseconds(raw_packet->dts, video_stream->time_base),
                                                  packet_duration_us});

    return {FfmpegReadStatus::kPacket, {}};
}

/**
 * @brief 将输入定位至指定的零基播放微秒时间
 * @param target_us 基于 0 原点的目标微秒位置（必须 >= 0）
 * @return FfmpegSeekResult 包含定位状态与错误信息
 * @details
 * 定位核心流程：
 * 1. 校验输入状态与 target_us 非负性；
 * 2. 检查前置打断；
 * 3. 将零基 target_us 换算为视频流时间基刻度；
 * 4. 叠加 timeline_origin_ts_ 换算回底层原生绝对时间戳（防范加法溢出）；
 * 5. 调用 ResetAvioReadStateForSeek 复位 EOF 标记；
 * 6. 执行 av_seek_frame（搭配 AVSEEK_FLAG_BACKWARD 向前寻找关键帧，保障解码器能正确建系解码）；
 * 7. 后置打断检查与状态收敛（复位 eof_reached_ 标志）。
 */
FfmpegSeekResult FfmpegDemuxer::Seek(std::int64_t target_us) {
    // 1. 状态与参数有效性校验
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

    // 2. 定位前中断检测
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

    // 3. 将零基目标微秒时间戳折算为视频流原生时间基刻度偏移量
    const std::int64_t target_offset_ts = av_rescale_q(target_us, AV_TIME_BASE_Q, video_stream->time_base);
    if (target_offset_ts == AV_NOPTS_VALUE || target_offset_ts < 0) {
        LOG_ERROR("FfmpegDemuxer::Seek cannot convert target {} us to stream timestamp", target_us);
        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(EINVAL))};
    }

    // 4. 防范加法向上溢出：origin + offset > MAX
    if (timeline_origin_ts_ > std::numeric_limits<std::int64_t>::max() - target_offset_ts) {
        LOG_ERROR("FfmpegDemuxer::Seek target {} us is outside timestamp range", target_us);
        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, AVERROR(ERANGE))};
    }

    // 5. 还原为传给底层驱动的原始时间戳
    const std::int64_t target_ts = timeline_origin_ts_ + target_offset_ts;

    // 6. 清除上一轮读取残留的 EOF 与错误标志
    ResetAvioReadStateForSeek(av_format_ctx_);

    // 7. 调用 FFmpeg 执行物理定位
    //    AVSEEK_FLAG_BACKWARD：确保定位到 target_ts 之前距离最近的关键帧（Keyframe/I-Frame），
    //    保证解码器从该处解压时能完整恢复后续画面
    const int seek_result = av_seek_frame(av_format_ctx_, video_stream_index_, target_ts, AVSEEK_FLAG_BACKWARD);

    // 8. 校验定位返回后的中断状态
    const DemuxerInterruptReason after_seek = interrupt_.GetInterruptReason();
    if (after_seek == DemuxerInterruptReason::kStop) {
        return {FfmpegSeekStatus::kStopInterrupted, std::nullopt, {}};
    }
    if (after_seek == DemuxerInterruptReason::kSeek) {
        return {FfmpegSeekStatus::kSeekInterrupted, std::nullopt, {}};
    }

    // 9. 处理底层定位错误
    if (seek_result < 0) {
        LOG_ERROR("av_seek_frame failed for video stream {}; target {} us, raw stream timestamp {}, code {}",
                  video_stream_index_,
                  target_us,
                  target_ts,
                  seek_result);
        return {FfmpegSeekStatus::kError, std::nullopt, MakeFfmpegError(kSeekFrameOperation, seek_result)};
    }

    // 10. 定位成功，重置流结束状态位
    eof_reached_ = false;

    LOG_INFO("Demuxer accepted seek for video stream {}; target {} us, raw stream timestamp {}",
             video_stream_index_,
             target_us,
             target_ts);

    return {FfmpegSeekStatus::kSucceeded, std::nullopt, {}};
}

}  // namespace mediacore
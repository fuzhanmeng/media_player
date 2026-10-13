/**
 * @file ffmpeg_video_decoder.cpp
 * @brief 基于 FFmpeg libavcodec 与 libswscale 的底层视频解码器实现
 * @details
 * 负责执行压缩数据包解码与目标色彩空间转码：
 * 1. 状态机驱动：遵循 avcodec_send_packet() 与 avcodec_receive_frame() 的异步流转规则；
 * 2. 协作排空与重置：支持 nullptr packet 进入 Drain 流程，以及 avcodec_flush_buffers() 的零残留 Flush；
 * 3. 内存解耦与格式对齐：使用 sws_getCachedContext() 弹性管理色彩空间上下文，将解码后的任意格式
 *    转码为项目标准 YUV420P VideoFrame，彻底解除对底层解码器复用缓冲区的生命周期依赖。
 */

#include "video_decoder/ffmpeg_video_decoder.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "mediacore/logger.h"
#include "utils/ffmpeg_error.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_par.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace mediacore {

namespace {

constexpr const char* kFindDecoderOperation = "avcodec_find_decoder(video)";
constexpr const char* kAllocateContextOperation = "avcodec_alloc_context3(video)";
constexpr const char* kCopyParametersOperation = "avcodec_parameters_to_context(video)";
constexpr const char* kOpenDecoderOperation = "avcodec_open2(video)";
constexpr const char* kAllocateFrameOperation = "av_frame_alloc()";
constexpr const char* kSendPacketOperation = "avcodec_send_packet(video)";
constexpr const char* kReceiveFrameOperation = "avcodec_receive_frame(video)";
constexpr const char* kValidateFrameSizeOperation = "av_image_get_buffer_size(yuv420p)";
constexpr const char* kConvertFrameOperation = "sws_scale(video->yuv420p)";
constexpr const char* kModuleName = "FfmpegVideoDecoder";

/**
 * @brief 将 FFmpeg 操作失败信息与错误码封装为项目统一的 MediaError
 * @param operation 触发错误的 FFmpeg 函数或操作名称
 * @param code FFmpeg 负数错误码
 * @return 归属于 kFfmpeg 类型的结构化错误对象
 */
MediaError MakeDecoderError(const char* operation, int code) {
    return ffmpeg_detail::MakeFfmpegMediaError(kModuleName, code, operation);
}

/**
 * @brief 将项目自定义的 Rational 安全转换为 FFmpeg 的 AVRational
 * @param value 项目自定义的时间基分数
 * @return 若分数合法且未超出 int 上限则返回 AVRational，否则返回 std::nullopt
 */
std::optional<AVRational> ToAVRational(const Rational& value) noexcept {
    constexpr auto kIntMax = static_cast<std::uint64_t>(std::numeric_limits<int>::max());

    if (!value.IsValid() || value.numerator_ > kIntMax || value.denominator_ > kIntMax) {
        return std::nullopt;
    }

    return AVRational{static_cast<int>(value.numerator_), static_cast<int>(value.denominator_)};
}

/**
 * @brief 将基于流时间基的视频帧时间戳换算为微秒（us）
 * @param value 原始时间戳刻度（AVFrame::best_effort_timestamp 或 pts）
 * @param time_base 视频流原生时间基
 * @return 换算后的微秒时间戳；若时间戳非法或换算失败则返回 std::nullopt
 */
std::optional<std::int64_t> TimestampToMicroseconds(std::int64_t value, const Rational& time_base) {
    if (value == AV_NOPTS_VALUE) {
        return std::nullopt;
    }

    const std::optional<AVRational> source_time_base = ToAVRational(time_base);
    if (!source_time_base.has_value()) {
        return std::nullopt;
    }

    const std::int64_t converted = av_rescale_q(value, *source_time_base, AV_TIME_BASE_Q);
    return converted == AV_NOPTS_VALUE ? std::nullopt : std::optional<std::int64_t>(converted);
}

/**
 * @struct CodecContextCloser
 * @brief 自定义 RAII 删除器：释放 AVCodecContext
 */
struct CodecContextCloser final {
    void operator()(AVCodecContext* context) const noexcept {
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }
};

/**
 * @struct FrameCloser
 * @brief 自定义 RAII 删除器：释放复用 AVFrame
 */
struct FrameCloser final {
    void operator()(AVFrame* frame) const noexcept {
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
    }
};

using CodecContextOwner = std::unique_ptr<AVCodecContext, CodecContextCloser>;
using FrameOwner = std::unique_ptr<AVFrame, FrameCloser>;

}  // namespace

/**
 * @brief 私有构造函数：保存并独占管理已成功打开的 FFmpeg 资源
 */
FfmpegVideoDecoder::FfmpegVideoDecoder(AVCodecContext* codec_context,
                                       AVFrame* decoded_frame,
                                       Rational video_stream_time_base,
                                       std::string codec_name) noexcept
    : codec_context_(codec_context),
      decoded_frame_(decoded_frame),
      video_stream_time_base_(video_stream_time_base),
      codec_name_(std::move(codec_name)) {}

/**
 * @brief 析构函数：释放转码上下文、解码器上下文及持有的复用 AVFrame
 */
FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    LOG_INFO("FfmpegVideoDecoder destroyed, codec {}", codec_name_);

    if (sws_context_ != nullptr) {
        sws_freeContext(sws_context_);
        sws_context_ = nullptr;
    }
    if (decoded_frame_ != nullptr) {
        av_frame_free(&decoded_frame_);
    }
    if (codec_context_ != nullptr) {
        avcodec_free_context(&codec_context_);
    }
}

/**
 * @brief 静态工厂：根据流参数创建并初始化视频解码器
 * @param parameters 解封装获取的编解码器参数（所有权转移）
 * @param video_stream_time_base 视频流原生时间基
 * @return 解码器打开结果
 */
FfmpegVideoDecoderOpenResult FfmpegVideoDecoder::Open(VideoCodecParameters parameters,
                                                      Rational video_stream_time_base) {
    FfmpegVideoDecoderOpenResult result;

    const AVCodecParameters* raw_parameters = parameters.Get();
    if (raw_parameters == nullptr || raw_parameters->codec_type != AVMEDIA_TYPE_VIDEO) {
        LOG_ERROR("FfmpegVideoDecoder::Open requires valid video codec parameters");
        result.error_ = MakeDecoderError(kFindDecoderOperation, AVERROR(EINVAL));
        return result;
    }

    const std::optional<AVRational> packet_time_base = ToAVRational(video_stream_time_base);
    if (!packet_time_base.has_value()) {
        LOG_ERROR("FfmpegVideoDecoder::Open received an invalid or out-of-range stream time base {}/{}",
                  video_stream_time_base.numerator_,
                  video_stream_time_base.denominator_);
        result.error_ = MakeDecoderError(kCopyParametersOperation, AVERROR(EINVAL));
        return result;
    }

    LOG_INFO("Opening video decoder for codec id {}, time base {}/{}",
             static_cast<int>(raw_parameters->codec_id),
             video_stream_time_base.numerator_,
             video_stream_time_base.denominator_);

    // 查找适用的解码器实现
    const AVCodec* codec = avcodec_find_decoder(raw_parameters->codec_id);
    if (codec == nullptr) {
        LOG_ERROR("avcodec_find_decoder returned nullptr for codec_id {}", static_cast<int>(raw_parameters->codec_id));
        result.error_ = MakeDecoderError(kFindDecoderOperation, AVERROR_DECODER_NOT_FOUND);
        return result;
    }

    const std::string codec_name = codec->name != nullptr ? codec->name : "unknown";

    // 分配解码上下文
    CodecContextOwner context_owner(avcodec_alloc_context3(codec));
    if (!context_owner) {
        LOG_ERROR("avcodec_alloc_context3 failed for decoder '{}'", codec_name);
        result.error_ = MakeDecoderError(kAllocateContextOperation, AVERROR(ENOMEM));
        return result;
    }

    // 复制容器中的流参数至解码上下文
    const int parameters_result = avcodec_parameters_to_context(context_owner.get(), raw_parameters);
    if (parameters_result < 0) {
        LOG_ERROR("avcodec_parameters_to_context failed for decoder '{}', code {}", codec_name, parameters_result);
        result.error_ = MakeDecoderError(kCopyParametersOperation, parameters_result);
        return result;
    }

    // 设置输入 Packet 的时间基准
    context_owner->pkt_timebase = *packet_time_base;

    // 打开并初始化解码器
    const int open_result = avcodec_open2(context_owner.get(), codec, nullptr);
    if (open_result < 0) {
        LOG_ERROR("avcodec_open2 failed for decoder '{}', code {}", codec_name, open_result);
        result.error_ = MakeDecoderError(kOpenDecoderOperation, open_result);
        return result;
    }

    // 分配用于接收解码帧的复用 AVFrame
    FrameOwner frame_owner(av_frame_alloc());
    if (!frame_owner) {
        LOG_ERROR("av_frame_alloc failed while opening decoder '{}'", codec_name);
        result.error_ = MakeDecoderError(kAllocateFrameOperation, AVERROR(ENOMEM));
        return result;
    }

    try {
        result.video_decoder_.reset(
            new FfmpegVideoDecoder(context_owner.get(), frame_owner.get(), video_stream_time_base, codec_name));
    } catch (const std::bad_alloc&) {
        LOG_ERROR("Failed to create FfmpegVideoDecoder for '{}' due to insufficient memory", codec_name);
        result.error_ = MakeDecoderError(kAllocateContextOperation, AVERROR(ENOMEM));
        return result;
    }

    // 移交所有权给 FfmpegVideoDecoder 实例
    (void)context_owner.release();
    (void)frame_owner.release();

    LOG_INFO("Video decoder opened, codec {}",
             result.video_decoder_ ? result.video_decoder_->GetVideoCodecName() : "unknown");

    return result;
}

/**
 * @brief 向底层解码器提交压缩数据包
 */
FfmpegDecoderSendResult FfmpegVideoDecoder::SendPacket(const Packet& packet) {
    if (codec_context_ == nullptr || decoded_frame_ == nullptr) {
        LOG_ERROR(
            "Video Decoder '{}' cannot send packet to AVCodecContext because its internal FFmpeg objects are "
            "unavailable",
            codec_name_);
        return {FfmpegDecoderSendStatus::kError, MakeDecoderError(kSendPacketOperation, AVERROR(EINVAL))};
    }

    if (!packet.IsValid() || packet.GetAVPacket()->data == nullptr || packet.GetAVPacket()->size <= 0) {
        LOG_ERROR("Video Decoder '{}' received an invalid packet for epoch {}", codec_name_, packet.GetEpoch());
        return {FfmpegDecoderSendStatus::kError, MakeDecoderError(kSendPacketOperation, AVERROR(EINVAL))};
    }

    if (drain_started_ || drained_) {
        return {FfmpegDecoderSendStatus::kDrained, {}};
    }

    const int send_result = avcodec_send_packet(codec_context_, packet.GetAVPacket());

    if (send_result == 0) {
        return {FfmpegDecoderSendStatus::kAccepted, {}};
    }

    // EAGAIN：缓冲区已满，需先调用 ReceiveFrame() 腾出空间再重试
    if (send_result == AVERROR(EAGAIN)) {
        return {FfmpegDecoderSendStatus::kNeedReceive, {}};
    }

    // AVERROR_EOF：解码器已被排空，Flush 前不能再接收新包
    if (send_result == AVERROR_EOF) {
        drain_started_ = true;
        return {FfmpegDecoderSendStatus::kDrained, {}};
    }

    LOG_ERROR("avcodec_send_packet failed for decoder '{}', packet epoch {}, code {}",
              codec_name_,
              packet.GetEpoch(),
              send_result);

    return {FfmpegDecoderSendStatus::kError, MakeDecoderError(kSendPacketOperation, send_result)};
}

/**
 * @brief 发送 nullptr packet，令解码器进入 Drain 排空模式
 */
FfmpegDecoderSendResult FfmpegVideoDecoder::BeginDrain() {
    if (codec_context_ == nullptr || decoded_frame_ == nullptr) {
        LOG_ERROR("Video Decoder '{}' cannot begin drain because its internal FFmpeg objects are unavailable",
                  codec_name_);
        return {FfmpegDecoderSendStatus::kError, MakeDecoderError(kSendPacketOperation, AVERROR(EINVAL))};
    }

    if (drained_ || drain_started_) {
        return {FfmpegDecoderSendStatus::kAccepted, {}};
    }

    // 传入 nullptr packet 发送 EOF 信号，排空内部暂存的延迟帧（如 B 帧重排队列）
    const int send_result = avcodec_send_packet(codec_context_, nullptr);

    if (send_result == 0) {
        drain_started_ = true;
        LOG_DEBUG("Video Decoder '{}' started drain", codec_name_);
        return {FfmpegDecoderSendStatus::kAccepted, {}};
    }

    if (send_result == AVERROR(EAGAIN)) {
        return {FfmpegDecoderSendStatus::kNeedReceive, {}};
    }

    if (send_result == AVERROR_EOF) {
        drain_started_ = true;
        drained_ = true;
        return {FfmpegDecoderSendStatus::kDrained, {}};
    }

    LOG_ERROR("avcodec_send_packet(nullptr) failed while draining decoder '{}', code {}", codec_name_, send_result);
    return {FfmpegDecoderSendStatus::kError, MakeDecoderError(kSendPacketOperation, send_result)};
}

/**
 * @brief 彻底清空解码器内部缓存并复位状态（用于 Seek 跳转）
 */
void FfmpegVideoDecoder::Flush() noexcept {
    LOG_DEBUG("Video Decoder '{}' flushing cached frames", codec_name_);
    if (codec_context_ != nullptr) {
        avcodec_flush_buffers(codec_context_);
    }

    if (decoded_frame_ != nullptr) {
        av_frame_unref(decoded_frame_);
    }

    drain_started_ = false;
    drained_ = false;
}

/**
 * @brief 返回码流中声明的编码宽度
 */
int FfmpegVideoDecoder::GetCodedWidth() const noexcept {
    return codec_context_ != nullptr ? codec_context_->coded_width : 0;
}

/**
 * @brief 返回码流中声明的编码高度
 */
int FfmpegVideoDecoder::GetCodedHeight() const noexcept {
    return codec_context_ != nullptr ? codec_context_->coded_height : 0;
}

/**
 * @brief 从解码上下文中提取一帧画面并转码为项目标准 YUV420P VideoFrame
 * @param epoch 目标帧所属的数据流世代版本号
 */
FfmpegDecoderReceiveResult FfmpegVideoDecoder::ReceiveFrame(std::uint64_t epoch) {
    if (codec_context_ == nullptr || decoded_frame_ == nullptr) {
        LOG_ERROR("Video Decoder '{}' cannot receive frame for epoch {} because its internal FFmpeg objects are null",
                  codec_name_,
                  epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                MakeDecoderError(kReceiveFrameOperation, AVERROR(EINVAL))};
    }

    const int receive_result = avcodec_receive_frame(codec_context_, decoded_frame_);

    if (receive_result == 0) {
        // 成功提取到解码裸帧，执行色彩格式转码与内存深拷贝
        FfmpegDecoderReceiveResult result = ConvertCurrentFrame(epoch);
        av_frame_unref(decoded_frame_);
        return result;
    }

    if (receive_result == AVERROR(EAGAIN)) {
        return {FfmpegDecoderReceiveStatus::kNeedInput, std::nullopt, {}};
    }

    if (receive_result == AVERROR_EOF) {
        drained_ = true;
        LOG_DEBUG("Video Decoder '{}' drain completed for epoch {}", codec_name_, epoch);
        return {FfmpegDecoderReceiveStatus::kDrained, std::nullopt, {}};
    }

    LOG_ERROR("avcodec_receive_frame failed for decoder '{}', epoch {}, code {}", codec_name_, epoch, receive_result);
    return {FfmpegDecoderReceiveStatus::kError, std::nullopt, MakeDecoderError(kReceiveFrameOperation, receive_result)};
}

/**
 * @brief 将 decoded_frame_ 中的原始 AVFrame 转换为自定义的 YUV420P VideoFrame
 * @details
 * 1. 基础物理参数校验：确保宽高合法且格式有效；
 * 2. 检查 libswscale 对当前源像素格式（如 NV12、YUV422P、RGB 等）的转换支持度；
 * 3. 使用 av_image_get_buffer_size() 预计算目标尺寸内存需求，防御潜在溢出；
 * 4. 通过 sws_getCachedContext() 弹性复用或动态重建转码上下文（支持源流分辨率动态改变）；
 * 5. 提取展示时间戳（best_effort_timestamp 回退 pts）并折算为微秒；
 * 6. 将转码后的像素深拷贝至新分配的连续内存 VideoFrame 中，消除外部对 FFmpeg 底层缓冲的依赖。
 */
FfmpegDecoderReceiveResult FfmpegVideoDecoder::ConvertCurrentFrame(std::uint64_t epoch) {
    if (decoded_frame_->width <= 0 || decoded_frame_->height <= 0 || decoded_frame_->format < 0) {
        LOG_ERROR("Decoder '{}' produced an invalid frame for epoch {}: {}x{}, format {}",
                  codec_name_,
                  epoch,
                  decoded_frame_->width,
                  decoded_frame_->height,
                  decoded_frame_->format);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                MakeDecoderError(kReceiveFrameOperation, AVERROR_INVALIDDATA)};
    }

    const AVPixelFormat source_format = static_cast<AVPixelFormat>(decoded_frame_->format);
    if (sws_isSupportedInput(source_format) <= 0) {
        LOG_ERROR("Decoder '{}' produced unsupported swscale input format {} for epoch {}",
                  codec_name_,
                  decoded_frame_->format,
                  epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                MakeDecoderError(kReceiveFrameOperation, AVERROR(EINVAL))};
    }

    const int output_buffer_size =
        av_image_get_buffer_size(AV_PIX_FMT_YUV420P, decoded_frame_->width, decoded_frame_->height, 1);
    if (output_buffer_size < 0) {
        LOG_ERROR("Decoder '{}' produced invalid output dimensions {}x{} for epoch {}, code {}",
                  codec_name_,
                  decoded_frame_->width,
                  decoded_frame_->height,
                  epoch,
                  output_buffer_size);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                MakeDecoderError(kValidateFrameSizeOperation, output_buffer_size)};
    }

    // 复用或按需重建色彩空间与缩放上下文（处理 Dynamic Resolution Change）
    sws_context_ = sws_getCachedContext(sws_context_,
                                        decoded_frame_->width,
                                        decoded_frame_->height,
                                        source_format,
                                        decoded_frame_->width,
                                        decoded_frame_->height,
                                        AV_PIX_FMT_YUV420P,
                                        SWS_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr);

    if (sws_context_ == nullptr) {
        LOG_ERROR("sws_getCachedContext failed for decoder '{}', epoch {}, {}x{}, source format {}",
                  codec_name_,
                  epoch,
                  decoded_frame_->width,
                  decoded_frame_->height,
                  static_cast<int>(source_format));
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                MakeDecoderError(kConvertFrameOperation, AVERROR(ENOMEM))};
    }

    const std::int64_t timestamp = decoded_frame_->best_effort_timestamp != AV_NOPTS_VALUE
        ? decoded_frame_->best_effort_timestamp
        : decoded_frame_->pts;

    const std::optional<std::int64_t> pts_us = TimestampToMicroseconds(timestamp, video_stream_time_base_);

    try {
        // 分配自持连续内存的 VideoFrame
        VideoFrame output(FrameSize{decoded_frame_->width, decoded_frame_->height});
        output.pts_us_ = pts_us;
        output.epoch_ = epoch;

        // 绑定 YUV420P 平面目标地址与行跨度（保留第 4 槽位为 nullptr 满足 FFmpeg 规范）
        std::array<std::uint8_t*, 4> destination_data{output.data_[0], output.data_[1], output.data_[2], nullptr};
        std::array<int, 4> destination_linesize{output.linesize_[0], output.linesize_[1], output.linesize_[2], 0};

        // 执行色彩空间转换与数据深拷贝
        const int converted_height = sws_scale(sws_context_,
                                               decoded_frame_->data,
                                               decoded_frame_->linesize,
                                               0,
                                               decoded_frame_->height,
                                               destination_data.data(),
                                               destination_linesize.data());

        if (converted_height != decoded_frame_->height) {
            LOG_ERROR("sws_scale produced {} rows for decoder '{}' at epoch {}, expected {}",
                      converted_height,
                      codec_name_,
                      epoch,
                      decoded_frame_->height);
            return {FfmpegDecoderReceiveStatus::kError,
                    std::nullopt,
                    MakeDecoderError(kConvertFrameOperation, AVERROR_EXTERNAL)};
        }

        return {FfmpegDecoderReceiveStatus::kFrame, std::optional<VideoFrame>(std::move(output)), {}};
    } catch (const std::bad_alloc&) {
        LOG_ERROR("Failed to allocate output frame for decoder '{}' at epoch {}", codec_name_, epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                MakeDecoderError(kConvertFrameOperation, AVERROR(ENOMEM))};
    } catch (const std::length_error&) {
        LOG_ERROR("Output frame size is invalid for decoder '{}' at epoch {}", codec_name_, epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                MakeDecoderError(kConvertFrameOperation, AVERROR(EINVAL))};
    } catch (const std::invalid_argument&) {
        LOG_ERROR(
            "Failed to create output frame for decoder '{}' at epoch {} due to invalid arguments", codec_name_, epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                MakeDecoderError(kConvertFrameOperation, AVERROR(EINVAL))};
    }
}

}  // namespace mediacore
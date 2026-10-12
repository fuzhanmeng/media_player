/**
 * @file ffmpeg_video_decoder.h
 * @brief 基于 FFmpeg libavcodec 的底层视频解码器封装（将 Packet 解码并转码为 YUV420P VideoFrame）
 * @details
 * 负责管理解码器上下文与像素格式转换：
 * 1. 核心流程：围绕 avcodec_send_packet() 与 avcodec_receive_frame() 的异步流转状态机运作；
 *    - 若 send 返回 EAGAIN，说明解码器缓冲区已满，调用方必须先 receive 腾出空间后再重试该 packet；
 *    - 读到流末尾（EOF）时调用 BeginDrain() 发送 nullptr packet，并持续 receive 直到 AVERROR_EOF；
 *    - Seek 跳转后调用 Flush()（内部调用 avcodec_flush_buffers()）清空历史参考帧与内部重排队列；
 * 2. 内存复用与转码：内部独占可复用的 AVFrame 与 SwsContext，将解码帧统一转为标准化 YUV420P VideoFrame；
 * 3. 线程安全约束：AVCodecContext 非线程安全，SendPacket / ReceiveFrame / BeginDrain / Flush
 * 必须由同一工作线程串行调用。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "av_types/packet.h"
#include "av_types/video_codec_parameters.h"
#include "mediacore/av_types/stream_info.h"
#include "mediacore/av_types/video_frame.h"
#include "mediacore/logger.h"
#include "mediacore/media_error.h"

struct AVCodecContext;
struct AVFrame;
struct SwsContext;

namespace mediacore {

/**
 * @enum FfmpegDecoderSendStatus
 * @brief avcodec_send_packet() 执行状态枚举
 */
enum class FfmpegDecoderSendStatus : std::uint8_t {
    kAccepted,  ///< 数据包或排空信号已被底层解码器成功接收
    kNeedReceive,  ///< 解码缓冲区已满（EAGAIN），必须先调用 ReceiveFrame() 取出解码帧后再重试当前 Packet
    kDrained,  ///< 解码器已进入排空结束状态，调用 Flush() 之前禁止再发送普通媒体数据包
    kError,  ///< 底层发生不可恢复的解码输入错误
};

/**
 * @struct FfmpegDecoderSendResult
 * @brief SendPacket() 与 BeginDrain() 的复合返回载荷
 */
struct FfmpegDecoderSendResult final {
    FfmpegDecoderSendStatus status_{FfmpegDecoderSendStatus::kError};  ///< 发送操作状态
    MediaError media_error_{};  ///< 失败时的详细错误信息
};

/**
 * @enum FfmpegDecoderReceiveStatus
 * @brief avcodec_receive_frame() 执行状态枚举
 */
enum class FfmpegDecoderReceiveStatus : std::uint8_t {
    kFrame,  ///< 成功获取并转码输出一帧完整的 VideoFrame
    kNeedInput,  ///< 解码器需要更多输入（EAGAIN），调用方应继续提交新的 Packet
    kDrained,  ///< 排空完成（AVERROR_EOF），解码器内已无任何残留数据
    kError,  ///< 帧接收或像素格式转换（sws_scale）发生错误
};

/**
 * @struct FfmpegDecoderReceiveResult
 * @brief ReceiveFrame() 的复合返回载荷
 */
struct FfmpegDecoderReceiveResult final {
    FfmpegDecoderReceiveStatus status_{FfmpegDecoderReceiveStatus::kError};  ///< 接收操作状态
    std::optional<VideoFrame> frame_{};  ///< 成功解码并转换后的视频帧
    MediaError media_error_{};  ///< 失败时的详细错误信息
};

class FfmpegVideoDecoder;

/**
 * @struct FfmpegVideoDecoderOpenResult
 * @brief FfmpegVideoDecoder::Open() 的输出载荷
 */
struct FfmpegVideoDecoderOpenResult final {
    std::unique_ptr<FfmpegVideoDecoder> video_decoder_{};  ///< 创建成功的解码器实例（独占所有权）
    MediaError error_{};  ///< 失败时的错误信息

    /**
     * @brief 判定解码器是否成功初始化
     */
    bool Succeeded() const noexcept { return video_decoder_ != nullptr && !error_.HasError(); }
};

/**
 * @class FfmpegVideoDecoder
 * @brief 拥有并管理 AVCodecContext 的视频解码与格式转换执行器
 */
class FfmpegVideoDecoder final {
public:
    /**
     * @brief 析构函数：释放解码器上下文、转换上下文及持有的复用 AVFrame
     */
    ~FfmpegVideoDecoder();

    // 严禁拷贝与移动，保证底层句柄生命周期唯一明确
    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder(FfmpegVideoDecoder&&) = delete;
    FfmpegVideoDecoder& operator=(FfmpegVideoDecoder&&) = delete;

    /**
     * @brief 静态工厂：查找并打开视频解码器
     * @param parameters 解封装层解析出的流编码参数（所有权转移）
     * @param video_stream_time_base 视频流原生时间基准，用于将解码帧的 PTS 正确换算为时间轴刻度
     * @return 包含解码器实例或失败原因的初始化结果
     */
    static FfmpegVideoDecoderOpenResult Open(VideoCodecParameters parameters, Rational video_stream_time_base);

    /**
     * @brief 向底层解码器提交一个待解码的压缩数据包
     * @param packet 包含压缩码流数据与时间戳的输入包
     * @return 发送状态结果；若返回 kNeedReceive，外部需先取走输出帧再重试当前包
     */
    FfmpegDecoderSendResult SendPacket(const Packet& packet);

    /**
     * @brief 向解码器下发排空信号（传入 nullptr packet 进入 Drain 模式）
     * @return 发送结果；成功后调用方应循环调用 ReceiveFrame() 直至返回 kDrained
     */
    FfmpegDecoderSendResult BeginDrain();

    /**
     * @brief 从解码器提取一帧画面，并转码为项目标准 YUV420P 格式
     * @param epoch 当前输出帧所属的媒体流世代版本号
     * @return 接收与转码结果
     */
    FfmpegDecoderReceiveResult ReceiveFrame(std::uint64_t epoch);

    /**
     * @brief 彻底清空解码器内部的参考帧缓冲与重排队列（用于 Seek 重置）
     * @note 仅用于时间跳转（Seek）后的 StreamReset 场景；普通的 EOF 流程必须使用 BeginDrain() 排空残留
     */
    void Flush() noexcept;

    /**
     * @brief 获取底层解码器的名称（如 "h264", "hevc"）
     */
    const std::string& GetVideoCodecName() const noexcept { return codec_name_; }

    /**
     * @brief 获取码流中声明的编码宽度
     * @note 编码宽度可能包含填充像素或对齐需求，最终呈现尺寸以实际输出帧的 VideoFrame::width 为准
     */
    int GetCodedWidth() const noexcept;

    /**
     * @brief 获取码流中声明的编码高度
     */
    int GetCodedHeight() const noexcept;

    /**
     * @brief 查询当前解码器是否已完全完成排空
     */
    bool IsDrained() const noexcept { return drained_; }

private:
    /**
     * @brief 私有构造函数，由 Open() 验证成功后通过 std::make_unique 创建
     */
    FfmpegVideoDecoder(AVCodecContext* codec_context,
                       AVFrame* frame,
                       Rational video_stream_time_base,
                       std::string codec_name) noexcept;

    /**
     * @brief 将 decoded_frame_ 中的原始 AVFrame 转换为项目自定义的 YUV420P VideoFrame
     * @param epoch 目标帧所属的数据流世代版本号
     */
    FfmpegDecoderReceiveResult ConvertCurrentFrame(std::uint64_t epoch);

    AVCodecContext* codec_context_{nullptr};  ///< 独占的 FFmpeg 解码器内部状态
    AVFrame* decoded_frame_{nullptr};  ///< avcodec_receive_frame() 反复写入的复用接收帧缓冲
    SwsContext* sws_context_{nullptr};  ///< 像素格式与色彩空间转换上下文句柄
    Rational video_stream_time_base_{};  ///< 视频流原生时间基准
    std::string codec_name_{};  ///< 解码器名称文本
    bool drain_started_{false};  ///< 是否已发送 nullptr packet 开始排空
    bool drained_{false};  ///< 是否已完全排空完毕（收到 AVERROR_EOF）
};

}  // namespace mediacore
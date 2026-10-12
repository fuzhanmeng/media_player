/**
 * @file ffmpeg_video_decoder_node.cpp
 * @brief 视频解码器节点实现（负责消费 Packet 并输出解码转码后的 VideoFrame）
 * @details
 * 节点在专属工作线程中运行，核心职责包括：
 * 1. 消费上游 DemuxerNode 传递的 StreamItem<Packet>；
 * 2. 调度 FfmpegVideoDecoder 执行底层解码及 YUV420P 像素转换；
 * 3. 严格遵循 FFmpeg 状态机：妥善处理 EAGAIN 重试与 nullptr packet 尾帧排空；
 * 4. 维护 Epoch 世代隔离，确保 Seek 后的 StreamReset 与流结束 EndOfStream 有序下发。
 */

#include "video_decoder/ffmpeg_video_decoder_node.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "mediacore/av_types/video_frame.h"
#include "mediacore/logger.h"
#include "mediacore/media_error.h"

namespace mediacore {

namespace {
constexpr const char* kNodeName = "ffmpeg_video_decoder";
}

/**
 * @brief 构造视频解码节点并初始化世代信息
 */
FfmpegVideoDecoderNode::FfmpegVideoDecoderNode(std::shared_ptr<SyncContext> sync_ctx,
                                               PipelineEventCallbacks event_callbacks,
                                               std::unique_ptr<FfmpegVideoDecoder> video_decoder)
    : INode(kNodeName, std::move(sync_ctx), std::move(event_callbacks)), video_decoder_(std::move(video_decoder)) {
    if (!video_decoder_) {
        LOG_ERROR("FfmpegVideoDecoderNode requires an opened Decoder");
        throw std::invalid_argument("FfmpegVideoDecoderNode requires an opened Decoder");
    }

    output_epoch_ = GetCurrentEpoch();
    LOG_INFO("Video Decoder Node created for codec '{}', initial epoch {}",
             video_decoder_->GetVideoCodecName(),
             output_epoch_);
}

/**
 * @brief 从上游 Link 消费并分发 Packet、StreamReset 或 EndOfStream
 * @details
 * 1. ReceiveInput() 在队列为空时阻塞等待；
 * 2. 停机或终止时 Link 触发 Abort 返回 std::nullopt；
 * 3. 正常媒体流结束（EOF）通过 EndOfStream 变体项显式传递，非通道断开。
 */
TickResult FfmpegVideoDecoderNode::Tick() {
    std::optional<StreamItem<Packet>> item = ReceiveInput();

    // 未能成功接收到数据项（Link 被打断或不可用）
    if (!item.has_value()) {
        eof_parked_.store(false, std::memory_order_release);

        if (IsStopRequested()) {
            LOG_DEBUG("Video decoder node input link was interrupted by stop");
            return TickResult::kStopped;
        }

        LOG_ERROR("Video decoder node input link is unavailable without a stop request");
        return Fail("Video decoder input link is unavailable", GetCurrentEpoch());
    }

    // 分发具体的数据流元素
    if (auto* packet = std::get_if<Packet>(&*item)) {
        return HandlePacket(*packet);
    }

    if (const auto* reset = std::get_if<StreamReset>(&*item)) {
        return HandleStreamReset(*reset);
    }

    if (const auto* eos = std::get_if<EndOfStream>(&*item)) {
        return HandleEndOfStream(*eos);
    }

    return FailIfEpochIsCurrent("VideoDecoderNode received an unknown stream item", output_epoch_);
}

/**
 * @brief 当且仅当错误所属的 epoch 等于当前 Pipeline epoch 时上报文本错误
 * @param message 错误描述文本
 * @param epoch 错误关联的世代号
 */
TickResult FfmpegVideoDecoderNode::FailIfEpochIsCurrent(std::string message, std::uint64_t epoch) {
    if (!IsCurrentEpoch(epoch)) {
        LOG_DEBUG("Ignoring non-current Decoder error for epoch {}", epoch);
        return TickResult::kContinue;
    }

    LOG_ERROR("Video decoder node failed at epoch {}: {}", epoch, message);
    return Fail(message, epoch);
}

/**
 * @brief 当且仅当错误所属的 epoch 等于当前 Pipeline epoch 时上报结构化 MediaError
 * @param error 结构化媒体错误对象
 * @param epoch 错误关联的世代号
 */
TickResult FfmpegVideoDecoderNode::FailIfEpochIsCurrent(const MediaError& error, std::uint64_t epoch) {
    if (!IsCurrentEpoch(epoch)) {
        LOG_DEBUG("Ignoring non-current Decoder error for epoch {}", epoch);
        return TickResult::kContinue;
    }

    LOG_ERROR("Video decoder node failed at epoch {}: {}", epoch, error.ToString());
    return Fail(error, epoch);
}

/**
 * @brief 提交单个输入 Packet 到解码器，并将就绪的解码视频帧投递至下游
 * @param packet 待解码的视频压缩数据包
 */
TickResult FfmpegVideoDecoderNode::HandlePacket(Packet& packet) {
    // 丢弃过时世代的 Packet
    if (!IsCurrentEpoch(packet.GetEpoch())) {
        return TickResult::kContinue;
    }

    // 新世代 Packet 到达前必须已建立对应的 StreamReset
    if (packet.GetEpoch() != output_epoch_) {
        return FailIfEpochIsCurrent("VideoDecoderNode received a new-epoch Packet before StreamReset",
                                    packet.GetEpoch());
    }

    // 循环提交 Packet，直到被成功接收或因异常退出
    while (true) {
        if (!IsCurrentEpoch(packet.GetEpoch())) {
            return TickResult::kContinue;
        }

        const FfmpegDecoderSendResult send_result = video_decoder_->SendPacket(packet);

        switch (send_result.status_) {
            case FfmpegDecoderSendStatus::kAccepted:
                // 数据包已被接受，抽取并转发当前可输出的所有帧
                return ReceiveAndForwardFrames(false, false);

            case FfmpegDecoderSendStatus::kNeedReceive: {
                // 缓冲区已满（EAGAIN），先取走已解码帧腾出缓冲空间，再重试当前包
                const TickResult receive_result = ReceiveAndForwardFrames(false, true);
                if (receive_result != TickResult::kContinue) {
                    return receive_result;
                }
                continue;
            }

            case FfmpegDecoderSendStatus::kDrained:
                return FailIfEpochIsCurrent("VideoDecoder rejected packet because codec is still drained",
                                            packet.GetEpoch());

            case FfmpegDecoderSendStatus::kError:
                return FailIfEpochIsCurrent(send_result.media_error_, packet.GetEpoch());
        }

        return FailIfEpochIsCurrent("VideoDecoder returned an unknown send status", packet.GetEpoch());
    }
}

/**
 * @brief 处理接收到的 StreamReset：清空解码器缓存、同步世代并向下游转发重置信令
 * @param reset 携带新世代 Epoch 与目标时间戳的信令
 */
TickResult FfmpegVideoDecoderNode::HandleStreamReset(const StreamReset& reset) {
    if (!IsCurrentEpoch(reset.epoch_)) {
        return TickResult::kContinue;
    }

    // 丢弃解码器内部的陈旧参考帧与重排缓存
    video_decoder_->Flush();

    // 向下游 VideoSinkNode 转发 StreamReset
    if (!SendOutput(VideoFrameStreamItem{reset})) {
        if (IsStopRequested()) {
            LOG_DEBUG("Video Decoder node could not forward StreamReset for epoch {}, output was interrupted by stop",
                      reset.epoch_);
            return TickResult::kStopped;
        }

        LOG_ERROR("Video Decoder node could not forward StreamReset for epoch {}, output Link is unavailable",
                  reset.epoch_);
        return Fail("Video Decoder output link is unavailable while forwarding StreamReset", reset.epoch_);
    }

    // 成功转发后更新当前活跃世代与状态标记
    output_epoch_ = reset.epoch_;
    eof_parked_.store(false, std::memory_order_release);
    eos_forwarded_ = false;

    LOG_DEBUG("Video Decoder reset to epoch {}", reset.epoch_);
    return TickResult::kContinue;
}

/**
 * @brief 执行解码器 Drain 流程：排空残留延迟帧并向下游转发 EndOfStream
 * @param eos 包含流结束世代号的控制项
 */
TickResult FfmpegVideoDecoderNode::HandleEndOfStream(const EndOfStream& eos) {
    if (!IsCurrentEpoch(eos.epoch_)) {
        return TickResult::kContinue;
    }

    if (eos.epoch_ != output_epoch_) {
        return FailIfEpochIsCurrent("Video Decoder Node received mismatched EOS for epoch", eos.epoch_);
    }

    // 同一世代内仅允许转发一次 EOS
    if (eos_forwarded_) {
        return FailIfEpochIsCurrent("VideoDecoderNode received a new-epoch EOS before StreamReset", eos.epoch_);
    }

    LOG_INFO("Video Decoder Node received EOS for epoch {}, draining delayed frames", eos.epoch_);

    // 触发解码器进入 Drain 状态（发送 nullptr packet）
    while (true) {
        if (!IsCurrentEpoch(eos.epoch_)) {
            return TickResult::kContinue;
        }

        const FfmpegDecoderSendResult send_result = video_decoder_->BeginDrain();

        if (send_result.status_ == FfmpegDecoderSendStatus::kAccepted) {
            break;
        }

        if (send_result.status_ == FfmpegDecoderSendStatus::kNeedReceive) {
            // 解码器存在尚未取出的帧，需先取出帧再尝试 BeginDrain
            const TickResult receive_result = ReceiveAndForwardFrames(false, true);
            if (receive_result != TickResult::kContinue) {
                return receive_result;
            }
            continue;
        }

        if (send_result.status_ == FfmpegDecoderSendStatus::kDrained) {
            return FailIfEpochIsCurrent("Video Decoder was already drained before ordered EOS", eos.epoch_);
        }

        if (send_result.status_ == FfmpegDecoderSendStatus::kError) {
            return FailIfEpochIsCurrent(send_result.media_error_, eos.epoch_);
        }

        return FailIfEpochIsCurrent("Video Decoder returned an unknown drain send status", eos.epoch_);
    }

    // 循环提取 Drain 阶段产出的所有尾帧，直到解码器返回 AVERROR_EOF
    const TickResult frame_result = ReceiveAndForwardFrames(true, false);
    if (frame_result != TickResult::kContinue) {
        return frame_result;
    }

    if (!IsCurrentEpoch(output_epoch_)) {
        return TickResult::kContinue;
    }

    // 向下游发送 EndOfStream 信令
    if (!SendOutput(VideoFrameStreamItem{EndOfStream{output_epoch_}})) {
        if (IsStopRequested()) {
            LOG_DEBUG("Video decoder node could not forward EOS for epoch {}; output was interrupted by stop",
                      output_epoch_);
            return TickResult::kStopped;
        }

        LOG_ERROR("Video Decoder Node could not forward EOS for epoch {}, output Link is unavailable", output_epoch_);
        return Fail("Video Decoder output link is unavailable while forwarding EndOfStream", output_epoch_);
    }

    eos_forwarded_ = true;
    eof_parked_.store(true, std::memory_order_release);

    LOG_INFO("Video Decoder Node finished draining and forwarded EOS for epoch {}", output_epoch_);
    return TickResult::kContinue;
}

/**
 * @brief 循环读取解码后的视频帧并推送到下游 Link
 * @param draining 是否正处于 EOF 排空模式（此时 ReceiveFrame 最终必须返回 kDrained）
 * @param previous_send_was_eagain 上一次发送操作是否返回了 EAGAIN
 */
TickResult FfmpegVideoDecoderNode::ReceiveAndForwardFrames(bool draining, bool previous_send_was_eagain) {
    bool received_any_frame = false;

    while (true) {
        if (!IsCurrentEpoch(output_epoch_)) {
            return TickResult::kContinue;
        }

        FfmpegDecoderReceiveResult receive_result = video_decoder_->ReceiveFrame(output_epoch_);

        switch (receive_result.status_) {
            case FfmpegDecoderReceiveStatus::kFrame: {
                if (!receive_result.frame_.has_value()) {
                    return FailIfEpochIsCurrent("Video Decoder returned kFrame without a VideoFrame", output_epoch_);
                }

                received_any_frame = true;
                VideoFrame& frame = *receive_result.frame_;

                if (!IsCurrentEpoch(frame.epoch_)) {
                    continue;
                }

                if (!SendOutput(VideoFrameStreamItem{std::in_place_type<VideoFrame>, std::move(frame)})) {
                    if (IsStopRequested()) {
                        LOG_DEBUG("Video Decoder Node was interrupted by stop while forwarding epoch {}",
                                  output_epoch_);
                        return TickResult::kStopped;
                    }

                    LOG_ERROR("Video decoder node output Link is unavailable while forwarding a frame for epoch {}",
                              output_epoch_);
                    return Fail("Video decoder output link is unavailable while forwarding a frame", output_epoch_);
                }
                continue;
            }

            case mediacore::FfmpegDecoderReceiveStatus::kNeedInput:
                // Drain 阶段底层不应再返回 EAGAIN
                if (draining) {
                    return FailIfEpochIsCurrent("Video Decoder returned EAGAIN after drain started", output_epoch_);
                }

                // Send 和 Receive 连续返回 EAGAIN 且未取到任何新帧，说明状态机锁死无进展
                if (previous_send_was_eagain && !received_any_frame) {
                    return FailIfEpochIsCurrent("VideoDecoder send and receive both returned EAGAIN", output_epoch_);
                }

                return TickResult::kContinue;

            case mediacore::FfmpegDecoderReceiveStatus::kDrained:
                // 仅在主动 Drain 阶段返回 EOF 属于正常排空完毕
                return draining
                    ? TickResult::kContinue
                    : FailIfEpochIsCurrent("VideoDecoder reached EOF before ordered EOS drain", output_epoch_);

            case FfmpegDecoderReceiveStatus::kError:
                return FailIfEpochIsCurrent(receive_result.media_error_, output_epoch_);
        }

        return FailIfEpochIsCurrent("VideoDecoder returned an unknown receive status", output_epoch_);
    }
}

}  // namespace mediacore
/**
 * @file ffmpeg_audio_decoder_node.h
 * @brief 音频解码器节点
 * @details 本节点是 音频解码模块，用于处理 来自 Demuxer 发送过来的 Packet，解码输出为 PcmFrame
 */

#pragma once

#include <cstdint>
#include <memory>

#include "audio_decoder/ffmpeg_audio_decoder.h"
#include "audio_decoder/pcm_frame.h"
#include "av_types/packet.h"
#include "core/inode.h"
#include "core/stream_item.h"
#include "mediacore/media_error.h"

namespace mediacore {

class SyncContext;
class PipelineEventCallbacks;

/**
 * @brief 解码节点向下游 AudioSinkNode 输出的数据流载体（Variant 变体类型）
 * @details 在任意时刻承载以下三类数据项之一：
 * 1. PcmFrame：解码并转换完成的 YUV420P 像素帧；
 * 2. StreamReset：跳转 Seek 后重置渲染时钟与画面缓冲的控制信令；
 * 3. EndOfStream：当前流已全部解码排空完毕的终止信号
 */
using PcmFrameStreamItem = StreamItem<PcmFrame>;

/**
 * @class FfmpegAudioDecoderNode
 * @brief std::unique_ptr 独占类型的音频解码节点
 * @details
 * Decode / Drain / Flush 都运行在本节点的线程中，控制线程通过共享的 SyncContext 更新 epoch
 */
class FfmpegAudioDecoderNode : public INode<StreamItem<Packet>, PcmFrameStreamItem> {
public:
    /**
     * @brief 构造函数：接管已经打开成功的解码器
     * @param sync_ctx 全局同步上下文
     * @param event_callbacks 用于向 Pipeline 控制层汇报错误与状态事件的回调集合
     * @param audio_decoder 音频解码器
     * @throw std::invalid_argument 当 sync_ctx 或 audio_decoder 为空时抛出
     */
    FfmpegAudioDecoderNode(std::shared_ptr<SyncContext> sync_ctx,
                           PipelineEventCallbacks event_callbacks,
                           std::unique_ptr<FfmpegAudioDecoder> audio_decoder);

    /**
     * @brief 析构函数：释放节点以及相关资源
     */
    ~FfmpegAudioDecoderNode() = default;

    /**
     * @brief 严禁拷贝和移动
     */
    FfmpegAudioDecoderNode(const FfmpegAudioDecoderNode&) = delete;
    FfmpegAudioDecoderNode& operator=(const FfmpegAudioDecoderNode) = delete;
    FfmpegAudioDecoderNode(FfmpegAudioDecoderNode&&) = delete;
    FfmpegAudioDecoderNode& operator=(FfmpegAudioDecoderNode&&) = delete;

protected:
    /**
     * @brief 处理一个 Packet / StreamReset / EndOfStream
     * @return TickResult 执行结果：
     * - kContinue：当前消息处理成功，继续进入下一轮 Tick；
     * - kStopped：因收到外部停止指令或 Link 被打断而正常退出线程；
     * - kError：发生当前世代不可恢复的致命异常并上报 Pipeline
     */
    TickResult Tick() override;

private:
    /**
     * @brief 处理普通压缩数据包：提交给解码器并循环抽取、转发产生的输出视频帧
     * @param packet Demuxer 投递的媒体包
     * @return 处理状态；若遇到解码器 EAGAIN，会自动调用 ReceiveAndForwardFrames 腾出内部缓冲后重试提交
     */
    TickResult HandlePacket(const Packet& packet);

    /**
     * @brief 处理时间跳转（Seek）后的流重置信令
     * @param reset 包含新世代 Epoch 与目标时间戳的控制载荷
     * @details
     * 先调用 FfmpegVideoDecoder::Flush() 丢弃历史世代的参考帧与重排队列，
     * 在成功向下游 Link 转发 StreamReset 后更新 output_epoch_，确保新帧不会跨越 Reset 产生顺序错乱。
     */
    TickResult HandleStreamReset(const StreamReset& reset);

    /**
     * @brief 处理流结束信号：向解码器发送 nullptr packet 触发排空，并在尾帧全部上屏后转发 EOS
     * @param eos 包含流结束世代号的控制载荷
     * @details 必须循环调用 ReceiveFrame() 直至返回 kDrained，才能将 EndOfStream 推送给下游。
     */
    TickResult HandleEndOfStream(const EndOfStream& eos);

    /**
     * @brief 把一块 PCM 推送到 AudioSinkNode
     * @param frame 即 PcmFrame 块，当前最多 240 个采样帧
     * @return 入队成功且 epoch 有效时返回 true，停止、epoch 过期或队列不可用返回 false
     */
    bool ForWard(PcmFrame frame);

    /**
     * @brief 当向下游投递数据或通知被中断时，裁决当前工作线程的下一步动作
     * @return
     * 如果是正常停止，返回 kStopped
     * 如果是 Seek 触发的 epoch 变化，返回 kContinue
     * 如果是 不可恢复性错误，返回 kError
     */
    TickResult CancelledResult();

    /**
     * @brief 校验错误是否发生在当前有效的播放世代
     * @param media_error 结构化错误描述
     * @param epoch 触发该错误的数据所归属的世代编号
     * @return
     * - TickResult::kStopped: 工作线程正常退出；
     * - TickResult::kContinue: 该数据已在 Seek 跳转中作废（epoch 已过期），忽略该旧错误并继续运行；
     * - TickResult::kError: 不可恢复性错误
     */
    TickResult FailIfEpochIsCurrent(MediaError error, std::uint64_t epoch);

private:
    std::unique_ptr<FfmpegAudioDecoder> audio_decoder_{nullptr}; ///< 本节点独占的音频解码器
    std::uint64_t output_epoch_{0};  ///< 本节点处理的 epoch 编号
    bool eos_forwarded_ { false }; ///< 是否收到了 EOS 通知
};
}  // namespace mediacore
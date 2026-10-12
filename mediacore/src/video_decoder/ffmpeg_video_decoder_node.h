/**
 * @file ffmpeg_video_decoder_node.h
 * @brief 将 Packet 压缩输入流解码为 VideoFrame 原始图像帧输出的流水线节点
 * @details
 * 节点在专属的后台工作线程（Worker Thread）中驱动解码流水线：
 * 1. 消费来自 Demuxer 节点的 StreamItem<Packet>（包含普通媒体包 Packet、流重置信令 StreamReset 和流结束信号
 * EndOfStream）；
 * 2. 独占调用 FfmpegVideoDecoder 执行底层音视频解码与 YUV420P 像素色彩空间转换；
 * 3. 严格遵循 FFmpeg 异步交互协议组织 Send / Receive 流程，妥善处理 EAGAIN 缓冲满回退与 Drain 尾帧排空；
 * 4. 维护世代隔离（Epoch Isolation），将解码产物有序编排并推向下游 VideoSinkNode。
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "av_types/packet.h"
#include "core/inode.h"
#include "core/stream_item.h"
#include "mediacore/av_types/video_frame.h"
#include "video_decoder/ffmpeg_video_decoder.h"

namespace mediacore {

/**
 * @brief 解码节点向下游 VideoSinkNode 输出的数据流载体（Variant 变体类型）
 * @details 在任意时刻承载以下三类数据项之一：
 * 1. VideoFrame：解码并转换完成的 YUV420P 像素帧；
 * 2. StreamReset：跳转 Seek 后重置渲染时钟与画面缓冲的控制信令；
 * 3. EndOfStream：当前流已全部解码排空完毕的终止信号。
 */
using VideoFrameStreamItem = StreamItem<VideoFrame>;

/**
 * @class FfmpegVideoDecoderNode
 * @brief 消费压缩 Packet，驱动解码器并将生成的 VideoFrame 推送至下游渲染节点
 * @details
 * 线程安全设计：
 * 节点独占一个 FfmpegVideoDecoder 实例。由于 FFmpeg 的 AVCodecContext 属于非线程安全结构，
 * 所有针对底层解码器的 SendPacket / ReceiveFrame / Flush 等操作全部限制在本节点唯一的 Worker 线程中串行执行。
 */
class FfmpegVideoDecoderNode final : public INode<StreamItem<Packet>, VideoFrameStreamItem> {
public:
    /**
     * @brief 构造函数：初始化解码节点并接管已打开的解码器实例
     * @param sync_ctx 跨节点共享的播放控制与状态同步上下文
     * @param event_callbacks 用于向 Pipeline 控制层汇报错误与状态事件的回调集合
     * @param video_decoder 已完成初始化探测的视频解码器实例（所有权转移）
     * @throws std::invalid_argument 当 sync_ctx 或 video_decoder 为空时抛出
     */
    FfmpegVideoDecoderNode(std::shared_ptr<SyncContext> sync_ctx,
                           PipelineEventCallbacks event_callbacks,
                           std::unique_ptr<FfmpegVideoDecoder> video_decoder);

    /**
     * @brief 析构函数：释放节点相关资源
     */
    ~FfmpegVideoDecoderNode() override = default;

    // 严禁拷贝与移动，保证节点拓扑生命周期唯一
    FfmpegVideoDecoderNode(const FfmpegVideoDecoderNode&) = delete;
    FfmpegVideoDecoderNode& operator=(const FfmpegVideoDecoderNode&) = delete;
    FfmpegVideoDecoderNode(FfmpegVideoDecoderNode&&) = delete;
    FfmpegVideoDecoderNode& operator=(FfmpegVideoDecoderNode&&) = delete;

    /**
     * @brief 获取节点当前是否因解码排空完毕而处于 EOF 停泊状态
     * @return true 表示当前世代已完成全部延迟帧排空（Drained），且 EndOfStream 已成功推送至下游 Link
     */
    bool IsEofParked() const noexcept { return eof_parked_.load(std::memory_order_acquire); }

protected:
    /**
     * @brief 从输入端口消费并分发处理一个 Packet、StreamReset 或 EndOfStream
     * @return TickResult 执行结果：
     * - kContinue：当前消息处理成功，继续进入下一轮 Tick；
     * - kStopped：因收到外部停止指令或 Link 被打断而正常退出线程；
     * - kError：发生当前世代不可恢复的致命异常并上报 Pipeline。
     */
    TickResult Tick() override;

private:
    /**
     * @brief 处理普通压缩数据包：提交给解码器并循环抽取、转发产生的输出视频帧
     * @param packet Demuxer 投递的媒体包
     * @return 处理状态；若遇到解码器 EAGAIN，会自动调用 ReceiveAndForwardFrames 腾出内部缓冲后重试提交
     */
    TickResult HandlePacket(Packet& packet);

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
     * @brief 循环接收并向下游 Link 转发当前解码器内部产出的所有有效 VideoFrame
     * @param draining true 表示当前正处于 EOF 排空模式，应持续抽取直至获得 kDrained；false 表示常规解码输出
     * @param previous_send_was_eagain true 表示本次是因为 send 返回 EAGAIN 而触发的腾空调用
     * @return 抽取与转发的综合执行结果
     */
    TickResult ReceiveAndForwardFrames(bool draining, bool previous_send_was_eagain);

    /**
     * @brief 世代校验错误上报：仅当报错所属的 Epoch 仍等于当前全局 Epoch 时才终止管线
     * @param message 错误文本描述
     * @param epoch 触发异常时数据包所绑定的世代号
     * @details
     * 若 Seek 已经推进了全局 Epoch，由旧 Packet 引发的延迟解码错误会被安全记录并丢弃，
     * 避免已被放弃的历史旧帧错误误杀新启动的播放管线。
     */
    TickResult FailIfEpochIsCurrent(std::string message, std::uint64_t epoch);

    /**
     * @brief 针对结构化 MediaError 的 FailIfEpochIsCurrent 重载版本
     * @param error 结构化媒体错误详情
     * @param epoch 触发异常时数据包所绑定的世代号
     */
    TickResult FailIfEpochIsCurrent(const MediaError& error, std::uint64_t epoch);

    std::unique_ptr<FfmpegVideoDecoder> video_decoder_;  ///< 独占的底层 FFmpeg 解码器包装实例
    std::uint64_t output_epoch_{0};  ///< 已由 StreamReset 建立、下游正在消费的世代号
    bool eos_forwarded_{false};  ///< 当前 output_epoch_ 是否已向下游发送过 EndOfStream
    std::atomic<bool> eof_parked_{false};  ///< 当前世代是否已完成全流排空并等待新 Reset/Stop
};

}  // namespace mediacore
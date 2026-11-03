/**
 * @file ffmpeg_demuxer_node.h
 * @brief 媒体流水线中的解复用节点（视频数据源 Source Node）
 * @details
 * 该节点拥有独立的后台工作线程，作为整个流水线的数据驱动源头：
 * 1. 负责独占调度 FfmpegDemuxer 执行数据读取（Read）与时间定位（Seek）；
 * 2. 读出的 Packet 压缩数据包推入下游 Link；
 * 3. 定位成功后向下游广播 StreamReset 冲刷信令；
 * 4. 读到流末尾（EOF）时向下游投递 EndOfStream 信号，并利用条件变量（WaitAtEof）阻塞自身工作线程，杜绝 CPU 空转。
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <chrono>

#include "av_sync/playback_timeline.h"
#include "av_types/packet.h"
#include "core/inode.h"
#include "core/iseek_source.h"
#include "core/stream_item.h"
#include "demux/ffmpeg_demuxer.h"

namespace mediacore {

/**
 * @brief 解复用节点向下游传递的数据类型。
 *
 * @details
 * StreamItem<Packet> 是一个变体类型，同一时刻只保存以下类型中的一种：
 * - Packet：一份压缩音频包或视频包，具体轨道由 Packet 的 stream_index 区分；
 * - StreamReset：Seek 成功后发送的控制消息，要求下游清理旧世代的解码状态；
 * - EndOfStream：当前世代的输入数据已经读完，通知下游结束处理并 drain
 */
using PacketStreamItem = StreamItem<Packet>;

/**
 * @class FfmpegDemuxerNode
 * @brief 从媒体输入中读取压缩音视频包，并将它们分发到下游解码节点的源节点
 * @details
 * 本节点属于 Source Node，无上游输入端口，只向下游提供数据流输出。
 *
 * 工作线程模型：
 * 1. FfmpegDemuxer 的 AVFormatContext 由本节点工作线程独占访问；
 * 2. 工作线程在 Tick() 中读取一个压缩包，并根据 stream_index 选择输出 Link；
 * 3. 外部线程调用 RequestSeek() 时，只更新 pending_seek_ 并触发底层 I/O 中断；
 * 4. 工作线程通过 ProcessPendingControl() 取出最新请求，再串行执行 ExecuteSeek()；
 * 5. 因此 UI/控制线程不会和工作线程并发调用同一个 FFmpeg 解复用上下文
 */
class FfmpegDemuxerNode final : public INodeBase, public ISeekSource {
public:
    // 每条输出 Link 的元素类型都是 PacketStreamItem：Packet / Reset / EOS
    using OutputLink = Link<PacketStreamItem>;
    ;

    /**
     * @brief 构造解复用源节点，并接管已经打开的 FfmpegDemuxer
     * @param sync 跨节点共享的播放控制与状态同步上下文
     * @param event_callbacks 用于向 Pipeline 控制层汇报错误与状态事件的回调集合
     * @param demuxer 已完成 Open 探测的解复用器实例（所有权转移）
     * @param timeline 可选的共享播放时间线；双路 Seek 预滚需要它判断音视频终端是否到达目标
     * @throws std::invalid_argument 当 sync 为空、demuxer 为空或缺少必要回调时抛出
     */
    FfmpegDemuxerNode(std::shared_ptr<SyncContext> sync,
                      PipelineEventCallbacks event_callbacks,
                      std::unique_ptr<FfmpegDemuxer> demuxer,
                      std::shared_ptr<PlaybackTimeline> timeline = {});

    /**
     * @brief 将视频压缩包输出端连接到视频解码器对应的有界 Link
     * @param output Pipeline 创建的视频 Packet 队列；本节点只保存其 shared_ptr，不负责创建队列
     * @details 节点只有处于 Start 前可以连接配置
     */
    bool ConnectVideoOutput(std::shared_ptr<OutputLink> output);

    /**
     * @brief  将音频压缩包输出端连接到音频解码器对应的有界 Link
     * @details 节点只有处于 Start 前可以连接配置
     */
    bool ConnectAudioOutput(std::shared_ptr<OutputLink> output);

    /**
     * @brief 析构节点；成员对象按 RAII 规则释放，底层 Demuxer 由 unique_ptr 管理
     */
    ~FfmpegDemuxerNode() override;

    // 禁用拷贝与移动，保证节点拓扑所有权唯一
    FfmpegDemuxerNode(const FfmpegDemuxerNode&) = delete;
    FfmpegDemuxerNode& operator=(const FfmpegDemuxerNode&) = delete;
    FfmpegDemuxerNode(FfmpegDemuxerNode&&) = delete;
    FfmpegDemuxerNode& operator=(FfmpegDemuxerNode&&) = delete;

    /**
     * @brief 获取当前媒体的元数据
     * @return 包含时长、编码、流配置等信息的 MediaInfo 引用
     */
    const MediaInfo& GetMediaInfo() const noexcept { return demuxer_->GetMediaInfo(); }

    /**
     * @brief 查询解复用工作线程是否已经在 EOF 处挂起等待
     * @return true 表示解复用线程已消费完全部 Packet 并休眠；收到新 Seek 或 Stop 时复位为 false
     * @note 仅反映数据源是否读完，不代表下游解码与渲染节点已全部消费呈现完毕
     */
    bool IsEofParked() const noexcept { return eof_parked_.load(std::memory_order_acquire); }

    /**
     * @brief 异步登记最新的 Seek 跳转请求，并打断当前正阻塞的 I/O 操作
     * @param epoch 本次跳转对应的唯一版本世代号
     * @param target_us 本次跳转的目标时间戳（微秒）
     * @details 非阻塞设计：仅将参数暂存至 pending_seek_ 并触发中断通知，由工作线程异步执行
     */
    void RequestSeek(std::uint64_t epoch, std::int64_t target_us) override;

private:
    /**
     * @brief 工作线程的单次主循环驱动函数
     * @return TickResult 执行结果：
     * - kContinue：继续执行下一次 Tick；
     * - kStopped：收到停止指令或下游连接已关闭，安全退出工作线程；
     * - kError：发生不可恢复的底层错误，上报 Pipeline。
     */
    TickResult Tick() override;

    /**
     * @brief 外部中断触发钩子：响应停止或管线暂停信号，强制打断阻塞中的底层 FFmpeg I/O 并唤醒挂起线程
     */
    void OnInterruptBlockingIo() noexcept override;

    /**
     * @brief 检查是否存在外部提交但尚未被工作线程处理的 Seek 控制请求
     * @return 存在待处理的请求时返回 true
     */
    bool HasPendingControl() const noexcept override;

    /**
     * @brief 提取并执行外部提交的 Seek 请求
     * @return 处理结果（kContinue、kStopped 或 kError）
     * @note 由工作线程在执行数据读取（Tick）前优先调用，保证控制指令的时效性高于数据流
     */
    TickResult ProcessPendingControl() override;

private:
    /**
     * @struct PendingSeek
     * @brief 待执行的 Seek 参数暂存结构体
     */
    struct PendingSeek {
        std::uint64_t epoch_{0};  ///< 跳转请求的世代版本号
        std::int64_t target_us_{0};  ///< 目标跳转时间（微秒）
    };

    /**
     * @brief 当底层数据流读到 EOF 时调用此函数阻塞工作线程，避免高频空转损耗 CPU
     * @return 线程被新 Seek、Flush 或 Stop 唤醒后返回 TickResult::kContinue
     */
    TickResult WaitAtEof();

    /**
     * @brief 执行底层跳转，并在成功后向下游推送 StreamReset 信令
     * @param request 包含世代号与目标时间的具体跳转请求
     * @return 执行状态结果：
     * - kContinue：Seek 成功或已被更新的后续请求替代；
     * - kStopped：收到停止通知或向下游推送信令失败；
     * - kError：跳转发生底层不可恢复异常。
     */
    TickResult ExecuteSeek(PendingSeek request);

    /**
     * @brief 向两条输出队列发送 StreamReset，等待过程中仍可响应 Stop 与更新的 Seek
     * @return kContinue 全部发送完成或被更新的 Seek 取代；kStopped 收到停止；kError 队列终止
     */
    TickResult SendSeekReset(const PendingSeek& request);

    /**
     * @brief 驱动双路 Seek 的预滚、等待就绪和最终重读校验流程
     * @details
     * 当视频、音频两个输出分支同时存在时，单个 Demuxer 读取游标会带来一个问题：
     * 如果视频解码器已经到达目标并停止消费，而 Demuxer 继续读取音频/视频交错数据，视频队列可能会被填满
     * 进而阻塞 Demuxer，导致音频分支无法继续获取需要的数据
     */
    TickResult TickSeekPreroll();

    /**
     * @brief 在同一轮双路 Seek 的阶段切换时，把 Demuxer 读取位置重新定位到目标附近
     * @details
     * 这是一次 Seek 流程内部的重定位，不是新的用户 Seek
     * 不生成新的 Seek、不重新向下游发送 StreamReset、不清空已经送入解码器的状态、只重置底层读取位置
     * @return
     * kContinue：可以继续下一轮处理，或由更新的请求接管
     * kStopped：被 Stop 中断；kError：本轮内部重定位失败
     */
    TickResult RestartSeekPass();

    /**
     * @brief 稳态待发项成功入队后的记录
     * @details 若待发项是 EndOfStream，标记对应轨道已发送，两路都发完后置 eos_sent_
     */
    void OnSteadyPendingPushed();

    struct SeekPreroll;


    std::unique_ptr<FfmpegDemuxer> demuxer_;  ///< 底层 FFmpeg 解复用器

    std::shared_ptr<PlaybackTimeline> timeline_;  ///< 与音视频终端共享的播放时间线状态

    std::unique_ptr<SeekPreroll> seek_preroll_;
    ///< 双路 Seek 的临时状态对象:保存当前预滚阶段、每条轨道已交付包的摘要、
    ///< 最终重读的校验进度，以及因下游队列暂时满而保留的至多一个待发送项

    std::shared_ptr<OutputLink> video_output_;  ///< 视频压缩包输出队列

    std::shared_ptr<OutputLink> audio_output_;  ///< 音频压缩包输出队列

    // -------------------- 数据世代与 EOF 状态 --------------------
    std::uint64_t output_epoch_{0};  ///< 当前节点允许向下游发送的数据所属世代号

    bool eos_sent_{false};  ///< 当前 output_epoch_ 是否已经向所有存在的输出分支发送过 EndOfStream

    /**
     * @brief 稳态播放阶段因下游队列已满而暂存的待发项（Packet 或 EndOfStream）
     * @details 与预滚的 SeekPreroll::pending_ 独立：稳态不校验前缀摘要，只保证一轮一个动作
     */
    std::optional<PacketStreamItem> steady_pending_;
    std::uint64_t steady_pending_epoch_{0};  ///< 待发项所属世代
    PlaybackTrack steady_pending_track_{PlaybackTrack::kVideo};  ///< 待发项应进入哪条输出队列
    bool video_eos_sent_{false};  ///< 当前世代视频队列是否已收到 EOS
    bool audio_eos_sent_{false};  ///< 当前世代音频队列是否已收到 EOS

    // -------------------- 外部控制请求 --------------------
    mutable std::mutex control_mutex_;
    ///< 保护 pending_seek_ 的互斥锁，也用于协调“登记新 Seek”与“建立下一次 I/O 中断基准”

    std::optional<PendingSeek> pending_seek_;  ///< 尚未由工作线程取走的 Seek 请求槽

    // -------------------- EOF 等待 --------------------
    std::atomic<bool> eof_parked_{false};
    ///< 工作线程是否已经进入 WaitAtEof() 等待状态，供外部只读查询

    std::condition_variable eof_cv_;
    ///< EOF 等待条件变量。RequestSeek() 和停止钩子会通知它，唤醒挂起的工作线程


};

}  // namespace mediacore
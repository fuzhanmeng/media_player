/**
 * @file video_sink_node.h
 * @brief 按输入顺序向显示端交付渲染视频帧的终点汇聚节点（Sink Node）
 * @details
 * VideoSinkNode 是视频链路的终端节点，核心职责包括：
 * 1. 消费驱动：从输入 Link 顺序提取 StreamItem<VideoFrame>（包含 VideoFrame、StreamReset 与 EndOfStream）；
 * 2. 帧分发解耦：通过绑定的 VideoSinkDispatcher 将解码帧安全投递给外部注册的 IVideoSink 显示视图；
 * 3. 精准 Seek 截获与挂起（Seek Hold）：
 *    - 在 Seek 期间过滤目标时间前的多余帧，仅保留紧邻前置候选帧作为兜底；
 *    - 首张命中目标时间（PTS >= target）的帧投递上屏后，主动挂起自身工作线程（Hold）；
 *    - 等待 Player 控制线程完成状态决策（Play/Pause）并显式调用 ReleaseSeekHold() 后再放行后续帧读取；
 * 4. 流末尾收敛（EOS）：在完成全部渲染后向上层汇报 MediaEnded 事件并维护 IsEndOfStreamReached 状态。
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "core/inode.h"
#include "core/iseek_hold_releasable.h"
#include "core/stream_item.h"
#include "mediacore/av_types/video_frame.h"
#include "mediacore/ivideo_sink.h"
#include "sink/video_sink_dispatcher.h"

namespace mediacore {

/**
 * @class VideoSinkNode
 * @brief 接收解码后的视频帧并安全交付至 UI 显示端的渲染汇聚节点
 * @details
 * 继承自 INode<StreamItem<VideoFrame>, NoPort>，属于没有下游输出端口的终端消费者。
 * 同时实现 ISeekHoldReleasable 接口，支持由 Player 统一协调精准定位后的画面定格与放行。
 */
class VideoSinkNode final : public INode<StreamItem<VideoFrame>, NoPort>, public ISeekHoldReleasable {
public:
    /**
     * @brief 构造函数：创建关联显示端分发器的视频输出节点
     * @param sync_ctx 跨节点共享的播放控制与状态同步上下文
     * @param event_callbacks 用于向 Pipeline 控制层汇报错误与状态事件的回调集合
     * @param video_frame_dispatcher 负责管理与分发帧至外部 IVideoSink 的共享分发器
     * @throws std::invalid_argument 当 video_frame_dispatcher 为空时抛出
     */
    VideoSinkNode(std::shared_ptr<SyncContext> sync_ctx,
                  PipelineEventCallbacks event_callbacks,
                  std::shared_ptr<VideoSinkDispatcher> video_frame_dispatcher);

    /**
     * @brief 便利构造函数：使用单一已有 IVideoSink 指针创建节点（内部自动创建分发器）
     * @param sync_ctx 跨节点共享的播放控制与状态同步上下文
     * @param event_callbacks 节点事件回调集合
     * @param sink 外部显示端实例指针（非拥有所有权）
     */
    VideoSinkNode(std::shared_ptr<SyncContext> sync_ctx, PipelineEventCallbacks event_callbacks, IVideoSink* sink);

    /**
     * @brief 析构函数：释放节点资源
     */
    ~VideoSinkNode() override = default;

    // 严禁拷贝与移动，确保渲染节点在管线拓扑中的唯一性
    VideoSinkNode(const VideoSinkNode&) = delete;
    VideoSinkNode& operator=(const VideoSinkNode&) = delete;
    VideoSinkNode(VideoSinkNode&&) = delete;
    VideoSinkNode& operator=(VideoSinkNode&&) = delete;

    /**
     * @brief 线程安全地查询当前媒体世代是否已处理至流末尾（EOS）
     * @return true 表示本节点已成功消费并处理当前世代的 EndOfStream 信令
     * @note 仅表明 VideoSinkNode 已完成数据流收尾，不代表 Player 控制循环已处理完毕回调
     */
    bool IsEndOfStreamReached() const noexcept { return active_epoch_eos_processed_.load(std::memory_order_acquire); }

    /**
     * @brief 解除 Seek 定位期间的帧读取挂起阻塞（实现 ISeekHoldReleasable 接口）
     * @details
     * Seek 定位首帧交付给 UI 后，本节点会进入 WaitForSeekRelease 挂起状态以定格画面；
     * Player 控制线程在确认播放状态迁移后调用本函数，唤醒节点继续向后消费数据流。
     */
    void ReleaseSeekHold() noexcept override;

protected:
    /**
     * @brief 节点工作线程的主循环单次驱动逻辑
     * @return TickResult 执行结果：
     * - kContinue：当前项处理成功，继续进入下一轮 Tick；
     * - kStopped：因收到外部停止指令或 Link 被打断而安全退出；
     * - kError：发生不可恢复的致命异常并上报 Pipeline。
     */
    TickResult Tick() override;

    /**
     * @brief 外部中断触发钩子：响应停止通知，唤醒正阻塞在 WaitForSeekRelease() 上的工作线程
     */
    void OnInterruptBlockingIo() noexcept override;

private:
    /**
     * @brief 处理常规视频帧：完成时钟同步、Seek 帧过滤以及向显示端交付渲染
     * @param frame 从输入 Link 提取的解码图像帧
     */
    TickResult HandleVideoFrame(VideoFrame frame);

    /**
     * @brief 处理跳转（Seek）后的流重置信令
     * @param reset 包含新世代 Epoch 与目标时间戳的控制载荷
     * @details 重置内部 Seek 状态机，开启目标首帧搜索并记录新的 active_media_epoch_。
     */
    TickResult HandleStreamReset(const StreamReset& reset);

    /**
     * @brief 处理当前媒体流的结束信号（EndOfStream）
     * @param eos 包含流结束世代号的控制载荷
     * @details
     * 若在 Seek 搜索目标帧期间直接遇到 EOS（如 Seek 到了文件最后空白处），
     * 会自动将此前保存的最后一帧候选帧（fallback_frame_before_seek_target_）兜底上屏，
     * 随后触发 SeekCompleted 与 MediaEnded 事件。
     */
    TickResult HandleEndOfStream(const EndOfStream& eos);

    /**
     * @brief Seek 定位期间对视频帧的筛选与截获算法
     * @param frame 待判定的解码帧
     * @details
     * - PTS < target_us：更新并保留为候选帧（fallback_frame_before_seek_target_），不送显；
     * - PTS >= target_us：作为命中目标帧直接交付上屏，触发 SeekCompleted 并开启 Seek 挂起锁。
     */
    void ProcessFrameDuringSeek(const VideoFrame& frame);

    /**
     * @brief 阻塞挂起节点工作线程，等待 Player 决策后调用 ReleaseSeekHold() 放行
     */
    void WaitForSeekRelease();

    // 显示分发与媒体世代
    std::shared_ptr<VideoSinkDispatcher> video_frame_dispatcher_{
        nullptr};  ///< 弱引用分发器，将帧安全投递给注册的 IVideoSink
    std::uint64_t active_media_epoch_{0};  ///< 本节点当前正在处理的媒体世代号
    std::atomic<bool> active_epoch_eos_processed_{false};  ///< 当前世代的 EOS 是否已被本节点消费处理

    // Seek 目标帧筛选状态机
    std::optional<VideoFrame>
        fallback_frame_before_seek_target_;  ///< 目标 PTS 之前紧邻的最后一帧候选帧（用于触底兜底）
    std::int64_t seek_target_pts_us_{0};  ///< 本次 Seek 检索的目标时间戳（微秒）
    bool is_searching_for_seek_target_frame_{false};  ///< 是否正处于 Seek 目标帧寻找阶段
    bool seek_result_frame_delivered_{false};  ///< 本次 Seek 对应的定位帧（目标帧或兜底帧）是否已交付上屏

    // Seek 首帧交付后的挂起同步机制
    bool is_waiting_for_player_seek_decision_{false};  ///< 是否正处于交付定位帧后的挂起等待阶段
    mutable std::mutex seek_hold_mutex_;  ///< 保护 seek_hold_release_requested_ 的互斥锁
    std::condition_variable seek_hold_cv_;  ///< 用于唤醒挂起工作线程的条件变量
    bool seek_hold_release_requested_{false};  ///< Player 是否已请求放行 Seek 挂起

    // 暂停/时钟同步期间暂存的延迟输入项
    std::optional<StreamItem<VideoFrame>>
        deferred_input_item_during_pause_;  ///< 挂起或暂停时从队列预读出但未处理的输入项
};

}  // namespace mediacore
/**
 * @file av_sync_node.h
 * @brief 音画同步实现
 * @details
 * 数据路径：VideoDecoder -> AvSyncNode -> VideoSinkNode -> 屏幕
 * AvSyncNode 读取 PlaybackTimeline 的媒体位置，以音频时钟为主时钟，比较视频 PTS
 * 如果没有音频时钟则切换为系统单调递增的时钟，保证视频正常速度播放
 * abs 是 absolute value（绝对值） 的缩写，也就是 C++ 里的 std::abs()
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "av_sync/playback_timeline.h"
#include "core/inode.h"
#include "core/stream_item.h"
#include "mediacore/av_types/video_frame.h"

namespace mediacore {

/**
 * @struct AvSyncStats
 * @brief 统计放行帧数量、吃到丢帧和时间差
 * @details 统计的是把帧交给下游时的状态，而不是屏幕实际显示时间或声学测量结果
 */
struct AvSyncStats {
    std::uint64_t presented_frames_{0};  ///< 正常播放时，放行的帧数
    std::uint64_t dropped_late_frames_{0};  ///< 正常播放时因迟到超过阈值时跳过的帧数
    std::int64_t average_abs_delta_us_{0};  ///< 决定放行时，视频 PTS 与媒体位置之差的绝对平均值
    std::int64_t max_abs_delta_us_{0};  ///< 决定放行的帧中，上述绝对时间差的最大值
    std::uint64_t audio_clock_fallback_count_{0};  ///< 正常调度中，观测到音频主时钟转为系统主时钟的次数
};

/**
 * @class AvSyncNode
 * @brief 按照 PlaybackTimeline 的全局时间轴进行音画同步
 * @details
 * Pipeline 创建本节点，基类启动工作线程来进行音画同步的处理，Tick 和待处理帧只有该线程访问
 * 节点拥有暂存帧和计数器，通过 shared_ptr 持有时间轴，不解码不参与音频、视频的播放
 * 外部线程可以 GetState 来获取相关的计数统计
 */
class AvSyncNode : public INode<StreamItem<VideoFrame>, StreamItem<VideoFrame>> {
public:
    /**
     * @brief 构造函数，创建音画同步节点
     * @param sync_ctx 全局同步上下文
     * @param event_callbacks 向 Pipeline 上报错误、结束等事件的回调
     * @param timeline 全局共享时间轴
     * @param frame_duration_us 根据视频帧率估算的一帧时长
     * @param drop_late_us 视频允许落后于主时间轴的时长，超过此阈值时丢弃视频帧
     * @throw std::invalid_argument 时间轴为空或时间参数不为正，基类还检查控制状态和回调
     * @note 构造不启动线程，由 Pipeline 连接上下游并统一启动
     */
    AvSyncNode(std::shared_ptr<SyncContext> sync_ctx,
               PipelineEventCallbacks event_callbacks,
               std::shared_ptr<PlaybackTimeline> timeline,
               std::int64_t frame_duration_us,
               std::int64_t drop_late_us);

    /**
     * @brief 析构函数
     */
    ~AvSyncNode();

    /**
     * @brief 节点拥有自己的线程，禁止拷贝、移动
     */
    AvSyncNode(const AvSyncNode&) = delete;
    AvSyncNode& operator=(const AvSyncNode&) = delete;
    AvSyncNode(AvSyncNode&&) = delete;
    AvSyncNode& operator=(AvSyncNode&&) = delete;

    /**
     * @brief 供控制线程或 UI 安全读取相关计数
     * @return 各个原子计数的结果
     */
    [[nodiscard]] AvSyncStats GetState() const;

protected:
    /**
     * @brief 在节点工作线程消费输入项，并根据播放状态调度视频输出。
     * @return kContinue 继续下一轮执行，kStopped 正常结束，kError 不可恢复性错误
     *
     * 输入可能是 VideoFrame、StreamReset 或 EndOfStream。
     *
     * VideoFrame：
     * - 正常播放：早到等待、轻微迟到放行、严重迟到丢帧。
     * - Seek 期间：不执行正常播放的等待和迟到丢帧规则；
     *   当前实现将新版本的视频帧交给 VideoSinkNode 筛选目标画面。
     * - 属于旧 epoch 的帧：直接丢弃，避免旧时间线的数据进入新播放版本。
     *
     * StreamReset：
     * - 清除旧时间线的 PTS 记录和 Seek 相关标志。
     * - 记录新的播放版本，并将重置通知继续发送给下游。
     *
     * EndOfStream：
     * - 正常播放时，如果最后一帧的预期显示时长尚未结束，则继续等待。
     * - 达到结束位置后发送 EOS，通知下游该视频流已经结束。
     * - Seek 期间不等待正常播放的末帧时长；如果目标帧不存在，
     *   必须由上层将该轨道的 EOS 作为 Seek 协调的终止条件处理。
     *
     * @note 本节点决定视频数据的调度时机，不直接执行文件 Seek，
     *       也不保证已经发送的画面实际显示到了屏幕上。
     */
    TickResult Tick() override;

private:
    std::shared_ptr<PlaybackTimeline> timeline_;  ///< 全局时间轴

    std::optional<StreamItem<VideoFrame>> pending_;  ///< 接收到数据副本
    std::optional<std::int64_t> last_pts_;  ///< 最近一次处理的有效 PTS

    const std::int64_t
        frame_duration_us_;  ///< 根据视频帧率估算的默认帧时长，用于补齐缺失的 PTS 和估算最后一帧的结束时间
    const std::int64_t drop_late_us_;  ///< 超时丢帧的阈值

    std::atomic<std::uint64_t> presented_frames_{0};  ///< 正常播放的放行帧数
    std::atomic<std::uint64_t> dropped_late_frames_{0};  ///<  因迟到而跳过的帧数
    std::atomic<std::int64_t> abs_delta_sum_us_{0};  ///< 正常放行时的绝对时间差之和
    std::atomic<std::int64_t> abs_delta_max_us_{0};  ///< 放行帧的最大绝对时间差
    std::atomic<std::uint64_t> audio_clock_fallback_count_{0};  ///< 音频作为主时钟不可用的次数

    bool was_using_audio_clock_{false};  ///< 上一轮调度是否通过音频作为主时钟
    bool seek_target_forwarded_{false};  ///< 本次 Seek 是否以送出首个 PTS 不早于目标的画面

    std::uint64_t seek_epoch_{0};  ///< Seek 世代号
};

}  // namespace mediacore

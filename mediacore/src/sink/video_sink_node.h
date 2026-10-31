
/**
 * @file video_sink_node.h
 * @brief 视频分支的输出节点，负责帧交付、Seek 预览帧选择和轨道状态报告
 *
 * 数据流：
 * VideoDecoderNode -> AvSyncNode -> Link -> VideoSinkNode-> VideoSinkDispatcher -> IVideoSink
 *
 * 正常播放时，帧的放行时机由 AvSyncNode 控制，本节点负责接收和交付
 * Seek 时，本节点寻找第一张 PTS 不早于目标时间的帧，并保留目标之前的最后一帧作为备用
 * 如果到达 EOF 仍未找到目标帧，则使用备用帧完成预览和就绪报告
 *
 * 帧交付和轨道就绪不代表外部显示端已经完成绘制
 * 本节点不拥有外部 IVideoSink；销毁节点前必须先停止 Pipeline 并等待工作线程退出
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "core/inode.h"
#include "core/stream_item.h"
#include "mediacore/av_types/video_frame.h"
#include "sink/video_sink_dispatcher.h"
#include "av_sync/playback_timeline.h"

namespace mediacore {

/**
 * @class VideoSinkNode
 * @brief 视频分支的最终消费者，负责帧交付和视频轨道状态报告
 *
 * 接收 VideoFrame、StreamReset 和 EndOfStream 三种数据
 * 正常播放时直接交付视频帧；Seek 时选择目标预览帧，并向 PlaybackTimeline 报告视频轨道就绪或结束状态
 *
 * 当前节点的帧处理状态由自身工作线程维护，视频轨道结束不代表整个媒体结束，仍需等待其他活动轨道完成
 */
class VideoSinkNode final : public INode<StreamItem<VideoFrame>, NoPort> {
public:
    /**
     * @brief 初始化视频输出节点
     * @param sync_context Pipeline 的运行状态和播放版本上下文
     * @param callbacks 节点向 Pipeline 上报就绪、结束和错误事件的回调
     * @param dispatcher 视频显示分发器
     * @param timeline 共享播放时间轴
     * @note 传入的共享对象必须有效，并在启动 Pipeline 前连接输入 Link
     */
    VideoSinkNode(std::shared_ptr<SyncContext> sync_context,
                  PipelineEventCallbacks callbacks,
                  std::shared_ptr<VideoSinkDispatcher> dispatcher,
                  std::shared_ptr<PlaybackTimeline> timeline);

protected:
    /**
     * @brief 处理一个输入数据项
     * @return 继续处理、停止或发生错误
     *
     * 输入等待采用限时方式暂停时保留已取出但尚未处理的数据，
     * 等待恢复后继续处理
     * Seek 视频轨道就绪后，等待其他活动轨道完成就绪
     */
    TickResult Tick() override;

private:
    /**
     * @brief 检查帧所属的播放版本，并交给显示分发器
     * @param frame 待交付的视频帧
     * @note 没有绑定显示端时，分发器可以丢弃帧
     */
    void DeliverFrame(VideoFrame frame);

    /**
     * @brief 报告视频轨道已经完成本轮 Seek 准备
     * @param position 选定预览帧的 PTS，单位微秒；无有效 PTS 时使用 Seek 目标时间
     * @param reached_eof 寻找目标帧时是否已经到达输入结尾
     */
    void ReportReady(std::int64_t position, bool reached_eof);

private:
    std::shared_ptr<VideoSinkDispatcher> video_dispatcher_;
    ///< 视频显示分发器，不拥有外部 IVideoSink

    std::shared_ptr<PlaybackTimeline> timeline_;
    ///< 共享播放时间轴，负责维护播放位置和各轨道的就绪状态

    std::uint64_t current_epoch_{0};
    ///< 当前处理的播放版本，用于过滤旧版本数据

    bool is_searching_{false};
    ///< 是否正在寻找 Seek 目标帧

    bool is_video_ready_{false};
    ///< 当前播放版本的视频轨道是否已经报告就绪

    bool is_eos_processed_{false};
    ///< 当前播放版本的 EndOfStream 是否已经处理

    std::int64_t seek_target_us_{0};
    ///< 当前 Seek 的目标时间，单位微秒

    std::optional<VideoFrame> fallback_frame_;
    ///< 目标时间之前最近的一帧；到达 EOF 仍未找到目标帧时用作预览

    std::optional<StreamItem<VideoFrame>> pending_item_;
    ///< 已从输入队列取出但尚未处理的数据项，避免暂停时丢失数据
};

}  // namespace mediacore

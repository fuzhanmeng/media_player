/**
 * @file video_sink_node.cpp
 * @brief 视频输出节点实现，负责帧交付、Seek 预览帧选择和轨道状态报告
 * @details
 * 正常播放的帧时序由 AvSyncNode 控制，本节点不再进行同步等待
 * Seek 时寻找第一张 PTS 不早于目标时间的帧，并保留目标之前最近的一帧作为备用
 */

#include "sink/video_sink_node.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

#include "core/sync_context.h"
#include "mediacore/av_types/video_frame.h"
#include "mediacore/ivideo_sink.h"
#include "mediacore/logger.h"

namespace mediacore {

/**
/**
 * @brief 初始化视频输出节点。
 * @param sync_context Pipeline 同步上下文
 * @param callbacks Pipeline 事件回调
 * @param video_dispatcher 视频帧分发器
 * @param timeline 共享播放时间轴
 * @throws std::invalid_argument 当 video_frame_dispatcher 为空时抛出
 */
VideoSinkNode::VideoSinkNode(std::shared_ptr<SyncContext> sync_ctx,
                             PipelineEventCallbacks event_callbacks,
                             std::shared_ptr<VideoSinkDispatcher> video_dispatcher,
                             std::shared_ptr<PlaybackTimeline> timeline)
    : INode("video_sink", std::move(sync_ctx), std::move(event_callbacks)),
      video_dispatcher_(std::move(video_dispatcher)),
      timeline_(std::move(timeline)) {
    if (video_dispatcher_ == nullptr || timeline_ == nullptr) {
        LOG_ERROR("VideoSinkNode requires a non-null VideoSinkDispatcher and PlaybackTimeline");
        throw std::invalid_argument("VideoSinkNode requires a non-null VideoSinkDispatcher and PlaybackTimeline");
    }
}

/**
 * @brief 将当前播放版本的视频帧交给显示端
 * @details
 * 交付前检查 epoch，丢弃 Seek 过程中失效的旧帧
 * 使用 shared_ptr 管理帧生命周期，允许显示端延迟使用
 */
void VideoSinkNode::DeliverFrame(VideoFrame frame) {
    if (IsCurrentEpoch(frame.epoch_) && timeline_->Get().epoch_ == frame.epoch_) {
        video_dispatcher_->DeliverFrame(std::make_shared<VideoFrame>(std::move(frame)));
    }
}

/**
 * @brief 登记视频轨道已经完成本轮 Seek，并在所有活动轨道就绪后上报聚合结果。
 * @param position 选定的视频帧位置，单位为微秒。
 * @param reached_eof 是否在输入结尾使用了目标之前的最后一帧完成定位。
 */
void VideoSinkNode::ReportReady(std::int64_t position, bool reached_eof) {
    if (is_video_ready_) {
        return;
    }

    is_video_ready_ = true;
    const auto result = timeline_->MarkReady(PlaybackTrack::kVideo, current_epoch_, position, reached_eof);

    LOG_DEBUG("Video sink reported seek readiness at epoch {}, position {} us, EOF {}",
              current_epoch_,
              position,
              reached_eof);

    if (result) {
        LOG_INFO("All tracks ready after seek at epoch {}, position {} us, all tracks at EOF {}",
                 current_epoch_,
                 result->position_us_,
                 result->all_eof_);
        ReportSeekCompleted(current_epoch_, result->position_us_, result->all_eof_);
    }
}

/**
 * @brief 消费视频输入并处理帧交付、Seek 和 EOF
 * @details
 * 输入类型：StreamReset / VideoFrame / EndOfStream，普通暂停时保留待处理数据；Seek 期间继续搜索预览帧
 */
TickResult VideoSinkNode::Tick() {
    const auto timeline_state = timeline_->Get();

    // 1. 如果 Seek 预览已经完成，等待其他轨道就绪
    if (current_epoch_ == timeline_state.epoch_ && is_video_ready_ && timeline_state.seeking_) {
        timeline_->Wait(timeline_state.state_version_);

        return TickResult::kContinue;
    }

    // 2. 获取输数据，没有处理完之前始终保留数据
    if (!pending_item_) {
        pending_item_ = ReceiveInputFor(std::chrono::milliseconds(5));
    }

    // 3. 如果没接收到数据
    if (!pending_item_) {
        return IsStopRequested() ? TickResult::kStopped : TickResult::kContinue;
    }

    // 4. 丢弃已经失效的旧版本数据
    const auto input_epoch = std::visit([](const auto& itme) { return itme.epoch_; }, *pending_item_);

    if (!IsCurrentEpoch(input_epoch) || input_epoch != timeline_->Get().epoch_) {
        pending_item_.reset();
        return TickResult::kContinue;
    }

    const auto current_state = timeline_->Get();

    // 普通暂停时不消费数据，Seek 期间允许继续处理
    if (!current_state.playing_ && !current_state.seeking_) {
        return TickResult::kContinue;
    }

    auto stream_item = std::move(*pending_item_);
    pending_item_.reset();

    // Seek 重置：切换播放版本并重新开始帧搜索
    if (const auto* reset = std::get_if<StreamReset>(&stream_item)) {
        current_epoch_ = reset->epoch_;
        seek_target_us_ = reset->target_us_;

        is_searching_ = true;
        is_video_ready_ = false;
        is_eos_processed_ = false;

        fallback_frame_.reset();
        return TickResult::kContinue;
    }

    // 视频帧处理
    if (auto* video_frame = std::get_if<VideoFrame>(&stream_item)) {
        if (input_epoch != current_epoch_) {
            return Fail("video frame before StreamReset", input_epoch);
        }

        if (is_searching_) {
            // 目标之前的帧仅作为备用，不交付显示
            const auto frame_pts = video_frame->pts_us_.value_or(seek_target_us_);

            if (frame_pts < seek_target_us_) {
                fallback_frame_ = std::move(*video_frame);
            } else {
                // 找到第一张满足目标时间的帧，完成预览
                const auto actual_position_us = frame_pts;

                DeliverFrame(std::move(*video_frame));
                is_searching_ = false;
                fallback_frame_.reset();
                ReportReady(actual_position_us, false);
            }
        } else if (!is_eos_processed_) {
            // 正常播放：直接交付，上游已完成同步调度
            DeliverFrame(std::move(*video_frame));
        }

        return TickResult::kContinue;
    }

    // 视频流结束处理
    if (const auto* end_of_stream = std::get_if<EndOfStream>(&stream_item)) {
        if (input_epoch != current_epoch_) {
            return Fail("video EOS before StreamReset", input_epoch);
        }

        // 防止重复处理 EOS
        if (is_eos_processed_) {
            return TickResult::kContinue;
        }

        is_eos_processed_ = true;

        if (is_searching_) {
            // 未找到目标帧时，使用备用帧完成 Seek 预览
            // 没有备用帧则直接使用目标时间报告就绪
            const auto actual_position_us =
                fallback_frame_ ? fallback_frame_->pts_us_.value_or(seek_target_us_) : seek_target_us_;

            if (fallback_frame_) {
                DeliverFrame(std::move(*fallback_frame_));
            }

            ReportReady(actual_position_us, true);
        }

        // 报告视频轨道结束，所有活动轨道结束后通知媒体结束
        if (timeline_->MarkEnded(PlaybackTrack::kVideo, current_epoch_)) {
            ReportMediaEnded(end_of_stream->epoch_);
        }

        return TickResult::kContinue;
    }

    return Fail("VideoSinkNode received an unknown stream item", input_epoch);
}

}  // namespace mediacore

/**
 * @file av_sync_node.cpp
 * @brief  实现音画同步
 * @details
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

#include "av_sync/av_sync_node.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <variant>

#include "mediacore/logger.h"

namespace mediacore {

namespace {
constexpr const char* kModuleName = "sv_sync_node";
}

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
AvSyncNode::AvSyncNode(std::shared_ptr<SyncContext> sync_ctx,
                       PipelineEventCallbacks event_callbacks,
                       std::shared_ptr<PlaybackTimeline> timeline,
                       std::int64_t frame_duration_us,
                       std::int64_t drop_late_us)
    : INode(kModuleName, std::move(sync_ctx), std::move(event_callbacks)),
      timeline_(std::move(timeline)),
      frame_duration_us_(frame_duration_us),
      drop_late_us_(drop_late_us) {
    if (!timeline_ || frame_duration_us <= 0 || drop_late_us <= 0) {
        throw std::invalid_argument("AvSyncNode: invalid timing configuration");
    }

    LOG_INFO("AVSync configured: nominal frame {} us, late drop threshold {} us", frame_duration_us_, drop_late_us_);
}

/**
 * @brief 析构函数，释放资源
 */
AvSyncNode::~AvSyncNode() {
    const auto stats = GetState();

    LOG_INFO("AVSync scheduled {}, droped {}, average/max scheduling error {}/{} us, clock fallback {}",
             stats.presented_frames_,
             stats.dropped_late_frames_,
             stats.average_abs_delta_us_,
             stats.max_abs_delta_us_,
             stats.audio_clock_fallback_count_);
}

/**
 * @brief 获取统计数据
 * @return 得到 AvSyncStats 中的统计数据
 */
AvSyncStats AvSyncNode::GetState() const {
    const auto count = presented_frames_.load();

    return {count,
            dropped_late_frames_.load(),
            count ? (abs_delta_sum_us_.load() / static_cast<std::int64_t>(count)) : 0,
            abs_delta_max_us_.load(),
            audio_clock_fallback_count_.load()};
}

/**
 * @brief 视频同步节点的单轮处理逻辑，主要负责实现音画同步
 * @details
 * 本函数由节点的工作线程反复调用，每次处理一个数据项
 * 核心任务
 * 1. 获取一个待处理的数据项，或继续处理之前暂存的 pending_
 * 2. 丢弃属于旧播放世代的数据
 * 3. 根据数据项类型进行处理
 * - StreamReset：重置视频时间
 * - VideoFram：早到等待、迟到放行、严重迟到丢帧
 * - EndOfStream：必要时等待最后一帧的预期显示时长结束
 * @return kContinue：继续进行下一轮；kStopped 正常停止；kError 不可恢复性错误
 */
TickResult AvSyncNode::Tick() {
    // 1. 第一阶段：处理 Seek

    // 获取当前播放控制状态，包括播放位置、Seek 状态和播放版本号
    const auto control = timeline_->Get();

    // 如果当前正在进行 Seek，并且目标画面已经发送到了下游，就暂时不取数据
    if (control.seeking_ && control.epoch_ == seek_epoch_ && seek_target_forwarded_) {
        timeline_->Wait(control.state_version_);
    }

    // 2. 如果pending_ 为空，取出一个待处理的数据项
    if (!pending_) {
        // 最多等待 5 ms 接收数据，避免没有输入时一直阻塞在这里
        pending_ = ReceiveInputFor(std::chrono::milliseconds(5));
    }

    // 如果暂时没有收到数据，就退出；否则返回工作循环，稍后再试
    if (!pending_) {
        return IsStopRequested() ? TickResult::kStopped : TickResult::kContinue;
    }

    // 3. 过滤旧版本的数据，StreamItem 中的没个数据都携带了 epoch，用于判断是否为本轮播放
    const auto epoch = std::visit([](const auto& item) { return item.epoch_; }, *pending_);

    // 重新读取时间轴状态，接收数据时，播放控制状态可能被改变
    auto state = timeline_->Get();

    if (!IsCurrentEpoch(epoch) || epoch != state.epoch_) {
        // 丢弃过期数据
        LOG_DEBUG("AVSync discarded stale item: item epoch {}, timeline epoch {}, current epoch",
                  epoch,
                  state.epoch_,
                  GetCurrentEpoch());

        // 丢弃数据
        pending_.reset();
        return TickResult::kContinue;
    }

    // 4. 开始处理 StreamItem

    // A：收到了 StreamReset
    if (auto* reset = std::get_if<StreamReset>(&*pending_)) {
        last_pts_.reset();
        was_using_audio_clock_ = false;
        seek_target_forwarded_ = false;
        seek_epoch_ = epoch;

        LOG_INFO("AVSync received StreamReset: epoch {}, target {} us", epoch, reset->target_us_);
    }

    // B：收到了 VideoFrame
    else if (auto* frame = std::get_if<VideoFrame>(&*pending_)) {
        // 1. 获取 PTS：有真实的 PTS 使用真实的 PTS，没有真实的 PTS 使用推算的 PTS
        if (!frame->pts_us_) {
            frame->pts_us_ = last_pts_ ? (*last_pts_ + frame_duration_us_) : state.media_position_us_;

            LOG_DEBUG("AVSync estimated missing PTS: epoch {}, PTS {} us, nominal frame {} us",
                      epoch,
                      *frame->pts_us_,
                      frame_duration_us_);
        }

        // B1：正常播放：早到等待、迟到放心、严重迟到直接丢帧

        // Seek 期间不执行这套正常播放调度规则
        if (!state.seeking_) {
            if (!state.playing_) {
                return TickResult::kContinue;
            }

            // 检查主时钟是否为音频时钟，音频停止推进时采用系统时钟，但是这里只记录时钟切换次数
            if (was_using_audio_clock_ && !state.is_audio_master_) {
                ++audio_clock_fallback_count_;

                LOG_WARN(
                    "AVSync audio clock ended/stalled;"
                    "using monotonic fallback, epoch {}",
                    epoch);
            }

            // 记录本轮观察到的主时钟状态，供下一轮判断是否发生切换
            was_using_audio_clock_ = state.is_audio_master_;

            // 计算视频帧 PTS 和主时钟的差值
            const auto delta = *frame->pts_us_ - state.media_position_us_;

            // 1. 视频帧来早了,保留 pending_，等待播放位置或时间轴状态发生变化,下一轮 Tick 再次检查同一帧
            if (delta > 0) {
                LOG_TRACE("AVSync waiting for video frame: epoch {}, PTS {} us, clock {} us, early {} us",
                          epoch,
                          *frame->pts_us_,
                          state.media_position_us_,
                          delta);

                return TickResult::kContinue;
            }

            // 当前帧已到达预期的时间或者已经迟到，无论是播放还是放行，都先记录其 PTS
            last_pts_ = frame->pts_us_;

            // 迟到过多：直接丢弃当前帧,这里只丢弃 pending_ 保存的视频帧
            if (delta < -drop_late_us_) {
                // 记录因迟到而跳过的帧数
                const auto dropped = dropped_late_frames_.fetch_add(1) + 1;

                LOG_DEBUG("AVSync dropped late frame: epoch {}, PTS {} us, clock {} us, late {} us, threshold {} us",
                          epoch,
                          *frame->pts_us_,
                          state.media_position_us_,
                          -delta,
                          drop_late_us_);

                if (dropped == 1 && dropped % 30 == 0) {
                    LOG_WARN("AVSync late video frames dropped: {}, latest lateness {} us, threshold {} us, epoch {}",
                             dropped,
                             -delta,
                             drop_late_us_,
                             epoch);
                }

                // 置空数据
                pending_.reset();
                return TickResult::kContinue;
            }

            // 当前帧没有超过迟到阈值，决定放行
            ++presented_frames_;

            // 记录放行时的绝对时间误差, delta 为正表示早到，为负表示迟到,统计时只关心误差大小
            const auto total_delta_sum_us = std::abs(delta);
            abs_delta_sum_us_.fetch_add(total_delta_sum_us);

            // 保存历史最大时间差，便于通过 GetState() 观察同步情况
            abs_delta_max_us_.store(std::max(abs_delta_max_us_.load(), total_delta_sum_us));

            LOG_TRACE("AVSync scheduled video frame: epoch {}, PTS {}, us, clock {} us, offset {} us",
                      epoch,
                      *frame->pts_us_,
                      state.media_position_us_,
                      delta);
        }

        // B2: 正在执行 Seek
        else {
            //  Seek 的目标是找到目标时间附近的画面，而不是按照正常播放时钟逐帧等待
            // 目标之前的帧仍然可以发送给下游，由 VideoSinkNode 根据 Seek 目标进行筛选
            last_pts_ = frame->pts_us_;

            // 找到第一帧 PTS 不早于目标位置的画面
            if (*frame->pts_us_ >= state.seek_target_us_) {
                seek_target_forwarded_ = true;

                LOG_INFO("AVSync selected seek target frame: epoch {}, PTS {} us, target {} us",
                         epoch,
                         *frame->pts_us_,
                         state.seek_target_us_);
            }
        }
    }

    // 情况C：收到了 EndOfStream
    else if (!state.seeking_ && last_pts_ && state.playing_ &&
             state.media_position_us_ < (*last_pts_ + frame_duration_us_)) {
        timeline_->Wait(state.state_version_);
        return TickResult::kContinue;
    }

    // 5. 将当前的数据项发送给下游
    auto item = std::move(*pending_);
    pending_.reset();

    // 检查 item 保存的是否为 EndOfStream
    const bool is_eos = std::holds_alternative<EndOfStream>(item);
    const bool sent = SendOutput(std::move(item));

    if (sent && is_eos) {
        // 转发成功
        LOG_INFO("AVSync forward EndOfStream: epoch {}", epoch);
    } else if (!sent) {
        // 转发失败
        LOG_WARN("AVSync failed to forward item downstream: epoch {}, stop requested {}", epoch, IsStopRequested());
    }

    return sent ? TickResult::kContinue : TickResult::kStopped;
}

}  // namespace mediacore

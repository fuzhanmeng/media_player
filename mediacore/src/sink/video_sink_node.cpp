/**
 * @file video_sink_node.cpp
 * @brief 视频输出渲染节点实现（按时序向显示端交付画面并处理 Seek Hold 与 EOS）
 * @details
 * 负责管线最终端数据的消费与交付：
 * 1. 帧分发与解耦：将解码后的 VideoFrame 包装为 std::shared_ptr 提交给 VideoSinkDispatcher，
 *    通过分发器弱引用解耦外部 UI 界面（如 Qt 窗口或原生视图）的生命周期；
 * 2. 精准 Seek 首帧截获与挂起机制（Seek Hold）：
 *    - 过滤目标时间点前的早到关键帧（Pre-roll frames），仅留最后一张作为兜底备选；
 *    - 第一张 PTS >= target 的目标帧交付上屏后，主动挂起节点工作线程（WaitForSeekRelease）；
 *    - 待 Player 控制线程完成状态流转判定并调用 ReleaseSeekHold() 后，方才放行后续帧读取；
 * 3. 流末尾兜底与事件汇报：在 Seek 撞到流末尾（EOF）时自动使用备选候选帧兜底呈现，
 *    并向 Pipeline 上报 SeekCompleted 与 MediaEnded 事件。
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

namespace {

/**
 * @brief 创建分发器并挂载指定的单一显示端，返回托管的 shared_ptr
 * @param sink 外部显示端实例指针（非拥有借用）
 */
std::shared_ptr<VideoSinkDispatcher> CreateAttachedDispatcher(IVideoSink* sink) {
    auto dispatcher = std::make_shared<VideoSinkDispatcher>();
    dispatcher->Attach(sink);
    return dispatcher;
}

constexpr const char* kNodeName = "video_sink";

}  // namespace

/**
 * @brief 构造函数：初始化视频输出节点并绑定分发器
 * @param sync_ctx 全局同步控制上下文
 * @param event_callbacks 节点事件回调集合
 * @param video_frame_dispatcher 负责向显示端分发帧的共享分发器
 * @throws std::invalid_argument 当 video_frame_dispatcher 为空时抛出
 */
VideoSinkNode::VideoSinkNode(std::shared_ptr<SyncContext> sync_ctx,
                             PipelineEventCallbacks event_callbacks,
                             std::shared_ptr<VideoSinkDispatcher> video_frame_dispatcher)
    : INode(kNodeName, std::move(sync_ctx), std::move(event_callbacks)),
      video_frame_dispatcher_(std::move(video_frame_dispatcher)) {
    if (video_frame_dispatcher_ == nullptr) {
        LOG_ERROR("VideoSinkNode requires a non-null VideoSinkDispatcher");
        throw std::invalid_argument("VideoSinkNode requires a non-null VideoSinkDispatcher");
    }

    active_media_epoch_ = GetCurrentEpoch();
    LOG_INFO("VideoSinkNode constructed, media epoch {}", active_media_epoch_);
}

/**
 * @brief 便利构造函数：基于已有 IVideoSink 指针委托构造
 */
VideoSinkNode::VideoSinkNode(std::shared_ptr<SyncContext> sync_ctx,
                             PipelineEventCallbacks event_callbacks,
                             IVideoSink* sink)
    : VideoSinkNode(std::move(sync_ctx), std::move(event_callbacks), CreateAttachedDispatcher(sink)) {}

/**
 * @brief 解除 Seek 定位期间的帧读取挂起阻塞（实现 ISeekHoldReleasable 接口）
 * @details
 * 目标帧交付上屏后，节点会停止后续读取并阻塞挂起；
 * Player 控制线程在处理完 SeekCompleted 并确定是继续播放还是保持定格暂停后，
 * 调用本函数解除挂起。
 */
void VideoSinkNode::ReleaseSeekHold() noexcept {
    {
        std::lock_guard<std::mutex> lock(seek_hold_mutex_);
        seek_hold_release_requested_ = true;
    }

    seek_hold_cv_.notify_all();
    LOG_DEBUG("VideoSinkNode allowed processing after seek target");
}

/**
 * @brief 从输入端口消费并分发处理一个 VideoFrame、StreamReset 或 EndOfStream
 */
TickResult VideoSinkNode::Tick() {
    // 1. 若此前已交付 Seek 定位帧，阻塞等待 Player 决策放行
    if (is_waiting_for_player_seek_decision_) {
        WaitForSeekRelease();
    }

    // 2. 优先检查是否存在暂停期间已出队但暂存未处理的输入项；若无暂存项，才从上游队列读取
    if (!deferred_input_item_during_pause_.has_value()) {
        deferred_input_item_during_pause_ = ReceiveInput();

        // 返回 nullopt 表示输入 Link 被打断（Abort）或已关闭，正常 EOF 会通过 EndOfStream 变体项传递
        if (!deferred_input_item_during_pause_.has_value()) {
            active_epoch_eos_processed_.store(false, std::memory_order_release);
            return TickResult::kStopped;
        }
    }

    // 3. 消费前校验运行状态：若当前管线处于暂停（Paused），保留该项不予处理，等待唤醒
    if (sync_ctx_->GetState() == PipelineRunState::kPaused) {
        return TickResult::kContinue;
    }

    StreamItem<VideoFrame> item = std::move(*deferred_input_item_during_pause_);
    deferred_input_item_during_pause_.reset();

    // 4. 分发处理视频帧
    if (auto* frame = std::get_if<VideoFrame>(&item)) {
        if (!IsCurrentEpoch(frame->epoch_)) {
            return TickResult::kContinue;
        }
        return HandleVideoFrame(std::move(*frame));
    }

    // 5. 分发处理流重置信令（StreamReset）
    if (const auto* reset = std::get_if<StreamReset>(&item)) {
        return HandleStreamReset(*reset);
    }

    // 6. 分发处理流结束信令（EndOfStream）
    if (const auto* eos = std::get_if<EndOfStream>(&item)) {
        return HandleEndOfStream(*eos);
    }

    return Fail("VideoSinkNode received an unknown stream item");
}

/**
 * @brief 挂起阻塞当前工作线程，直到 Player 批准放行或触发停机/新 Seek
 */
void VideoSinkNode::WaitForSeekRelease() {
    std::unique_lock<std::mutex> lock(seek_hold_mutex_);

    seek_hold_cv_.wait(lock, [this]() {
        return seek_hold_release_requested_ || IsStopRequested() || !IsCurrentEpoch(active_media_epoch_);
    });

    is_waiting_for_player_seek_decision_ = false;
    seek_hold_release_requested_ = false;
}

/**
 * @brief 处理常规视频图像帧
 * @param frame 待渲染呈现的视频帧对象
 */
TickResult VideoSinkNode::HandleVideoFrame(VideoFrame frame) {
    if (IsStopRequested()) {
        return TickResult::kStopped;
    }

    // 取出帧后若管线恰好转入暂停，将帧重新暂存，等恢复后再上屏
    if (sync_ctx_->GetState() == PipelineRunState::kPaused) {
        deferred_input_item_during_pause_.emplace(std::in_place_type<VideoFrame>, std::move(frame));
        return TickResult::kContinue;
    }

    // 丢弃不属于当前全局活跃世代的过期残留帧
    if (!IsCurrentEpoch(frame.epoch_)) {
        return TickResult::kContinue;
    }

    // 处于 Seek 检索阶段时，交由专用检索逻辑进行时间戳匹配
    if (is_searching_for_seek_target_frame_) {
        ProcessFrameDuringSeek(frame);
        return TickResult::kContinue;
    }

    // 正常播放流程：通过分发器安全投递上屏
    video_frame_dispatcher_->DeliverFrame(std::make_shared<VideoFrame>(std::move(frame)));
    return TickResult::kContinue;
}

/**
 * @brief Seek 寻帧状态机核心处理：筛选目标帧并建立兜底备份
 * @param frame 待检索判定的视频帧
 */
void VideoSinkNode::ProcessFrameDuringSeek(const VideoFrame& frame) {
    if (!frame.pts_us_.has_value()) {
        return;
    }

    // 尚未达到目标时间：仅作为候选帧备份保留，不直接上屏，防止画面闪烁倒退
    if (frame.pts_us_.value() < seek_target_pts_us_) {
        fallback_frame_before_seek_target_ = frame;
        return;
    }

    // 命中第一张 PTS >= seek_target_pts_us_ 的精准目标帧，立即交付上屏呈现
    video_frame_dispatcher_->DeliverFrame(std::make_shared<VideoFrame>(frame));

    // 首帧交付完成后立即进入 Seek Hold 挂起等待状态，等待 Player 决策
    {
        std::lock_guard<std::mutex> lock(seek_hold_mutex_);
        is_waiting_for_player_seek_decision_ = true;
        seek_hold_release_requested_ = false;
    }

    is_searching_for_seek_target_frame_ = false;
    seek_result_frame_delivered_ = true;

    LOG_INFO(
        "VideoSinkNode delivered seek target frame at pts {} us, epoch {}", frame.pts_us_.value(), active_media_epoch_);

    // 向上层汇报 Seek 顺利完成（精确匹配到目标帧）
    ReportSeekCompleted(active_media_epoch_, frame.pts_us_.value(), false);
}

/**
 * @brief 处理时间跳转（Seek）后的流重置信令
 * @param reset 包含新世代 Epoch 与目标时间戳的控制载荷
 */
TickResult VideoSinkNode::HandleStreamReset(const StreamReset& reset) {
    if (!IsCurrentEpoch(reset.epoch_)) {
        return TickResult::kContinue;
    }

    // 同步更新活跃世代与重置 EOS 处理标记
    active_media_epoch_ = reset.epoch_;
    active_epoch_eos_processed_.store(false, std::memory_order_release);

    // 开启新一轮 Seek 目标帧检索
    is_searching_for_seek_target_frame_ = true;
    seek_target_pts_us_ = reset.target_us_;
    seek_result_frame_delivered_ = false;
    is_waiting_for_player_seek_decision_ = false;
    fallback_frame_before_seek_target_.reset();

    {
        std::lock_guard<std::mutex> lock(seek_hold_mutex_);
        seek_hold_release_requested_ = false;
    }

    LOG_INFO("VideoSinkNode reset to epoch {}, seek target us is {}", reset.epoch_, reset.target_us_);
    return TickResult::kContinue;
}

/**
 * @brief 处理流结束信令（EndOfStream）并汇报播放结束事件
 * @param eos 包含流结束世代号的控制载荷
 */
TickResult VideoSinkNode::HandleEndOfStream(const EndOfStream& eos) {
    if (!IsCurrentEpoch(eos.epoch_)) {
        return TickResult::kContinue;
    }

    if (eos.epoch_ != active_media_epoch_) {
        return Fail("VideoSinkNode received a new epoch EOS before StreamReset");
    }

    // 极端场景：Seek 检索期间直接遭遇流末尾（如 Seek 到视频最后空白处）
    if (is_searching_for_seek_target_frame_ && !seek_result_frame_delivered_) {
        if (fallback_frame_before_seek_target_.has_value()) {
            // 将目标时间前距离最近的最后一帧候选帧兜底交付上屏
            video_frame_dispatcher_->DeliverFrame(std::make_shared<VideoFrame>(*fallback_frame_before_seek_target_));

            {
                std::lock_guard<std::mutex> lock(seek_hold_mutex_);
                is_waiting_for_player_seek_decision_ = true;
                seek_hold_release_requested_ = false;
            }

            is_searching_for_seek_target_frame_ = false;
            seek_result_frame_delivered_ = true;

            const std::int64_t fallback_pts = fallback_frame_before_seek_target_->pts_us_.value_or(0);
            LOG_INFO("VideoSinkNode seek hit EOF, using last preroll frame at pts {} us", fallback_pts);

            // 汇报 SeekCompleted，标记 reached_eof 为 true
            ReportSeekCompleted(active_media_epoch_, fallback_pts, true);
        } else {
            // 连一张备用帧都没有就已触底，视为媒体源数据异常
            is_searching_for_seek_target_frame_ = false;
            return Fail("Seek reached EOS with no candidate frame to display");
        }
    }

    // 标记当前世代 EOS 已消费处理完成，并上报全局 MediaEnded 事件
    active_epoch_eos_processed_.store(true, std::memory_order_release);
    LOG_INFO("VideoSinkNode reached end of stream, epoch {}", eos.epoch_);
    ReportMediaEnded(eos.epoch_);

    return TickResult::kContinue;
}

/**
 * @brief 外部中断触发钩子：响应停止通知，唤醒处于 Seek Hold 挂起的工作线程
 */
void VideoSinkNode::OnInterruptBlockingIo() noexcept {
    LOG_DEBUG("VideoSinkNode interrupting blocking I/O");
    seek_hold_cv_.notify_all();
}

}  // namespace mediacore
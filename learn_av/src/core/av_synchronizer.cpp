/**
 * @file av_synchronizer.cpp
 * @brief 音画同步器核心实现
 * @details 采用"音频为主时钟"的经典同步策略，
 *
 * 同步策略：
 * 1. 视频帧比音频慢超过阈值：直接丢弃过期帧
 * 2. 视频帧比音频快超过阈值：精确休眠等待
 * 3. 时间差在阈值内：立即渲染
 */

#include "av_synchronizer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

#include "av_core_def.h"
#include "interface/iaudio_speaker.h"
#include "logger.h"
#include "output/sdl_audio_speaker.h"
#include "playback_sync.h"
#include "utils/thread_safe_queue.h"
#include "video_frame.h"

namespace av {

/**
 * @brief 创建音画同步器实例
 * @param sync_ctx 全局状态同步上下文（播放/暂停/跳转状态的唯一来源）
 * @param audio_speaker 音频扬声器接口指针，用于获取物理音频时钟
 * @return 音画同步器智能指针
 */
std::unique_ptr<IAVSynchronizer> IAVSynchronizer::Create(std::shared_ptr<SyncContext> sync_ctx,
                                                         IAudioSpeaker* audio_speaker) {
    return std::make_unique<AVSynchronizer>(std::move(sync_ctx), audio_speaker);
}

/**
 * @brief 构造函数
 * @param sync_ctx 全局状态同步上下文
 * @param audio_speaker 音频扬声器接口指针
 */
AVSynchronizer::AVSynchronizer(std::shared_ptr<SyncContext> sync_ctx, IAudioSpeaker* audio_speaker)
    : sync_ctx_(std::move(sync_ctx)), audio_speaker_(audio_speaker) {}

/**
 * @brief 析构函数
 * @details 严格遵循RAII原则，自动停止后台线程并释放资源
 */
AVSynchronizer::~AVSynchronizer() {
    RequestStop();
    WaitForStop();
}

/**
 * @brief 注册下游监听器
 * @param listener 接收同步完成视频帧的目标对象（Player）
 * @note 线程安全，由listener_mutex_保护
 */
void AVSynchronizer::SetListener(Listener* listener) {
    std::lock_guard<std::mutex> lock(listener_mutex_);
    listener_ = listener;
}

/**
 * @brief 音画同步启动
 * @details 激活内部安全队列并拉起后台同步线程
 * @note 线程安全，重复调用会被拦截
 */
void AVSynchronizer::Start() {
    // 防止重复启动导致的线程句柄覆盖
    if (sync_thread_.joinable()) {
        LOG_WARN("AVSynchronizer thread is already running, Start() ignored");
        return;
    }

    // 重置终止标志位
    is_abort_.store(false);
    force_render_.store(false);

    // 每次启动时重置日志节流时间，防止跨 Play 周期的 static 变量问题
    last_render_log_time_ = std::chrono::steady_clock::now();

    // 激活队列
    video_queue_.Start();

    // 启动后台同步线程
    sync_thread_ = std::thread(&AVSynchronizer::ThreadLoop, this);

    LOG_INFO("AVSynchronizer working thread started successfully");
}

/**
 * @brief 阶段一：下发异步终止信令 -- 非阻塞
 * @details 发送终止信号，唤醒所有可能阻塞的操作
 */
void AVSynchronizer::RequestStop() {
    // 防止重复发送终止信号
    if (is_abort_.load()) {
        return;
    }

    // 设置终止标志位
    is_abort_.store(true);

    // 唤醒可能阻塞在状态条件变量上的线程
    if (sync_ctx_) {
        sync_ctx_->state_cv_.notify_all();
    }

    // 中止视频队列，解除所有阻塞在Pop/Push操作上的线程
    video_queue_.Abort();
    LOG_INFO("----- AVSynchronizer::RequestStop -----");
}

/**
 * @brief 阶段二：同步等待线程回收并清理资源（阻塞）
 */
void AVSynchronizer::WaitForStop() {
    // 1. 等待后台线程完全退出
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }

    // 2. 清空队列中残留的视频帧，释放内存
    video_queue_.Clear();

    LOG_INFO("----- AVSynchronizer::WaitForStop -----");
}

/**
 * @brief 清空队列内部缓存
 */
void AVSynchronizer::Flush() {
    video_queue_.Clear();

    LOG_INFO("AVSynchronizer Flush");
}

/**
 * @brief 清空队列，唤醒阻塞
 */
void AVSynchronizer::AbortAndClearQueue() {
    // AbortAndClear = 先设 is_abort_=true 唤醒阻塞线程，再清空数据
    video_queue_.AbortAndClear();
}
/**
 * @brief 启动队列
 */
void AVSynchronizer::StartQueue() { video_queue_.Start(); }

/**
 * @brief 接收上游解码器输出的视频帧
 * @param video_frame 解码完成的纯净YUV视频帧，或者是带内控制信令
 * @note VideoDecoder->FileReader->Player->AV_Sync->VideoSink->OpenglView
 */
void AVSynchronizer::PushVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    // 1. 判断 video_frame
    if (!video_frame) {
        LOG_WARN("Received null video frame, dropped");
        return;
    }

    // 2. 处理带内 Flush 信令
    if (video_frame->HasFlag(av::AVFrameFlag::kFlush)) {
        LOG_DEBUG("AV_Sync receive IN-BAND Flush signal");

        // 1. 清空视频队列
        video_queue_.Clear();

        // 2. 确何在暂停态 Seek 后，新到达的第一帧能够立刻渲染上屏
        force_render_.store(true);

        // 3. 信令已达，解除全局的 Seek 锁定期，允许后续画面正常进入同步环节
        if (sync_ctx_) {
            sync_ctx_->state_cv_.notify_all(); // 唤醒沉睡的工作线程
        }

        return;
    }

    // 2. 处理带内EOF信令
    if (video_frame->HasFlag(AVFrameFlag::kEOF)) {
        LOG_INFO("AV_Sync received IN-BAND EOF signal, pushing to render queue.");
        video_queue_.Push(std::move(video_frame));
        return;
    }

    // 4. 普通视频帧的合法性校验
    if (!video_frame->IsValid()) {
        LOG_WARN("Received invalid video frame, dropped");
        return;
    }

    // 4. 将合法数据帧压入队列，队列满时会自动阻塞上游解码线程
    video_queue_.Push(std::move(video_frame));
}

/**
 * @brief 音画同步核心工作线程
 * @details 死循环执行 "取帧-比对时钟-同步-渲染" 流程
 */
void AVSynchronizer::ThreadLoop() {
    while (!is_abort_.load()) {
        //  阶段1：全局状态判断
        {
            std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
            sync_ctx_->state_cv_.wait(lock, [this]() {
                // 唤醒条件：
                // 1. 收到终止请求
                // 2. 播放器处于停止状态
                // 3. 暂停状态下且强制单帧渲染
                // 4. 播放器处于正常播放状态且不在跳转中
                return is_abort_.load() || sync_ctx_->state_.load() == PlaybackState::kStopped ||
                    (sync_ctx_->state_.load() == PlaybackState::kPaused && force_render_.load()) ||
                    (!sync_ctx_->is_seeking_.load() && sync_ctx_->state_.load() == PlaybackState::kPlaying);
            });
        }

        // 唤醒后立即检查终止状态
        if (is_abort_.load() || sync_ctx_->state_.load() == PlaybackState::kStopped) {
            break;
        }

        // 阶段2：从队列中提取视频帧 - Pop是阻塞提取的
        std::shared_ptr<VideoFrame> video_frame;
if (!video_queue_.Pop(video_frame)) {
    // 队列 Abort 是 Seek 时的预期行为，不是错误，降级为 DEBUG
    LOG_DEBUG("Video queue aborted (seek in progress), returning to wait.");
    continue;
}
        // 阶段3：处理EOF帧
        if (video_frame->HasFlag(AVFrameFlag::kEOF)) {
            LOG_INFO("AVSynchronizer received EOF, notifying Player.");
            {
                std::lock_guard<std::mutex> lock(listener_mutex_);
                if (listener_)
                    listener_->OnSyncCompleted();
            }
            continue; // 明确跳过后续处理，不渲染空帧
        }

        if (!video_frame || !video_frame->IsValid()) {
            LOG_ERROR("Video Frame is Bad");
            continue;
        }

        //  阶段4：暂停态强制渲染逻辑 (Seek强刷屏)  仅在暂停状态下，且拿到刚跳转后的第一张有效帧时触发
        if (sync_ctx_->state_.load() == PlaybackState::kPaused && force_render_.load()) {
            LOG_INFO("Seek Completed: Force rendering new frame at {:.3f} sec", video_frame->GetTimestampSec());

            std::unique_lock<std::mutex> lock(listener_mutex_);
            if (listener_) {
                listener_->OnSyncNotifyVideoFrame(std::move(video_frame));
            }

            force_render_.store(false);
            continue;
        }

        //  阶段5： 不进行音画同步，纯视频播放
        if (!audio_speaker_) {
            std::unique_lock<std::mutex> lock(listener_mutex_);
            if (listener_) {
                listener_->OnSyncNotifyVideoFrame(std::move(video_frame));
            }
            continue;
        }

        //  阶段6：音画同步核心逻辑
        while (!is_abort_.load() && !sync_ctx_->is_seeking_.load() &&
               sync_ctx_->state_.load() == PlaybackState::kPlaying) {
            int64_t video_pts_us = video_frame->GetTimestampUs();
            int64_t audio_clock_us = audio_speaker_->GetAudioClockUs();

            if (audio_clock_us < 0) {
                audio_clock_us = video_pts_us;
            }

            // 每秒打印一次同步状态日志
            // 使用成员变量 last_render_log_time_ 替代原来的 static 变量
            auto log_now = std::chrono::steady_clock::now();
            if (log_now - last_render_log_time_ >= std::chrono::seconds(1)) {
                last_render_log_time_ = log_now;
                LOG_INFO("[AV Sync] video={:.3f}s  audio={:.3f}s  diff={:+.1f}ms",
                         static_cast<double>(video_pts_us) / 1000000.0, static_cast<double>(audio_clock_us) / 1000000.0,
                         static_cast<double>(audio_clock_us - video_pts_us) / 1000.0);
            }

            int64_t time_diff_us = audio_clock_us - video_pts_us;

            // 异常检测：差值超过 5s（改为 5s，原来 1s 太严格）
            // seek 后视频可能领先音频若干百毫秒，1s 会误触发
            if (time_diff_us > 5000000LL && !sync_ctx_->is_seeking_.load()) {
                LOG_WARN("[AV Sync] Anomaly >5s: video={:.3f}s audio={:.3f}s, dropping frame.",
                         static_cast<double>(video_pts_us) / 1000000.0,
                         static_cast<double>(audio_clock_us) / 1000000.0);
                break;
            }

            // 策略A：视频落后，丢帧追赶
            if (time_diff_us > kSyncThresholdUs) {
                LOG_DEBUG("[AV Sync] Drop late frame: pts={:.3f}s, behind by {:.1f}ms",
                          static_cast<double>(video_pts_us) / 1000000.0, static_cast<double>(time_diff_us) / 1000.0);
                break;
            }

            // 策略B：视频超前，休眠等待
            if (time_diff_us < -kSyncThresholdUs) {
                std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
                sync_ctx_->state_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                    return is_abort_.load() || sync_ctx_->is_seeking_.load() ||
                        sync_ctx_->state_.load() != PlaybackState::kPlaying;
                });
                continue;
            }

            // 策略C：在阈值范围内，渲染
            {
                std::unique_lock<std::mutex> lock(listener_mutex_);
                if (listener_) {
                    listener_->OnSyncNotifyVideoFrame(std::move(video_frame));
                }
            }
            break;
        }
    }

    LOG_INFO("AVSynchronizer core thread exited gracefully");
}

} // namespace av
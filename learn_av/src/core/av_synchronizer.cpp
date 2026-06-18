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
#include "output/interface/iaudio_speaker.h"
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
    if (!is_abort_.load()) {
        return;
    }

    // 2. 回收上次可能遗留的线程句柄
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }

    // 3.设置线程标志
    is_abort_.store(false);
    force_render_.store(false);
    audio_finished_.store(false);

    // 视频主时钟复位（无音频轨道时使用）
    video_clock_us_.store(-1);
    reset_video_clock_.store(true);

    // 每次启动时重置日志节流时间，防止跨 Play 周期的 static 变量问题
    last_render_log_time_ = std::chrono::steady_clock::now();

    // 4. 清空&&启动队列
    video_queue_.Clear();
    video_queue_.Start();

    // 5. 启动后台同步线程
    sync_thread_ = std::thread(&AVSynchronizer::ThreadLoop, this);

    LOG_INFO("AVSynchronizer working thread started successfully");
}

/**
 * @brief 阶段一：下发异步终止信令 -- 非阻塞
 * @details 发送终止信号，唤醒所有可能阻塞的操作
 */
void AVSynchronizer::RequestStop() {

    // 1. 下发终止信令
    is_abort_.store(true);

    // 2. 唤醒阻塞在 state_cv_ 上的线程
    if (sync_ctx_) {
        sync_ctx_->state_cv_.notify_all();
    }

    // 3.  唤醒阻塞在 video_queue_ 上的线程
    video_queue_.Abort();
}

/**
 * @brief 阶段二：同步等待线程回收并清理资源（阻塞）
 */
void AVSynchronizer::WaitForStop() {
    // 1. 等待后台线程完全退出
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
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

        // 2.1 视频主时钟基准失效，需要在下一帧重新建立（无音频轨道场景）
        reset_video_clock_.store(true);
        video_clock_us_.store(-1);

        // 3. 信令已达，解除全局的 Seek 锁定期，允许后续画面正常进入同步环节
        if (sync_ctx_) {
            sync_ctx_->state_cv_.notify_all(); // 唤醒沉睡的工作线程
        }

        return;
    }

    // 2. 处理带内EOF信令
    if (video_frame->HasFlag(AVFrameFlag::kEOF)) {
        LOG_INFO("AV_Sync received IN-BAND EOF signal, pushing to render queue.");
        video_queue_.Start();   // EOF 帧入队前确保队列处于可用状态
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
 * @brief 通知音画同步器：音频流已播放完毕
 * @details 由 Player::OnAudioCompleted 调用
 *          AVSync 内部据此区分"SDL 过渡期"和"音频永久停止"两种情况
 */
void AVSynchronizer::NotifyAudioFinished() {
    audio_finished_.store(true);
    sync_ctx_->state_cv_.notify_all();
}

/**
 * @brief 告知同步器当前媒体是否含有音频轨道
 */
void AVSynchronizer::SetHasAudio(bool has_audio) { has_audio_.store(has_audio); }

/**
 * @brief 获取视频主时钟（最近渲染帧 PTS，微秒）
 */
int64_t AVSynchronizer::GetVideoClockUs() const { return video_clock_us_.load(); }

/**
 * @brief 音画同步核心工作线程
 *
 * 整体流程：
 *   外层 while → 阶段1 wait（kPaused/is_seeking 时挂起，节省 CPU）
 *              → 阶段2 Pop 一帧（阻塞等待，Seek 时 Abort 立刻返回）
 *              → 阶段3 控制信令处理（EOF 通知播放完成）
 *              → 阶段4 暂停态强制渲染（Seek 完成后首帧刷屏）
 *              → 阶段5 纯视频播放（无音频轨道，直接渲染）
 *              → 阶段6 音画同步（内层 while 对同一帧反复评估时机）
 *
 * 音画同步策略（以音频时钟为主，视频跟随）：
 *   diff = audio_clock - video_pts
 *   diff > +25ms : 视频落后，策略A丢帧追赶
 *   diff < -25ms : 视频超前，策略B休眠等音频追上
 *   |diff| ≤ 25ms: 在容忍窗口内，策略C立即渲染
 *
 * 内层 while 的语义：
 *   continue = 重新评估"同一帧"（策略B：等音频追上再渲染）
 *   break    = 结束对当前帧的处理（策略A丢帧 / 策略C渲染完 / 音频停止）
 *              break 后回到外层 while，阶段2 Pop 取下一帧
 */
void AVSynchronizer::ThreadLoop() {

    if (!sync_ctx_) {
        return;
    }

    while (!is_abort_.load()) {
        // 阶段1：线程调度
        // 在 kPaused 或 is_seeking 时挂起，避免空转浪费 CPU
        {
            std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
            sync_ctx_->state_cv_.wait(lock, [this]() {
                return is_abort_.load() ||
                    (sync_ctx_->state_.load() == PlaybackState::kPaused && force_render_.load()) ||
                    (!sync_ctx_->is_seeking_.load() && sync_ctx_->state_.load() == PlaybackState::kPlaying);
            });
        }

        // 唤醒后先确认生命周期
        if (is_abort_.load()) {
            break;
        }

        // 阶段2：阻塞式取帧，Pop 阻塞直到队列有帧或队列被 Abort

        std::shared_ptr<VideoFrame> video_frame;
        if (!video_queue_.Pop(video_frame)) {
            LOG_DEBUG("Video queue aborted (seek in progress), returning to wait.");
            continue;
        }

        // 阶段3：带内控制信令处理

        // EOF 信令：视频解码器已排空所有帧
        // 通知 Player 视频已结束，然后 continue 回到阶段1
        if (video_frame->HasFlag(AVFrameFlag::kEOF)) {
            LOG_INFO("AVSynchronizer received EOF, notifying Player.");
            {
                std::lock_guard<std::mutex> lock(listener_mutex_);
                if (listener_) {
                    listener_->OnSyncCompleted();
                }
            }
            continue;
        }

        // 合法性校验：VideoFrame(0,0) 或未初始化的帧，直接丢弃
        if (!video_frame || !video_frame->IsValid()) {
            LOG_WARN("Received invalid video frame, dropped.");
            continue;
        }

        // 阶段4：暂停态强制渲染（Seek 完成后刷新画面）
        // 正常 kPaused 时线程在阶段1挂起，能走到这里说明 force_render_=true
        // 此时跳过音画同步，直接上屏，让用户看到跳转后的位置
        // 渲染完成后 force_render_ 置 false，下次 Pause 时不再触发

        if (sync_ctx_->state_.load() == PlaybackState::kPaused && force_render_.load()) {
            LOG_INFO("Seek Completed: Force rendering new frame at {:.3f} sec", video_frame->GetTimestampSec());
            video_clock_us_.store(video_frame->GetTimestampUs());
            {
                std::unique_lock<std::mutex> lock(listener_mutex_);
                if (listener_) {
                    listener_->OnSyncNotifyVideoFrame(std::move(video_frame));
                }
            }
            force_render_.store(false);
            continue;
        }

        // 阶段5：视频主时钟定时渲染（无音频轨道）
        // audio_speaker_ 为空，或当前媒体没有音频流（has_audio_=false）时，
        // 不能再以音频时钟为基准（音频时钟恒为 -1 会导致画面以解码速度狂飙），
        // 改用系统墙钟 + 视频 PTS 自驱定时，保证正常速率播放，并维护 video_clock_us_ 供进度条读取。

        if (!audio_speaker_ || !has_audio_.load()) {
            int64_t video_pts_us = video_frame->GetTimestampUs();

            // 建立/重置基准：首帧（或 Start/Seek 后）记录"首帧 PTS"与"当前墙钟"
            if (reset_video_clock_.exchange(false)) {
                video_master_first_pts_us_ = video_pts_us;
                video_master_start_time_ = std::chrono::steady_clock::now();
            }

            // 目标墙钟 = 基准墙钟 + (当前帧 PTS - 首帧 PTS)
            auto target_time =
                video_master_start_time_ + std::chrono::microseconds(video_pts_us - video_master_first_pts_us_);

            // 可被 暂停/跳转/停止 立即中断的等待，直到到达该帧的呈现时刻
            while (!is_abort_.load() && !sync_ctx_->is_seeking_.load() &&
                   sync_ctx_->state_.load() == PlaybackState::kPlaying) {
                auto now = std::chrono::steady_clock::now();
                if (now >= target_time) {
                    break;
                }
                std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
                sync_ctx_->state_cv_.wait_for(lock, target_time - now, [this]() {
                    return is_abort_.load() || sync_ctx_->is_seeking_.load() ||
                        sync_ctx_->state_.load() != PlaybackState::kPlaying;
                });
            }

            // 被 Seek/暂停 打断则丢弃本帧，回外层重新调度
            if (is_abort_.load() || sync_ctx_->is_seeking_.load() ||
                sync_ctx_->state_.load() != PlaybackState::kPlaying) {
                continue;
            }

            video_clock_us_.store(video_pts_us);
            {
                std::unique_lock<std::mutex> lock(listener_mutex_);
                if (listener_) {
                    listener_->OnSyncNotifyVideoFrame(std::move(video_frame));
                }
            }
            continue;
        }

        // 阶段6：音画同步核心逻辑
        //
        // 内层 while 对同一帧反复评估时间差：
        //   每次 continue 都是重新评估同一帧（等待音频时钟推进）
        //   每次 break   都是结束当前帧的处理（丢弃/渲染完成/音频停止）
        //
        // audio_clock_us = -1 的两种情况需要区分：
        //   情况A: 播放刚开始/Seek 后的短暂过渡期，SDL 还没消费第一帧
        //          特征：audio_finished_=false
        //          处理：降级用视频 PTS 代替音频时钟，diff=0，走策略C直接渲染
        //
        //   情况B: 音频 EOF 后 SDL 主动重置，音频永久停止不再增长
        //          特征：audio_finished_=true（由 OnAudioCompleted 触发设置）
        //          处理：直接 break 出内层 while，让外层取到 EOF 帧在阶段3处理
        //          如果不处理情况B，策略B会死循环：
        //          音频时钟永远不增长 → diff 永远是负数 → 永远休眠等待 → EOF 帧永远出不来

        while (!is_abort_.load() && !sync_ctx_->is_seeking_.load() &&
               sync_ctx_->state_.load() == PlaybackState::kPlaying) {
            int64_t video_pts_us = video_frame->GetTimestampUs();
            int64_t audio_clock_us = audio_speaker_->GetAudioClockUs();

            // 处理 audio_clock_us = -1 的两种情况
            if (audio_clock_us < 0) {
                if (audio_finished_.load()) {
                    // 情况B：音频已永久停止
                    // break 出内层 while，外层阶段2取下一帧
                    // 下一帧是 EOF 帧时，阶段3触发 OnSyncCompleted，播放完成流程正常走通
                    LOG_DEBUG(
                        "[AV Sync] Audio finished, exit sync wait. "
                        "Expecting EOF frame next.");
                    break;
                }
                // 情况A：过渡期，降级处理，diff=0 走策略C直接渲染
                audio_clock_us = video_pts_us;
            }

            // 每秒打印一次同步快照，便于观察播放进度和排查音画不同步问题
            auto log_now = std::chrono::steady_clock::now();
            if (log_now - last_render_log_time_ >= std::chrono::seconds(1)) {
                last_render_log_time_ = log_now;
                LOG_INFO("[AV Sync] video={:.3f}s  audio={:.3f}s  diff={:+.1f}ms",
                         static_cast<double>(video_pts_us) / 1000000.0, static_cast<double>(audio_clock_us) / 1000000.0,
                         static_cast<double>(audio_clock_us - video_pts_us) / 1000.0);
            }

            int64_t time_diff_us = audio_clock_us - video_pts_us;

            // 异常检测：视频落后音频超过 5s
            // 通常发生在极端 Seek 场景（音频时钟已到新位置，视频还在旧位置）
            // 直接丢弃当前帧，外层取下一帧追赶
            // 注：阈值为 5s（原 1s 太严格，seek 后正常过渡期就会误触发）
            if (time_diff_us > 5000000LL && !sync_ctx_->is_seeking_.load()) {
                LOG_WARN("[AV Sync] Anomaly >5s: video={:.3f}s audio={:.3f}s, dropping.",
                         static_cast<double>(video_pts_us) / 1000000.0,
                         static_cast<double>(audio_clock_us) / 1000000.0);
                break;
            }

            // 策略A：视频落后（audio 领先超过 +25ms）
            // 丢弃当前帧，外层取下一帧，追赶音频进度
            if (time_diff_us > kSyncThresholdUs) {
                LOG_DEBUG("[AV Sync] Drop late frame: pts={:.3f}s, behind by {:.1f}ms",
                          static_cast<double>(video_pts_us) / 1000000.0, static_cast<double>(time_diff_us) / 1000.0);
                break;
            }

            // 策略B：视频超前（audio 落后超过 -25ms）
            // 休眠 10ms 后重新评估同一帧，等待音频时钟推进到合适位置
            // 使用 wait_for + lambda 而非 sleep，是为了能被 Seek/暂停/停止立刻中断
            if (time_diff_us < -kSyncThresholdUs) {
                std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
                sync_ctx_->state_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                    return is_abort_.load() || sync_ctx_->is_seeking_.load() ||
                        sync_ctx_->state_.load() != PlaybackState::kPlaying ||
                        audio_finished_.load(); // 音频结束时也要立刻唤醒
                });
                // 醒来后 continue，重新读最新 audio_clock_us 再次评估
                // 如果是 audio_finished_ 触发的唤醒，下一轮循环顶部会走情况B的 break
                continue;
            }

            // 策略C：时间差在 ±25ms 容忍窗口内，立即渲染
            {
                video_clock_us_.store(video_pts_us);
                std::unique_lock<std::mutex> lock(listener_mutex_);
                if (listener_) {
                    listener_->OnSyncNotifyVideoFrame(std::move(video_frame));
                }
            }
            break; // 渲染完成，外层取下一帧
        }
    }

    LOG_INFO("AVSynchronizer core thread exited gracefully");
}
} // namespace av
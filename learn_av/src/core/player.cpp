/**
 * @file player.cpp
 * @brief 播放器核心调度中枢实现类
 * @details 连接UI模块，控制播放、暂停、停止,接收来自FileReader的PCM和YUV并进行下一步分发
 */

#include "player.h"

#include <atomic>
#include <memory>
#include <string>

#include "define/av_frame.h"
#include "interface/iaudio_speaker.h"
#include "interface/iav_synchronizer.h"
#include "interface/ifile_reader.h"
#include "iplayer.h"
#include "ivideo_sink.h"
#include "logger.h"
#include "playback_state.h"
#include "playback_sync.h"
#include "video_frame.h"

namespace av {
namespace {
const char* StateToString(PlaybackState state) {
    switch (state) {
        case PlaybackState::kIdle:
            return "kIdle";
        case PlaybackState::kOpened:
            return "kOpened";
        case PlaybackState::kPlaying:
            return "kPlaying";
        case PlaybackState::kPaused:
            return "kPaused";
        case PlaybackState::kStopped:
            return "kStopped";
        default:
            return "Unknown";
    }
}
} // namespace
class Frame;
class VideoFrame;
enum class PlaybackState;

/**
 * @brief Player Create
 */
std::shared_ptr<IPlayer> IPlayer::Create(std::shared_ptr<IGLContext> shared_gl_context) {
    return std::make_shared<Player>(shared_gl_context);
}

/**
 * @brief 构造函数
 */
Player::Player(std::shared_ptr<IGLContext> gl_context)
    : gl_context_(std::move(gl_context)), sync_ctx_(std::make_shared<SyncContext>()) {
    // 1. 创建file_reader_实例
    file_reader_ = std::unique_ptr<IFileReader>(IFileReader::Create());

    // 2. 建立上行数据回调通道,数据传给file_reader_
    if (file_reader_) {
        file_reader_->SetListener(this);
    }

    // 3. 创建并初始化音频硬件输出设备
    audio_speaker_ = std::unique_ptr<IAudioSpeaker>(IAudioSpeaker::Create());
    if (audio_speaker_) {
        audio_speaker_->SetListener(this);
    }

    // 4. 创建音画同步模块
    av_sync_ = std::unique_ptr<IAVSynchronizer>(IAVSynchronizer::Create(sync_ctx_, audio_speaker_.get()));
    if (av_sync_) {
        av_sync_->SetListener(this);
    }

    // 5. 创建心跳循环函数并启动
    if (!progress_thread_.joinable()) {
        is_progress_thread_running_.store(true);
        progress_thread_ = std::thread(&Player::ProgressLoop, this);
    }
}

/**
 * @brief 析构函数
 */
Player::~Player() {
    Stop();
    is_progress_thread_running_.store(false);
    if (progress_thread_.joinable()) {
        progress_thread_.join();
    }
}

// ================= 渲染管道控制 =================

/**
 * @brief 绑定视频渲染槽
 * @param sink 实现了 IVideoSink 接口的视频帧接收器
 */
void Player::Player::AttachDisplayView(IVideoSink* sink) {
    std::lock_guard<std::mutex> lock(sinks_mutex_);

    // 检查是否已经绑定，防止重复添加导致同一帧被渲染两次
    auto it = std::find(video_sinks_.begin(), video_sinks_.end(), sink);

    // 如果遍历完毕后还没有找到，就将其 Attach
    if (it == video_sinks_.end()) {
        video_sinks_.push_back(sink);
        LOG_INFO("Video sink attached successfully. Current sink count: {}", video_sinks_.size());
    }
}

/**
 * @brief 解绑视频渲染槽
 * @param sink 要解绑的视频帧接收器
 */
void Player::Player::DetachDisplayView(IVideoSink* sink) {
    std::lock_guard<std::mutex> lcok(sinks_mutex_);

    // find 要解绑的 sink
    auto it = std::find(video_sinks_.begin(), video_sinks_.end(), sink);

    // 从名单中剔除该裸指针，切断后续推流
    if (it != video_sinks_.end()) {
        video_sinks_.erase(it);
        LOG_INFO("Video sink detached successfully. Current sink count: {}", video_sinks_.size());
    }
}

// ================= 播放状态机控制指令 =================

/**
 * @brief 打开媒体流并建立底层数据路由
 * @param url 媒体文件路径或网络流地址
 * @return true 探测成功；false 打开失败
 */
bool Player::Open(const std::string& url) {
    // 1. 中断并回收既有的后台线程资源
    Stop();

    // 2. 打开文件
    if (!file_reader_) {
        return false;
    }

    bool ret = file_reader_->Open(url);
    if (ret) {
        ChangeState(PlaybackState::kOpened);
    }

    audio_finished_.store(false);
    video_finished_.store(false);

    has_video_.store(file_reader_->HasVideo());
    has_audio_.store(file_reader_->HasAduio());
    return ret;
}

/**
 * @brief 点击播放
 */
void Player::Play() {
    if (!sync_ctx_) {
        LOG_ERROR("Fatal: SyncContext is invalid before Play().");
        return;
    }

    // 情况1：从kOpened状态第一次播放
    PlaybackState expected_opened = PlaybackState::kOpened;
    if (sync_ctx_->state_.compare_exchange_strong(expected_opened, PlaybackState::kPlaying)) {
        LOG_INFO("====== State transition: kOpened -> kPlaying. Initializing worker threads ======");

        // 1. 先启动消费者
        if (av_sync_) {
            av_sync_->Start();
        }

        if (audio_speaker_) {
            audio_speaker_->Play();
        }

        // 2. 再启动生产者
        if (file_reader_) {
            file_reader_->Start(sync_ctx_);
        }

        if (listener_) {
            listener_->OnPlaybackStateChanged(PlaybackState::kPlaying);
        }
        return;
    }

    // 情况2：从kPaused状态恢复播放
    PlaybackState expected_paused = PlaybackState::kPaused;
    if (sync_ctx_->state_.compare_exchange_strong(expected_paused, PlaybackState::kPlaying)) {
        LOG_INFO("====== State transition: kPaused -> kPlaying. Waking up suspended threads ======");

        if (audio_speaker_) {
            audio_speaker_->Play();
        }

        // 唤醒其他线程
        sync_ctx_->state_cv_.notify_all();

        if (listener_) {
            listener_->OnPlaybackStateChanged(PlaybackState::kPlaying);
        }
        return;
    }

    // 情况3：从kStopped状态重新播放
    PlaybackState expected_stopped = PlaybackState::kStopped;
    if (sync_ctx_->state_.compare_exchange_strong(expected_stopped, PlaybackState::kPlaying)) {
        LOG_INFO("====== State transition: kStopped -> kPlaying. Re-initializing worker threads ======");

        // 1. 先启动消费者
        if (av_sync_) {
            av_sync_->Start();
        }

        if (audio_speaker_) {
            audio_speaker_->Play();
        }

        // 2. 再启动生产者
        if (file_reader_) {
            file_reader_->Start(sync_ctx_);
        }

        if (listener_) {
            listener_->OnPlaybackStateChanged(PlaybackState::kPlaying);
        }
        return;
    }
    LOG_DEBUG("Play() invoked but state is already kPlaying or invalid for transition.");
}

/**
 * @brief 驱动有限状态机向 kPaused 跃迁，触发底层线程休眠屏障
 */
void Player::Pause() {
    if (!sync_ctx_) {
        return;
    }

    LOG_INFO("========== Play Pause ==========");

    PlaybackState expected_playing = PlaybackState::kPlaying;

    if (sync_ctx_->state_.compare_exchange_strong(expected_playing, PlaybackState::kPaused)) {
        // 返回 true 的分支：跃迁成功
        // 发生条件：CPU 读取 sync_ctx_->state_ 发现它等于 kPlaying
        // 底层行为：CPU 瞬间将 sync_ctx_->state_ 修改为 kPaused

        LOG_INFO("State transition: kPlaying -> kPaused. Notifying threads to suspend.");

        // 暂停音频播放
        if (audio_speaker_) {
            audio_speaker_->Pause();
        }

        // 广播信令：唤醒所有可能阻塞在 state_cv_ 上的线程，让它们去读取新的 kPaused 状态并休眠
        sync_ctx_->state_cv_.notify_all();

        // 业务回调：通知 UI 层状态变更
        if (listener_) {
            listener_->OnPlaybackStateChanged(PlaybackState::kPaused);
        }
    } else {
        LOG_DEBUG("Pause() invoked but state is not kPlaying.");
    }
}

/**
 * @brief 终止播放状态机，安全释放底层的解码与解复用引擎
 */
void Player::Stop() {
    LOG_INFO("========== Play Stop ==========");

    // 1. 获取当前状态，如果已经停止
    PlaybackState current_state = sync_ctx_->state_.load();
    if (current_state == PlaybackState::kStopped || current_state == PlaybackState::kIdle) {
        return;
    }

    // 阶段 1：下发全局信令
    // 1. 改变全局状态，唤醒所有因 Pause 挂起的线程
    ChangeState(PlaybackState::kStopped);
    sync_ctx_->state_cv_.notify_all();

    // 2. 唤醒所有因队列满/空而挂起的线程
    if (av_sync_) {
        av_sync_->RequestStop(); // 唤醒 AVSync 内部阻塞，并 Abort video_queue
    }

    if (audio_speaker_) {
        audio_speaker_->Abort();
    }

    if (file_reader_) {
        file_reader_->RequestStop(); // 内部依次调用 decoder 和 demuxer 的 RequestStop
    }

    // 阶段 2： 回收线程
    // 2A. 回收最上游：Demuxer/AV_Decoder
    if (file_reader_) {
        file_reader_->WaitForStop(); // 内部先 join demuxer，再 join decoders
    }

    // 2B. 回收下游：音画同步器
    if (av_sync_) {
        av_sync_->WaitForStop();
    }

    // 2C. 回收最下游：物理硬件声卡
    if (audio_speaker_) {
        audio_speaker_->Close();
    }

    LOG_INFO("========== Player resources fully released ==========");
}

/**
 * @brief 触发媒体物理时间轴跳转 (Seek)
 * @param target_time_us 目标绝对时间（单位：微秒）
 * @details 封装组合状态序列：Pause(强挂起) -> Flush(数据冲刷) -> Seek(时间轴重定位) -> Play(现场恢复)
 */
void Player::SeekTo(int64_t target_time_us) {
    if (!sync_ctx_)
        return;

    PlaybackState old_state = sync_ctx_->state_.load();
    if (audio_speaker_) {
        audio_speaker_->ResetAlock();
    }

    // 1. 触发状态机向 kPaused 跃迁，执行休眠同步点
    Pause();

    // 2. 解除阻塞状态并销毁队列内的历史数据
    if (file_reader_) {
        file_reader_->AbortAndClearDecoderQueues(); // 处理 Demuxer 到 Decoder 的 packet_queue
    }
    if (av_sync_) {
        av_sync_->AbortAndClearQueue(); // 处理 Player 到渲染器的 video_queue
    }
    if (audio_speaker_) {
        audio_speaker_->AbortAndClearQueue(); // 处理 Player 到音频设备的 audio_queue
    }

    // 3. 配置上下文状态变量
    {
        std::lock_guard<std::mutex> lock(sync_ctx_->state_mutex_);
        sync_ctx_->is_seeking_.store(true);
        sync_ctx_->target_time_us_.store(target_time_us);
        sync_ctx_->resume_play_after_seek_ = (old_state == PlaybackState::kPlaying);
        sync_ctx_->state_cv_.notify_all();
    }

    // 4. 调用解封装层执行物理重定位
    if (file_reader_) {
        file_reader_->SeekTo(sync_ctx_->target_time_us_);
    }
}
// ================= 状态查询与事件订阅 =================

/**
 * @brief 获取当前媒体全局总时长
 * @return 总时长（单位：秒）。若是直播流或未就绪，返回 0.0
 */
int64_t Player::GetDuration() const {
    // if  file_reader_ != nullptr, return file_reader_->GetDuration()
    return file_reader_ ? file_reader_->GetDuration() : 0;
}

/**
 * @brief 获取图像物理宽度
 * @note 仅在 Open 成功解析流信息后有效
 */
int Player::GetVideoWidth() const { return file_reader_ ? file_reader_->GetVideoWidth() : 0; }

/**
 * @brief 获取图像物理高度
 * @note 仅在 Open 成功解析流信息后有效
 */
int Player::GetVideoHeight() const { return file_reader_ ? file_reader_->GetVideoHeight() : 0; }

/**
 * @brief 查询当前播放器状态机的原子状态
 * @return 当前所处的 PlaybackState 枚举值
 */
PlaybackState Player::GetState() const {
    LOG_INFO("the state is: {}", StateToString(sync_ctx_->state_.load()));
    return sync_ctx_->state_.load();
}

void Player::SetListener(IPlayer::Listener* listener) { listener_ = listener; }

// ================= IFileReader::Listener 数据上行接口实现 =================

/**
 * @brief 接收底层解码输出的纯净 PCM 音频帧
 * @param audio_frame 包含采样率、声道数等物理参数的音频数据 DTO
 */
void Player::OnFileReaderNotifyAudioFrame(std::shared_ptr<Frame> audio_frame) {
    // 1. 判断audio_frame合法性
    if (!audio_frame) {
        return;
    }

    // 2. 判断是不是Flush/EOF 包
    if (audio_frame->HasFlag(AVFrameFlag::kFlush) || audio_frame->HasFlag(AVFrameFlag::kEOF)) {
        if (audio_speaker_) {
            audio_speaker_->PushAudioFrame(std::move(audio_frame));
        }
        return;
    }

    // 3. 常规数据的合法性校验
    if (audio_frame->IsEmpty()) {
        return;
    }

    // 4. 将音频帧直接投递至物理扬声器的线程安全队列中
    if (audio_speaker_) {
        audio_speaker_->PushAudioFrame(std::move(audio_frame));
    }
}

/**
 * @brief 接收底层解码输出的纯净 YUV/RGB 视频帧
 * @param video_frame 包含分辨率、色彩空间等物理参数的视频数据 DTO
 */
void Player::OnFileReaderNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    // 1. 判断video_frame合法性
    if (!video_frame) {
        return;
    }

    // 2. 判断是不是Flush/EOF 包
    if (video_frame->HasFlag(AVFrameFlag::kFlush) || video_frame->HasFlag(AVFrameFlag::kEOF)) {
        if (av_sync_) {
            av_sync_->PushVideoFrame(std::move(video_frame));
            return;
        }
    }

    // 4. 常规数据的合法性校验
    if (!video_frame->IsValid()) {
        return;
    }

    // 5. 将视频送入音画同步模块
    if (av_sync_) {
        av_sync_->PushVideoFrame(std::move(video_frame));
    }
}

void Player::OnSyncNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    // 2. 将视频帧传递给后续渲染模块
    std::lock_guard<std::mutex> lock(sinks_mutex_);

    for (auto* sink : video_sinks_) {
        if (sink) {
            sink->OnReceiveVideoFrame(video_frame);
        }
    }
}

/**
 * @brief 音画同步器宣告彻底渲染完毕 (视频流结束)
 */
void Player::OnSyncCompleted() {
    LOG_INFO("Player received Video TRULY finished signal.");

    // 1. 标记视频管线已结束
    video_finished_.store(true);

    // 2. 触发判定
    CheckAndNotifyPlaybackCompleted();
}

/**
 * @brief 扬声器宣告彻底播放完毕 (音频流结束)
 */
void Player::OnAudioCompleted() {
    LOG_INFO("Player received Audio TRULY finished signal.");

    // 1. 标记音频管线已结束
    audio_finished_.store(true);

    // 2. 通知 AVSync 音频已永久停止，让 AVSync 策略B不再等待音频时钟，直接处理剩余帧和 EOF 帧
    if (av_sync_) {
        av_sync_->NotifyAudioFinished();
    }

    // 2. 触发判定
    CheckAndNotifyPlaybackCompleted();
}

/**
 * @brief 实现 IFileReader::Listener 接口
 * @details 响应底层复位信令，解除全局所有数据队列的 Abort 状态
 */
void Player::OnFileReaderSeekResetQueues() {
    LOG_INFO("Player received SeekResetQueues. Re-enabling all pipeline queues.");

    // 1. 解除 FileReader 内部解码器 Packet 队列的阻塞状态
    if (file_reader_) {
        file_reader_->StartDecoderQueues();
    }
    // 2. 解除音视频同步器视频队列的阻塞状态
    if (av_sync_) {
        av_sync_->StartQueue();
    }
    // 3. 解除音频输出组件音频队列的阻塞状态
    if (audio_speaker_) {
        audio_speaker_->StartQueue();
    }
}

/**
 * @brief 实现 IFileReader::Listener 接口
 * @details 响应底层 Seek 完成信令，正式恢复有限状态机至播放状态
 */
void Player::OnFileReaderSeekCompleteResumePlay() {
    LOG_INFO("Player received SeekCompleteResumePlay. Restarting playback loop.");

    Play();
}

/**
 * @brief 接收获取到的采样率和声道数，启动SDL音频播放功能
 */
void Player::OnFileReaderAudioInfo(int sample_rate, int channels) {
    LOG_INFO("Player received audio format from pipeline: {}Hz, {} channels. Opening sound card...", sample_rate,
             channels);

    if (audio_speaker_) {
        bool ret = audio_speaker_->Open(sample_rate, channels);
        if (!ret) {
            LOG_ERROR("Failed to open audio speaker with negotiated parameters!");
        }
    }
}

// ================= 内部控制逻辑 =================

/**
 * @brief 内部状态机跃迁辅助函数
 * @param new_state 目标原子状态
 * @details 负责执行 CAS 状态锁更新，并异步派发事件至 UI 层
 */
void Player::ChangeState(PlaybackState new_state) {
    PlaybackState old_state = sync_ctx_->state_.exchange(new_state);

    if (old_state != new_state) {
        LOG_INFO("State transition: {} -> {}", StateToString(old_state), StateToString(new_state));

        if (listener_) {
            listener_->OnPlaybackStateChanged(new_state);
        }
    }
}

/**
 * @brief 心跳循环线程（用于进度条回调显示）
 */
void Player::ProgressLoop() {
    const auto kUpdateInterval = std::chrono::milliseconds(100);

    while (is_progress_thread_running_.load()) {
        if (sync_ctx_ && sync_ctx_->state_.load() == PlaybackState::kPlaying) {
            // 1. 获取当前时间（微妙）
            int64_t current_time_us = 0;
            if (audio_speaker_) {
                current_time_us = audio_speaker_->GetAudioClockUs();
            }

            // 时钟未就绪时跳过本次更新，继续循环等待
            if (current_time_us < 0) {
                std::this_thread::sleep_for(kUpdateInterval);
                continue;
            }

            // 2. 获取总时长（微妙）
            int64_t total_time_us = file_reader_ ? file_reader_->GetDuration() : 0;

            // 3. 计算百分比
            float progress = 0.0f;
            if (total_time_us > 0) {
                progress =
                    static_cast<float>(static_cast<double>(current_time_us) / static_cast<double>(total_time_us));
                // 边界防御
                progress = std::clamp(progress, 0.0f, 1.0f);
            }

            // 4. 触发回调
            if (listener_) {
                listener_->OnPlaybackProgress(progress, current_time_us, total_time_us);
            }
        }
        std::this_thread::sleep_for(kUpdateInterval);
    }
}

/**
 * @brief 判定所有存在的流是否均已结束
 */
void Player::CheckAndNotifyPlaybackCompleted() {
    // 1. 检查各轨道状态：若该轨道不存在，或该轨道已结束，则视为该部分 Done
    bool video_done = !has_video_.load() || video_finished_.load();
    bool audio_done = !has_audio_.load() || audio_finished_.load();

    // 2. 当且仅当所有存在的轨道都结束时，才执行全局状态切换
    if (video_done && audio_done) {
        PlaybackState expected_playing = PlaybackState::kPlaying;
        PlaybackState expected_paused = PlaybackState::kPaused;

        // 3. 使用 CAS 操作修改底层状态机，防范多线程并发执行时导致 UI 被重复通知
        if (sync_ctx_->state_.compare_exchange_strong(expected_playing, PlaybackState::kStopped) ||
            sync_ctx_->state_.compare_exchange_strong(expected_paused, PlaybackState::kStopped)) {
            LOG_INFO("Playback TRULY Completed. Notifying UI...");

            // 4. 唯一合法的 UI 通知触发点
            if (listener_) {
                listener_->OnPlaybackCompleted();
            }
        }
    }
}
}; // namespace av

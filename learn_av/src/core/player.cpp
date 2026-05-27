/**
 * @file player.cpp
 * @brief 播放器核心调度中枢实现类
 * @details 连接UI模块，控制播放、暂停、停止,接收来自FileReader的PCM和YUV并进行下一步分发
 */

#include "player.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <string>

#include "define/av_frame.h"
#include "interface/ifile_reader.h"
#include "iplayer.h"
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
    LOG_INFO("PLayer Create...");
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
}

/**
 * @brief 析构函数
 */
Player::~Player() { Stop(); }

// ================= 渲染管道控制 =================

/**
 * @brief 绑定视频渲染槽
 * @param sink 实现了 IVideoSink 接口的视频帧接收器
 */
void Player::Player::AttachDisplayView(IVideoSink* sink) {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    video_sinks_.push_back(sink);
}

/**
 * @brief 解绑视频渲染槽
 * @param sink 要解绑的视频帧接收器
 */
void Player::Player::DetachDisplayView(IVideoSink* sink) {
    std::lock_guard<std::mutex> lcok(sinks_mutex_);

    // find 要解绑的 sink
    auto it = std::find(video_sinks_.begin(), video_sinks_.end(), sink);

    // 如果找到了
    if (it != video_sinks_.end()) {
        video_sinks_.erase(it);
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

    if (!file_reader_) {
        return false;
    }

    // 2. 调试代码：以截断模式创建并打开新的文件
    if (debug_pcm_file_.is_open()) {
        debug_pcm_file_.close();
    }
    if (debug_yuv_file_.is_open()) {
        debug_yuv_file_.close();
    }

    debug_pcm_file_.open("debug_out.pcm", std::ios::binary | std::ios::trunc);
    debug_yuv_file_.open("debug_out.yuv", std::ios::binary | std::ios::trunc);

    // 3. 打开文件
    bool ret = file_reader_->Open(url);
    if (ret) {
        ChangeState(PlaybackState::kOpened);
    }
    return ret;
}

/**
 * @brief 点击播放
 */
void Player::Play() {
    if (!sync_ctx_ || !sync_ctx_->IsValid()) {
        LOG_ERROR("Fatal: SyncContext is invalid before Play().");
        return;
    }

    // 情况1：从kOpened状态第一次播放
    PlaybackState expected_opened = PlaybackState::kOpened;
    if (sync_ctx_->state_.compare_exchange_strong(expected_opened, PlaybackState::kPlaying)) {
        LOG_INFO("State transition: kOpened -> kPlaying. Initializing worker threads.");

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
        LOG_INFO("State transition: kPaused -> kPlaying. Waking up suspended threads.");

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
        LOG_INFO("State transition: kStopped -> kPlaying. Re-initializing worker threads.");

        if (file_reader_) {
            // 线程已在 Stop 时销毁，需重新执行创建序列
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

    PlaybackState expected_playing = PlaybackState::kPlaying;

    if (sync_ctx_->state_.compare_exchange_strong(expected_playing, PlaybackState::kPaused)) {
        // 返回 true 的分支：跃迁成功
        // 发生条件：CPU 读取 sync_ctx_->state_ 发现它等于 kPlaying
        // 底层行为：CPU 瞬间将 sync_ctx_->state_ 修改为 kPaused
        // 业务执行：因为成功抢占了状态机，当前线程可以合法地执行暂停相关的收尾工作

        LOG_INFO("State transition: kPlaying -> kPaused. Notifying threads to suspend.");
        // 广播信令：唤醒所有可能阻塞在 state_cv_ 上的线程，让它们去读取新的 kPaused 状态并休眠
        sync_ctx_->state_cv_.notify_all();

        // 业务回调：通知 UI 层状态变更
        if (listener_) {
            listener_->OnPlaybackStateChanged(PlaybackState::kPaused);
        }
    } else {
        // 返回 false 的分支：跃迁被拒绝
        // 发生条件：CPU 读取 sync_ctx_->state_ 发现它不等于 kPlaying（可能是 kPaused, kStopped 等）
        // 底层行为：拒绝修改 state_，保持原样
        // 业务执行：防御性编程，跳过 if 块内部的逻辑，直接丢弃本次无效的暂停请求

        LOG_DEBUG("Pause() invoked but state is not kPlaying.");
    }
}

/**
 * @brief 终止播放状态机，安全释放底层的解码与解复用引擎
 */
void Player::Stop() {
    PlaybackState current_state = sync_ctx_->state_.load();

    if (current_state == PlaybackState::kStopped || current_state == PlaybackState::kIdle) {
        return;
    }

    // 1. 标记全链路终止
    ChangeState(PlaybackState::kStopped);

    // 2. 触发条件变量广播，防止底层线程死锁于 kPaused 的 wait() 屏障中，确保可被成功 join
    sync_ctx_->state_cv_.notify_all();

    // 3. 执行底层物理线程回收与缓冲清理
    if (file_reader_) {
        file_reader_->Stop();
    }

    // 3. 调试代码：安全关闭测试文件流，强制数据刷入磁盘
    if (debug_pcm_file_.is_open()) {
        debug_pcm_file_.close();
    }

    if (debug_yuv_file_.is_open()) {
        debug_yuv_file_.close();
    }
}

/**
 * @brief 触发媒体物理时间轴跳转 (Seek)
 * @param target_time_sec 目标绝对时间（单位：秒）
 * @details 封装组合状态序列：Pause(强挂起) -> Flush(数据冲刷) -> Seek(时间轴重定位) -> Play(现场恢复)
 */
void Player::SeekTo(double target_time_sec) {
    if (!sync_ctx_) {
        return;
    }

    // 1. 记录跳转前的原状态
    PlaybackState old_state_ = sync_ctx_->state_.load();

    // 2. 触发挂起信令：利用条件变量阻塞底层队列的生产-消费流动
    if (old_state_ == PlaybackState::kPlaying) {
        Pause();
    }

    // 3. 开启 Seek 瞬态标志，并穿透 Pause 屏障强行唤醒解码线程
    {
        std::lock_guard<std::mutex> lock(sync_ctx_->state_mutex_);
        sync_ctx_->is_seeking_.store(true);
        sync_ctx_->state_cv_.notify_all();
    }

    // 4. 通知file_reader_模块进行绝对时间跳转
    if (file_reader_) {
        file_reader_->SeekTo(target_time_sec);
    }

    {
        std::lock_guard<std::mutex> lock(sync_ctx_->state_mutex_);
        sync_ctx_->is_seeking_.store(true);
    }

    // 5. 若原状态为播放，则完成跳转后自动恢复播放
    if (old_state_ == PlaybackState::kPlaying) {
        Play();
    }
}

// ================= 状态查询与事件订阅 =================

/**
 * @brief 获取当前媒体全局总时长
 * @return 总时长（单位：秒）。若是直播流或未就绪，返回 0.0
 */
double Player::GetDuration() const {
    // if  file_reader_ != nullptr, return file_reader_->GetDuration()
    return file_reader_ ? file_reader_->GetDuration() : 0.0;
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

    AVFrame* raw_frame = audio_frame->GetRawFrame();
    if (!raw_frame || raw_frame->nb_samples <= 0 || !raw_frame->data[0]) {
        return;
    }

    // 2. 存储PCM
    if (debug_pcm_file_.is_open()) {
        // 计算公式：采样数 * 通道数 * 采样位深(16位=2字节)
        int data_size = raw_frame->nb_samples * raw_frame->channels * 2;
        debug_pcm_file_.write(reinterpret_cast<const char*>(raw_frame->data[0]), data_size);
    }
}

/**
 * @brief 接收底层解码输出的纯净 YUV/RGB 视频帧
 * @param video_frame 包含分辨率、色彩空间等物理参数的视频数据 DTO
 */
void Player::OnFileReaderNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    // 1. 判断video_frame合法性
    if (!video_frame || !video_frame->buffer_ || video_frame->width_ <= 0 || video_frame->height_ <= 0) {
        LOG_ERROR("Invalid video frame! width: {}, height: {}, buffer: {}", video_frame ? video_frame->width_ : 0,
                  video_frame ? video_frame->height_ : 0,
                  static_cast<void*>(video_frame ? video_frame->buffer_.get() : nullptr));
        return;
    }

    // 2. 正确写入YUV420P文件
    if (debug_yuv_file_.is_open()) {
        // 计算正确的分量大小（兼容奇数分辨率）
        const int kYSize = video_frame->width_ * video_frame->height_;
        const int kUvWidth = (video_frame->width_ + 1) / 2;
        const int kUvHeight = (video_frame->height_ + 1) / 2;
        const int kUvSize = kUvWidth * kUvHeight;
        const int kTotalSize = kYSize + kUvSize * 2;

        // 分别写入三个分量
        debug_yuv_file_.write(reinterpret_cast<const char*>(video_frame->data_[0]),
                              static_cast<std::streamsize>(kYSize));
        debug_yuv_file_.write(reinterpret_cast<const char*>(video_frame->data_[1]),
                              static_cast<std::streamsize>(kUvSize));
        debug_yuv_file_.write(reinterpret_cast<const char*>(video_frame->data_[2]),
                              static_cast<std::streamsize>(kUvSize));

        // 强制刷新缓冲区，避免数据留在内存
        debug_yuv_file_.flush();
    }
}

/**
 * @brief Audio EOF
 * @details 底层音频解码器内部残留帧已全部排空
 */
void Player::OnFileReaderNotifyAudioFinished() {
    // 预留接口，后续可在此处增加 audio_eof_ 标志位的设置与播放完成检查
}

/**
 * @brief Video EOF
 * @details 底层视频解码器内部残留帧已全部排空
 */
void Player::OnFileReaderNotifyVideoFinished() {
    // 预留接口，后续可在此处增加 video_eof_ 标志位的设置与播放完成检查
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
}; // namespace av

/**
 * @file sdl_audio_speaker.cpp
 * @brief 音频播放类实现
 */

#include "sdl_audio_speaker.h"

#include <SDL2/SDL.h>

#include <memory>
#include <mutex>

#include "define/av_frame.h"
#include "logger.h"
#include "utils/thread_safe_queue.h"

namespace av {

/**
 * @brief 创建音频播放实例
 */
std::unique_ptr<IAudioSpeaker> IAudioSpeaker::Create() { return std::make_unique<SDLAudioSpeaker>(); }

/**
 * @brief 设置下游监听器
 */
void SDLAudioSpeaker::SetListener(Listener* listener) { listener_ = listener; }

/**
 * @brief 构造函数
 */
SDLAudioSpeaker::SDLAudioSpeaker() {}

/**
 * @brief 析构函数
 */
SDLAudioSpeaker::~SDLAudioSpeaker() { Close(); }

/**
 * @brief 初始化扬声器硬件
 * @param sample_rate 目标采样率 (例如 44100)
 * @param channels 目标声道数 (例如 2)
 * @return true 初始化成功；false 初始化失败
 */
bool SDLAudioSpeaker::Open(int sample_rate, int channels) {
    // 参数校验：采样率/声道数非法时直接拒绝，避免后续时钟换算除零
    if (sample_rate <= 0 || channels <= 0) {
        LOG_ERROR("SDLAudioSpeaker::Open invalid params: sample_rate={}, channels={}", sample_rate, channels);
        return false;
    }

    // 1. 初始化 SDL 音频子系统（须最先执行）
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        LOG_ERROR("SDL_Init(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
        return false;
    }

    sample_rate_ = sample_rate;
    channels_ = channels;

    // 2. 期望参数 / 实际协商得到的参数
    SDL_AudioSpec wanted_spec;
    SDL_AudioSpec obtained_spec;

    wanted_spec.freq = sample_rate_; // 采样率
    wanted_spec.format = AUDIO_S16SYS; // 采样格式：16 位有符号整数，交错排布
    wanted_spec.silence = 0; // 静音填充值
    wanted_spec.samples = 1024; // 单次回调样本数

    // 3. 注册回调与 userdata
    wanted_spec.callback = SDLAudioCallback; // 硬件缺数据时由 SDL 回调线程调用
    wanted_spec.userdata = this; // 回调时透传 this 指针

    // 4. 打开音频设备（底层连接硬件，并将实际参数写回 obtained_spec）
    audio_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &obtained_spec, 0);
    if (audio_device_id_ == 0) {
        LOG_ERROR("SDL_OpenAudioDevice failed: {}", SDL_GetError());
        return false;
    }

    // 5. 设备已就绪，读取实际驱动与协商参数
    const char* driver_name = SDL_GetCurrentAudioDriver();
    LOG_INFO("SDL Audio Device opened successfully. Driver: {}, Freq: {}, channels: {}",
             driver_name ? driver_name : "Unknown", obtained_spec.freq, obtained_spec.channels);

    // 重新激活队列
    audio_queue_.Start();

    // 复位 EOF 门闩，允许新一轮播放结束时再次通知
    eof_fired_.store(false);

    // audio_clock_us_ = -1 语义：设备尚未发声，时钟未就绪
    audio_clock_us_.store(-1);

    return true;
}

/**
 * @brief 启动音频物理设备播放
 */
void SDLAudioSpeaker::Play() {
    if (audio_device_id_ > 0) {
        is_playing_.store(true);
        LOG_INFO("SDLAudioSpeaker Play...");
        // SDL 的 API 很反直觉：传入 0 代表“取消暂停（即播放）”
        SDL_PauseAudioDevice(audio_device_id_, 0);
    }
}

/**
 * @brief 暂停音频物理设备播放
 */
void SDLAudioSpeaker::Pause() {
    if (audio_device_id_ > 0) {
        is_playing_.store(false);
        LOG_INFO("SDLAudioSpeaker Pause...");
        // 传入 1 代表“暂停”
        SDL_PauseAudioDevice(audio_device_id_, 1);
    }
}

/**
 * @brief 关闭设备并释放硬件资源
 */
void SDLAudioSpeaker::Close() {
    Pause();
    LOG_INFO("SDLAudioSpeaker Close...");

    audio_queue_.Abort();
    audio_queue_.Clear();

    if (audio_device_id_ > 0) {
        SDL_CloseAudioDevice(audio_device_id_);
        audio_device_id_ = 0;
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/**
 * @brief 接收解码后的 PCM 音频帧并放入硬件缓冲队列
 * @param audio_frame 纯净 PCM 帧，或者带内信令帧
 */
void SDLAudioSpeaker::PushAudioFrame(std::shared_ptr<Frame> audio_frame) {
    if (!audio_frame) {
        LOG_ERROR("SDLAudioSpeaker received null audio frame!");
        return;
    }

    // 1. Flush 带内信令：Seek 时冲刷硬件与应用层缓冲
    if (audio_frame->HasFlag(av::AVFrameFlag::kFlush)) {
        LOG_DEBUG("SDLAudioSpeaker intercepted in-band Flush signal, executing hardware reset.");

        std::lock_guard<std::mutex> lock(sync_mutex_);

        // 1. 暂停设备，防止冲刷过程中触发硬件回调
        SDL_PauseAudioDevice(audio_device_id_, 1);

        // 2. 清空 SDL 内部硬件缓冲（防止 Seek 后残留前一段音频）
        SDL_ClearQueuedAudio(audio_device_id_);

        // 3. 清空应用层队列（丢弃尚未送入硬件的旧帧）
        audio_queue_.Clear();

        // 4. 重置当前正在切片消费的帧状态
        current_frame_ = nullptr;
        current_frame_offset_ = 0;

        // 5. 复位音频时钟与 EOF 门闩
        audio_clock_us_.store(-1);
        eof_fired_.store(false);

        // 6. 若原为播放态，恢复设备运转
        if (is_playing_.load()) {
            SDL_PauseAudioDevice(audio_device_id_, 0);
        }

        LOG_DEBUG("SDLAudioSpeaker in-band flush completed, clock reset to -1.");

        return;
    }

    // 2. 如果是 EOF 信令
    if (audio_frame->HasFlag(AVFrameFlag::kEOF)) {
        audio_queue_.Push(std::move(audio_frame));
        return;
    }

    // 3. 如果 音频帧帧无效
    if (audio_frame->IsEmpty()) {
        LOG_WARN("SDLAudioSpeaker received empty audio frame");
        return;
    }

    // 4. 入队
    audio_queue_.Push(std::move(audio_frame));
}

/**
 * @brief 清空内部缓存
 */
void SDLAudioSpeaker::Flush() {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    // 1. 暂停音频设备
    SDL_PauseAudioDevice(audio_device_id_, 1);

    // 2. 清空 SDL 内部缓冲区
    SDL_ClearQueuedAudio(audio_device_id_);

    // 3. 清空应用层队列
    audio_queue_.Clear();

    // 4. 重置当前帧状态
    current_frame_ = nullptr;
    current_frame_offset_ = 0;

    audio_clock_us_.store(-1);
    eof_fired_.store(false);
}

/**
 * @brief audio_queue_队列唤醒
 */
void SDLAudioSpeaker::Abort() { audio_queue_.Abort(); }

/**
 * @brief 清空队列，唤醒阻塞
 */
void SDLAudioSpeaker::AbortAndClearQueue() { audio_queue_.AbortAndClear(); }

/**
 * @brief 启动队列
 */
void SDLAudioSpeaker::StartQueue() { audio_queue_.Start(); }

/**
 * @brief 获取当前精准的音频播放时钟（微秒）
 * @note 供音画同步模块调用以音频为基准进行对齐
 */
int64_t SDLAudioSpeaker::GetAudioClockUs() const { return audio_clock_us_.load(); }

/**
 * @brief 获取当前精准的音频播放时钟（秒）
 * @note 用于UI显示
 */
double SDLAudioSpeaker::GetAudioClock() const {
    int64_t current_us = audio_clock_us_.load();
    // 防御处理：如果时钟尚未就绪，向 UI 输出 0 秒
    return (current_us < 0) ? 0.0 : static_cast<double>(current_us) / 1000000.0;
}

/**
 * @brief 重置时钟（用于跳转时进度条更新）
 */
void SDLAudioSpeaker::ResetAlock() {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    audio_clock_us_.store(-1);
    eof_fired_.store(false);

    current_frame_ = nullptr;
    current_frame_offset_ = 0;
}

/**
 * @brief SDL 硬件回调入口（静态）
 * @note 运行在 SDL 独立的高优先级回调线程中；userdata 即 Open 时传入的 this 指针
 */
void SDLAudioSpeaker::SDLAudioCallback(void* userdata, Uint8* stream, int len) {
    auto* speaker = static_cast<SDLAudioSpeaker*>(userdata);
    speaker->FillAudioData(stream, len);
}

/**
 * @brief 填充 PCM 数据到 SDL 硬件音频缓冲区
 * @param stream SDL 硬件音频缓冲区首地址
 * @param len 本次回调请求填充的字节总长度
 * @note 运行在 SDL 回调线程；以 sync_mutex_ 保护 current_frame_/offset/clock，
 *       回调业务（OnAudioCompleted）须在解锁后调用以避免与上层回环加锁
 */
void SDLAudioSpeaker::FillAudioData(Uint8* stream, int len) {
    std::unique_lock<std::mutex> lock(sync_mutex_);

    // 1. 先填静音：若后续取不到数据，硬件播放静音而非脏内存
    SDL_memset(stream, 0, len);

    // 暂停态直接返回，输出静音
    if (!is_playing_.load()) {
        return;
    }

    // 已写入硬件缓冲区的累计字节偏移
    int total_bytes_written = 0;

    // 循环填充直到满足本次请求长度 len
    while (len > 0) {
        // 步骤 A：当前无帧时从队列非阻塞取新帧
        if (!current_frame_) {
            // 回调线程严禁阻塞等待
            if (!audio_queue_.TryPop(current_frame_)) {
                if (++underrun_log_count_ % 100 == 0) {
                    LOG_DEBUG("Audio queue underrun, padding silence...");
                }
                return;
            }
            // 取到新帧，复位读取偏移
            current_frame_offset_ = 0;
        }

        // 步骤 B：EOF 信令处理
        if (current_frame_->HasFlag(AVFrameFlag::kEOF)) {
            LOG_INFO("SDLAudioSpeaker received EOF");

            audio_clock_us_.store(-1);
            current_frame_ = nullptr;

            // 暂停设备，停止后续硬件回调
            SDL_PauseAudioDevice(audio_device_id_, 1);

            // 幂等门闩：保证 OnAudioCompleted 仅触发一次
            bool already_fired = eof_fired_.exchange(true);

            // 在临界区外回调业务层，避免持锁执行上层逻辑（OnAudioCompleted 会 notify_all 并 emit 至 UI）
            lock.unlock();
            if (!already_fired && listener_) {
                listener_->OnAudioCompleted();
            }
            return;
        }

        // 步骤 C：帧字节数（int64 计算防止溢出），非法时丢弃该帧
        int64_t nb_samples = current_frame_->GetRawFrame()->nb_samples;
        int64_t frame_total_bytes = nb_samples * channels_ * 2;
        if (frame_total_bytes <= 0 || sample_rate_ <= 0) {
            current_frame_ = nullptr;
            current_frame_offset_ = 0;
            continue;
        }

        // 步骤 D：更新音频主时钟（帧起始 PTS + 已消耗时长占比）
        if (current_frame_->IsValid()) {
            int64_t base_pts_us = current_frame_->GetTimestampUs();
            int64_t frame_duration_us = (nb_samples * 1000000LL) / sample_rate_;
            double consumed_ratio = static_cast<double>(current_frame_offset_) / static_cast<double>(frame_total_bytes);
            int64_t current_exact_clock_us =
                base_pts_us + static_cast<int64_t>(static_cast<double>(frame_duration_us) * consumed_ratio);
            audio_clock_us_.store(current_exact_clock_us);
        }

        // 步骤 E：计算本次拷贝长度 = min(硬件剩余请求, 当前帧剩余字节)
        int64_t remain_bytes = frame_total_bytes - current_frame_offset_;
        int bytes_to_copy = static_cast<int>(std::min<int64_t>(len, remain_bytes));

        // 步骤 F：拷贝 PCM 到硬件缓冲区
        memcpy(stream + total_bytes_written, current_frame_->GetRawFrame()->data[0] + current_frame_offset_,
               bytes_to_copy);

        // 步骤 G：推进偏移
        len -= bytes_to_copy; // 硬件剩余请求长度
        total_bytes_written += bytes_to_copy; // 已写入累计
        current_frame_offset_ += bytes_to_copy; // 当前帧读取偏移

        // 步骤 H：当前帧读尽则置空，下一轮 while 触发取新帧
        if (current_frame_offset_ >= frame_total_bytes) {
            current_frame_ = nullptr;
            current_frame_offset_ = 0;
        }
    }
}

} // namespace av
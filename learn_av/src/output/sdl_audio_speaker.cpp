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
    // 1. 初始化 SDL 音频子系统 (必须最先执行！)
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        LOG_ERROR("SDL_Init(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
        return false; // 初始化失败应该直接拦截
    }

    sample_rate_ = sample_rate;
    channels_ = channels;

    // 2. 期望的声卡参数 / 实际得到的声卡参数
    SDL_AudioSpec wanted_spec;
    SDL_AudioSpec obtained_spec;

    wanted_spec.freq = sample_rate_; // 采样率
    wanted_spec.format = AUDIO_S16SYS; // 采样格式 强制使用 16位有符号整数，交错排布
    wanted_spec.silence = 0; // 静音值
    wanted_spec.samples = 1024; // 单次回调样本数

    // 3. 绑定回调与指针
    // 告诉 SDL：缺数据了就调这个叫 SDLAudioCallback
    wanted_spec.callback = SDLAudioCallback;
    // 告诉 SDL：调用回调时，请把这个 this 指针作为参数带进去！
    wanted_spec.userdata = this;

    // 4. 打开音频设备 (此时底层开始连接硬件，并把实际参数写回到 obtained_spec)
    audio_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &obtained_spec, 0);
    if (audio_device_id_ == 0) {
        LOG_ERROR("SDL_OpenAudioDevice failed: {}", SDL_GetError());
        return false; // 打开失败应该直接返回
    }

    // 5. 此时环境已就绪，硬件已打开，可以安全地读取驱动和真实参数了
    const char* driver_name = SDL_GetCurrentAudioDriver();
    LOG_INFO("SDL Audio Device opened successfully. Driver: {}, Freq: {}, channels: {}",
             driver_name ? driver_name : "Unknown", obtained_spec.freq, obtained_spec.channels);

    // 重新激活队列
    audio_queue_.Start();

    // -1 的语义是："声卡当前尚未发声，时钟状态未就绪"
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

    // 1. 如果是 Flush 信令
    if (audio_frame->HasFlag(av::AVFrameFlag::kFlush)) {
        LOG_DEBUG("SDLAudioSpeaker intercepted IN-BAND Flush signal. Executing hardware reset.");

        std::lock_guard<std::mutex> lock(sync_mutex_);

        // 1. 暂停音频设备，防止在清空过程中发生硬件回调
        SDL_PauseAudioDevice(audio_device_id_, 1);

        // 2. 暴力清空 SDL 内部硬件缓冲区（防范跳转后出现前一秒的爆音残留）
        SDL_ClearQueuedAudio(audio_device_id_);

        // 3. 清空应用层队列（排空尚未送入硬件的旧数据）
        audio_queue_.Clear();

        // 4. 彻底重置当前正在拆解的帧状态
        current_frame_ = nullptr;
        current_frame_offset_ = 0;

        // 5.设置音频时钟
        audio_clock_us_.store(-1);

        // 6. 如果之前是播放状态，立刻恢复设备运转，准备迎接紧随其后的新声音
        if (is_playing_.load()) {
            SDL_PauseAudioDevice(audio_device_id_, 0);
        }

        LOG_DEBUG("SDLAudioSpeaker IN-BAND flush completed, clock reset to -1.");

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

    // 2. 清空SDL内部缓冲区
    SDL_ClearQueuedAudio(audio_device_id_);

    // 3. 清空应用层队列
    audio_queue_.Clear();

    // 4. 重置当前帧状态
    current_frame_ = nullptr;
    current_frame_offset_ = 0;

    audio_clock_us_.store(-1);
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

    current_frame_ = nullptr;
    current_frame_offset_ = 0;
}

/**
 * @brief SDL 硬件底层回调函数
 * @note 运行在 SDL 独立的后台高级优先级线程中
 */
void SDLAudioSpeaker::SDLAudioCallback(void* userdata, Uint8* stream, int len) {
    // 1. SDL 把我们 Open 时存入的 this 指针通过 userdata 原封不动地还给了我们
    // 2. 我们把它强转回 C++ 的对象指针
    auto* speaker = static_cast<SDLAudioSpeaker*>(userdata);

    // 3. 然后调用这个对象内部的成员函数
    speaker->FillAudioData(stream, len);
}

/**
 * @brief 填充 PCM 数据到硬件音频缓冲区
 * @param stream SDL 硬件底层的音频内存缓冲区首地址
 * @param len 本次硬件回调请求填充的字节总长度
 */
void SDLAudioSpeaker::FillAudioData(Uint8* stream, int len) {
    // 使用 unique_lock 以便在回调业务层前可主动 unlock，避免持锁执行上层逻辑
    std::unique_lock<std::mutex> lock(sync_mutex_);

    // 1. 初始化静音：如果后面拿不到数据，声卡播的就是静音
    SDL_memset(stream, 0, len);

    // 若当前处于暂停状态，直接跳出执行，硬件将输出上述初始化的静音数据
    if (!is_playing_.load()) {
        return;
    }

    // 记录累计写入硬件缓冲区的字节数偏移量
    int total_bytes_written = 0;

    // 循环执行内存填充，直至满足硬件本次回调请求的全部数据长度 (len)
    while (len > 0) {
        // 步骤 A：若当前无可用音频帧（初始状态或上一帧已读取完毕），从线程安全队列中提取新帧
        if (!current_frame_) {
            // 严禁在此处死锁等待
            if (!audio_queue_.TryPop(current_frame_)) {
                static int empty_count = 0;
                if (++empty_count % 100 == 0) {
                    LOG_DEBUG("SDL Thread Starving! Audio queue is empty. Padding silence...");
                }
                return;
            }
            // 成功提取新帧，重置当前帧的内部读取偏移量
            current_frame_offset_ = 0;
        }

        // 处理 EOF
        if (current_frame_->HasFlag(AVFrameFlag::kEOF)) {
            LOG_INFO("SDL Receive EOF");

            audio_clock_us_.store(-1);
            current_frame_ = nullptr;

            // 暂停设备（1 = 暂停），停止后续硬件回调
            SDL_PauseAudioDevice(audio_device_id_, 1);

            // 在临界区之外回调业务层，避免在持有 sync_mutex_ 时执行任意上层逻辑
            // （OnAudioCompleted 会 notify_all 并可能 emit 至 UI，若未来回环触及 speaker 即死锁）
            lock.unlock();
            if (listener_) {
                listener_->OnAudioCompleted();
            }
            return;
        }

        // 更新全局音频时钟
        if (current_frame_ && current_frame_->IsValid()) {
            // 1. 获取这一帧的起始时间 - 微秒
            int64_t base_pts_us = current_frame_->GetTimestampUs();

            // 2. 帧总字节数 = 采样点数 * 通道数 * 采样位深
            int frame_total_bytes = current_frame_->GetRawFrame()->nb_samples * channels_ * 2;

            // 3. 计算这一帧的持续总时长 - 微秒
            // 公式：时长(微秒) = (采样点数 * 1000000) / 采样率
            int64_t frame_duration_us =
                (static_cast<int64_t>(current_frame_->GetRawFrame()->nb_samples) * 1000000LL) / sample_rate_;

            // 4. 计算当前已经被声卡消耗的字节占比
            double consumed_ratio = static_cast<double>(current_frame_offset_) / frame_total_bytes;

            // 5. 当前精准音频时钟 = 帧起始时间 + (帧总时长 * 已消耗占比)
            int64_t current_exact_clock_us =
                base_pts_us + static_cast<int64_t>(static_cast<double>(frame_duration_us) * consumed_ratio);

            // 6. 更新给全局，供视频线程读取
            audio_clock_us_.store(current_exact_clock_us);
        }

        // 步骤 B：计算剩余字节
        int frame_total_bytes = current_frame_->GetRawFrame()->nb_samples * channels_ * 2;
        int remain_bytes = frame_total_bytes - current_frame_offset_;

        // 步骤 C：确定本次内存拷贝的实际大小
        // 取"硬件缓冲区剩余请求长度(len)"与"当前音频帧剩余有效长度(remain_bytes)"的最小值
        int bytes_to_copy = std::min(len, remain_bytes);

        // 步骤 D：执行内存拷贝，将 PCM 数据写入 SDL 硬件缓冲区
        // 目标基址：硬件缓冲区首地址(stream) + 累计已写入偏移量(total_bytes_written)
        // 源基址：当前音频帧 PCM 数据首地址(data[0]) + 当前帧已被读取的偏移量
        memcpy(stream + total_bytes_written, current_frame_->GetRawFrame()->data[0] + current_frame_offset_,
               bytes_to_copy);

        // 步骤 E：更新指针与内存偏移量状态
        len -= bytes_to_copy; // 递减硬件缓冲区的剩余需求长度
        total_bytes_written += bytes_to_copy; // 递增已写入硬件缓冲区的累计总长度
        current_frame_offset_ += bytes_to_copy; // 递增当前音频帧内部的读取偏移量

        // 步骤 F：检验当前音频帧的内存数据是否已被完整读取
        if (current_frame_offset_ >= frame_total_bytes) {
            // 当前帧数据已耗尽，置空智能指针以触发底层 AVFrame 内存释放，并重置偏移量
            // 下次进入 while 循环时将触发 步骤A 提取新帧
            current_frame_ = nullptr;
            current_frame_offset_ = 0;
        }
    }
}

} // namespace av
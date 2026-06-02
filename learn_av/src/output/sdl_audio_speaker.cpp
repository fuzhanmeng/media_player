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
std::unique_ptr<IAudioSpeaker> IAudioSpeaker::Create() {
    LOG_INFO("SDLAudioSpeaker Create...");
    return std::make_unique<SDLAudioSpeaker>();
}

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
    // 强制使用 16位有符号整数，交错排布
    wanted_spec.format = AUDIO_S16SYS; // 采样格式
    wanted_spec.silence = 0; // 静音值
    wanted_spec.samples = 1024; // 单次回调样本数

    // 3. 绑定回调与指针
    // 告诉 SDL：缺数据了就调这个叫 SDLAudioCallback 的静态函数
    wanted_spec.callback = SDLAudioCallback;
    // 告诉 SDL：调用回调时，请把这个 this 指针作为参数带进去！
    wanted_spec.userdata = this;

    // 4. 打开音频设备 (此时底层开始连接硬件，并把实际参数写回到 obtained_spec)
    audio_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &obtained_spec, 0);
    if (audio_device_id_ == 0) {
        LOG_ERROR("SDL_OpenAudioDevice failed: {}", SDL_GetError());
        return false; // 打开失败应该直接拦截
    }

    // 5. 此时环境已就绪，硬件已打开，可以安全地读取驱动和真实参数了！
    const char* driver_name = SDL_GetCurrentAudioDriver();
    LOG_INFO("SDL Audio Device opened successfully. Driver: {}, Freq: {}, channels: {}",
             driver_name ? driver_name : "Unknown", obtained_spec.freq, obtained_spec.channels);

    // 重新激活队列
    audio_queue_.Start();
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
 * @param audio_frame 纯净 PCM 帧
 */
void SDLAudioSpeaker::PushAudioData(std::shared_ptr<Frame> audio_frame) {
    if (audio_frame->IsEmpty()) {
        LOG_INFO("the audio_frame is empty");
    }

    if (audio_frame && !audio_frame->IsEmpty()) {
        // 加上日志：每收到 50 帧打印一次，证明快递员(Player)确实送货了
        static int push_count = 0;
        if (++push_count % 50 == 0) {
            LOG_INFO("Speaker received 50 frames from Player. Queue size: {}", audio_queue_.Size());
        }
        audio_queue_.Push(std::move(audio_frame));
    } else {
        LOG_ERROR("SDLAudioSpeaker received invalid audio frame!");
    }
}

/**
 * @brief 清空内部缓存（用于 Seek 时防范历史声音串扰）
 */
void SDLAudioSpeaker::Flush() {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    audio_queue_.Clear();

    // 清空当前正在被剥削的帧和进度
    current_frame_ = nullptr;
    current_frame_offset_ = 0;
}

/**
 * @brief SDL 硬件底层静态回调函数
 * @note 运行在 SDL 独立的后台高级优先级线程中
 */
void SDLAudioSpeaker::SDLAudioCallback(void* userdata, Uint8* stream, int len) {
    // 1. SDL 把我们 Open 时存入的 this 指针通过 userdata 原封不动地还给了我们
    // 2. 我们把它强转回 C++ 的对象指针
    auto* speaker = static_cast<SDLAudioSpeaker*>(userdata);

    // 3. 然后调用这个对象内部的成员函数，完美回到了 C++ 的面向对象世界！
    speaker->FillAudioData(stream, len);
}

/**
 * @brief 填充 PCM 数据到硬件音频缓冲区
 * @param stream SDL 硬件底层的音频内存缓冲区首地址
 * @param len 本次硬件回调请求填充的字节总长度
 */
void SDLAudioSpeaker::FillAudioData(Uint8* stream, int len) {
    // 加锁，防止 Flush 操作与回调读取发生数据竞争
    std::lock_guard<std::mutex> lock(sync_mutex_);

    // 1. 初始化静音：如果后面拿不到数据，声卡播的就是静音，而不是上一秒残留的刺耳爆音
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
            // 严禁在此处死锁等待，否则将导致操作系统的音频硬件线程发生死锁阻塞
            if (!audio_queue_.TryPop(current_frame_)) {
                static int empty_count = 0;
                if (++empty_count % 100 == 0) {
                    LOG_DEBUG("SDL Thread Starving! Audio queue is empty. Padding silence...");
                }
                return;
            }
            // 成功提取新帧，重置当前帧的内部读取偏移量
            current_frame_offset_ = 0;

            // 成功拿到数据，执行了内存拷贝
            static int pop_count = 0;
            if (++pop_count % 50 == 0) {
                LOG_INFO("SDL successfully popped & copied 50 frames to hardware buffer!");
            }
        }

        // 步骤 B：计算当前音频帧的剩余有效字节数
        // 帧总字节数 = 采样点数 * 通道数 * 采样位深
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
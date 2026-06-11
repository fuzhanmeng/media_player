/**
 * @file sdl_audio_speaker.h
 * @brief SDL音频输出实现
 */

#include <SDL2/SDL.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "define/av_frame.h"
#include "interface/iaudio_speaker.h"
#include "utils/thread_safe_queue.h"

namespace av {

class SDLAudioSpeaker : public IAudioSpeaker {
public:
    SDLAudioSpeaker();
    ~SDLAudioSpeaker() override;

    /**
     * @brief 初始化扬声器硬件
     * @param sample_rate 目标采样率 (例如 44100)
     * @param channels 目标声道数 (例如 2)
     * @return true 初始化成功；false 初始化失败
     */
    bool Open(int sample_rate, int channels) override;

    /**
     * @brief 启动音频物理设备播放
     */
    void Play() override;

    /**
     * @brief 暂停音频物理设备播放
     */
    void Pause() override;

    /**
     * @brief 关闭设备并释放硬件资源
     */
    void Close() override;

    /**
     * @brief 接收解码后的 PCM 音频帧并放入硬件缓冲队列
     * @param audio_frame 纯净 PCM 帧
     */
    void PushAudioFrame(std::shared_ptr<Frame> audio_frame) override;

    /**
     * @brief 清空内部缓存（用于 Seek 时防范历史声音串扰）
     */
    void Flush() override;

    /**
     * @brief 唤醒队列
     */
    void Abort() override;

    /**
     * @brief 清空队列，唤醒阻塞
     */
    void AbortAndClearQueue() override;

    /**
     * @brief 启动队列
     */
    void StartQueue() override;

    /**
     * @brief 获取当前精准的音频播放时钟（微秒）
     * @note 供音画同步模块调用以音频为基准进行对齐
     */
    int64_t GetAudioClockUs() const override;

    /**
     * @brief 获取当前精准的音频播放时钟（秒）
     * @note 用于UI显示
     */
    double GetAudioClock() const override;

    /**
     * @brief 重置时钟（用于跳转时进度条更新）
     */
    void ResetAlock() override;

private:
    /**
     * @brief SDL 硬件底层静态回调函数
     * @note 运行在 SDL 独立的后台高级优先级线程中
     */
    static void SDLAudioCallback(void* userdata, Uint8* stream, int len);

    /**
     * @brief 填充PCM
     */
    void FillAudioData(Uint8* stream, int len);

private:
    SDL_AudioDeviceID audio_device_id_{0};

    // 音频物理参数
    int sample_rate_{44100};
    int channels_{2};

    // 线程安全队列，用于接收 Player 送来的 PCM 帧
    ThreadSafeQueue<std::shared_ptr<Frame>> audio_queue_;

    // 消费状态维护：记录当前正在被切片消费的帧
    std::shared_ptr<Frame> current_frame_{nullptr};
    int current_frame_offset_{0}; ///< 当前帧已经被声卡消费了多少字节

    std::mutex sync_mutex_;
    std::atomic<bool> is_playing_{false};

    // 设计原则：内部计算全部使用整数，对外提供浮点接口
    std::atomic<int64_t> audio_clock_us_{-1};
};

} // namespace av
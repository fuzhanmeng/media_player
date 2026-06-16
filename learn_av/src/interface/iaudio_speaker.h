/**
 * @file iaudio_speaker.h
 * @brief 音频物理输出设备接口定义
 */
#pragma once

#include <memory>

#include "define/av_frame.h"

namespace av {

class IAudioSpeaker {
public:
    /**
     * @brief 创建音频播放类实例
     */
    static std::unique_ptr<IAudioSpeaker> Create();

    /**
     * @brief 析构函数
     */
    virtual ~IAudioSpeaker() = default;

    /**
     * @brief 初始化扬声器硬件
     * @param sample_rate 目标采样率 (例如 44100)
     * @param channels 目标声道数 (例如 2)
     * @return true 初始化成功；false 初始化失败
     */
    virtual bool Open(int sample_rate, int channels) = 0;

    /**
     * @brief 启动音频物理设备播放
     */
    virtual void Play() = 0;

    /**
     * @brief 暂停音频物理设备播放
     */
    virtual void Pause() = 0;

    /**
     * @brief 关闭设备并释放硬件资源
     */
    virtual void Close() = 0;

    /**
     * @brief 接收解码后的 PCM 音频帧并放入硬件缓冲队列
     * @param audio_frame 纯净 PCM 帧
     */
    virtual void PushAudioFrame(std::shared_ptr<Frame> audio_frame) = 0;

    /**
     * @brief 清空内部缓存（用于 Seek 时防范历史声音串扰）
     */
    virtual void Flush() = 0;

    /**
     * @brief 唤醒队列
     */
    virtual void Abort() = 0;


    /**
     * @brief 清空队列，唤醒阻塞
     */
    virtual void AbortAndClearQueue() = 0;

    /**
     * @brief 启动队列
     */
    virtual void StartQueue() = 0;


    /**
     * @brief 获取当前精准的音频播放时钟（微秒）
     * @note 供音画同步模块调用以音频为基准进行对齐
     */
    virtual int64_t GetAudioClockUs() const = 0;

    /**
     * @brief 获取当前精准的音频播放时钟（秒）
     * @note 用于UI显示
     */
    virtual double GetAudioClock() const = 0;
};

} // namespace av
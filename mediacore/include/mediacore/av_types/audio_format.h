/**
 * @file audio_format.h
 * @brief 音频帧使用的基础值对象：采样率 / 声道数 / 每个采样率占用的字节数
 * @details
 * 这个类型主要是用于描述一帧 packed PCM 音频的最基本布局，不会引入 FFmpeg 的 sample format
 */
#pragma once

namespace mediacore {
/**
 * @struct AudioFormat
 * @brief 音频帧布局
 * @details
 * sample_rate_hz_     采样率，单位 HZ，每个声道每秒采集多少个样本点
 * channel_count_      声道数
 * bytes_per_sample_   单个采样点占多少字节
 */ 
struct AudioFormat {
    int sample_rate_hz_{0};  ///< 采样率，单位 Hz，如44100
    int channel_count_{0};  ///< 声道数，如 1、2、6
    int bytes_per_sample_{0};  ///< 单个采样点占用的字节数，如 S16 为 2

    /**
     * @brief 判断音频布局是否有效
     * @return 有效返回 true，无效返回 false，三个字段都 > 0 才表示有效
     */
    bool IsValid() const { return sample_rate_hz_ > 0 && channel_count_ > 0 && bytes_per_sample_ > 0; }
};
}  // namespace mediacore
/**
 * @file audio_format.h
 * @brief 音频帧布局值对象：采样率 / 声道数 / 单个采样点占用的字节数
 * @details packed PCM 音频帧的基础布局描述，不引入 FFmpeg 的 sample format
 */
#pragma once

namespace mediacore {
/**
 * @struct AudioFormat
 * @brief 音频帧布局
 */
struct AudioFormat {
    int sample_rate_hz_{0};  ///< 采样率，单位 Hz，如44100
    int channel_count_{0};  ///< 声道数，如 1、2、6
    int bytes_per_sample_{0};  ///< 单个采样点占用的字节数，如 S16 为 2

    /**
     * @brief 判断音频布局是否有效
     * @return 三个字段均 > 0 时返回 true
     */
    bool IsValid() const { return sample_rate_hz_ > 0 && channel_count_ > 0 && bytes_per_sample_ > 0; }
};
}  // namespace mediacore

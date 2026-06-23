/**
 * @file fileWriterParameters.h
 * @brief 音视频参数
 */
#pragma once

namespace av {

struct FileWriterParameters {
    /// 音频
    int sample_rate_{44100};
    int channels_{2};
    int audio_bit_rate_{128000};

    /// 视频
    int width_{1080};
    int height_{1920};
    int fps_{30};
    int video_bit_rate_{2000000}; /// H.264码率，默认2Mbps
};
} // namespace av
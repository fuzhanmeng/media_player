/**
 * @file stream_info.h
 * @brief 媒体流参数 DTO
 */
#pragma once

#include <cstdint>
#include <vector>

namespace av {

struct StreamInfo {
    int stream_index_{-1}; ///< 流索引
    int codec_id_{0}; ///< 编码器 ID (对应 AVCodecID)

    // 视频参数
    int width_{0}; ///< 视频宽度
    int height_{0}; ///< 视频高度

    // 音频参数
    int sample_rate_{0}; ///< 采样率
    int channels_{0}; ///< 通道数

    // 时序参数
    int32_t timebase_num_{1}; ///< 时间基分子
    int32_t timebase_den_{1}; ///< 时间基分母

    // 编码器私有数据 (如 H.264 的 SPS/PPS)
    std::vector<uint8_t> extra_data_;
};

} // namespace av
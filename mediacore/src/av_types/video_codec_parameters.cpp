/**
 * @file video_codec_parameters.cpp
 * @brief 保存一份可以交给视频解码器使用的 FFmpeg 参数
 */

#include "video_codec_parameters.h"

#include <utility>

#include "mediacore/logger.h"

extern "C" {
#include <libavcodec/codec_par.h>
}

namespace mediacore {

/**
 * @brief 创建一个不包含参数的空对象
 */
VideoCodecParameters::~VideoCodecParameters() {
    LOG_TRACE("VideoCodecParameters released: {}", static_cast<const void*>(av_parameters_));

    avcodec_parameters_free(&av_parameters_);
}

/**
 * @brief 移动构造函数，使用一个对象去初始化另一个对象
 * @param other 被移动的对象，调整后被置空
 */
VideoCodecParameters::VideoCodecParameters(VideoCodecParameters&& other) noexcept
    : av_parameters_(std::exchange(other.av_parameters_, nullptr)) {}

/**
 * @brief 移动赋值运算符，将一个对象资源转移到另一个对象
 * @param other 被转移资源的对象
 */
VideoCodecParameters& VideoCodecParameters::operator=(VideoCodecParameters&& other) noexcept {
    if (this == &other) {
        LOG_ERROR("Cannot self-replicate: this == &other");
        return *this;
    }

    // 1. 释放资源
    avcodec_parameters_free(&av_parameters_);

    // 2. 转移资源并置空
    av_parameters_ = std::exchange(other.av_parameters_, nullptr);

    LOG_TRACE("VideoCodecParameters move-assigned: {}", static_cast<const void*>(av_parameters_));
    return *this;
}

}  // namespace mediacore
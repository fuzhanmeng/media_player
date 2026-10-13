/**
 * @file audio_codec_parameters.cpp
 * @brief 深拷贝的音频 AVCodecParameters 的 RAII 资源管理类的具体实现
 */

#include "av_types/audio_codec_parameters.h"

#include <memory>
#include <stdexcept>

extern "C" {
#include <libavcodec/codec_par.h>
}

namespace mediacore {

/**
 * @brief 删除器
 */
void AudioCodecParameters::Deleter::operator()(AVCodecParameters* p) const noexcept {
    avcodec_parameters_free(&p);  // FFmpeg 允许 *p 为空
}

/**
 * @brief 移动构造函数的实现
 */
AudioCodecParameters::AudioCodecParameters(AudioCodecParameters&&) noexcept = default;

/**
 * @brief 移动赋值运算符的实现
 */
AudioCodecParameters& AudioCodecParameters::operator=(AudioCodecParameters&&) noexcept = default;

/**
 * @brief 对外部传入的音频编解码参数 source 执行深拷贝操作
 * @param source 指向源 AVCodecParameters 的指针，如 AVStream::codecpar
 * @return 拥有独立副本所有权的 AudioCodecParameters 实例
 * @throw std::invalid_argument 输入 source 指针为空时抛出
 * @throw std::bad_alloc 分配 AVCodecParameters 结构体显示内存失败时抛出
 * @throw std::runtime_error 调用 avcodec_parameters_copy 拷贝参数或 extradata 失败时抛出
 * @note 内部使用 avcodec_parameters_copy，保证包括 extradata 二进制缓冲在内的完整复制
 */
AudioCodecParameters AudioCodecParameters::CopyFrom(const AVCodecParameters* source) {
    if (source == nullptr) {
        throw std::invalid_argument("AudioCodecParameters: source pointer is nullptr");
    }

    // 1. 申请音频参数内存
    AudioCodecParameters result;
    result.audio_parameters_.reset(avcodec_parameters_alloc());

    // 2. 申请失败
    if (!result.audio_parameters_) {
        throw std::bad_alloc();
    }

    // 3. 申请成功
    if (avcodec_parameters_copy(result.audio_parameters_.get(), source) < 0) {
        throw std::runtime_error("AudioCodecParameters: avcodec_parameters_copy failed");
    }

    return result;
}

}  // namespace mediacore
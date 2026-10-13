/**
 * @file video_codec_parameters.cpp
 * @brief 保存一份可以交给视频解码器使用的 FFmpeg 参数
 */

#include "av_types/video_codec_parameters.h"

#include <memory>
#include <stdexcept>

extern "C" {
#include <libavcodec/codec_par.h>
}

namespace mediacore {

VideoCodecParameters::~VideoCodecParameters() = default;

/**
 * @brief 自定义的删除器
 */
void VideoCodecParameters::Deleter::operator()(AVCodecParameters* p) const noexcept {
    avcodec_parameters_free(&p);
}

/**
 * @brief 移动构造函数，使用一个对象去初始化另一个对象
 * @param other 被移动的对象，调整后被置空
 */
VideoCodecParameters::VideoCodecParameters(VideoCodecParameters&& other) noexcept = default;

/**
 * @brief 移动赋值运算符，将一个对象资源转移到另一个对象
 * @param other 被转移资源的对象
 */
VideoCodecParameters& VideoCodecParameters::operator=(VideoCodecParameters&& other) noexcept = default;

/**
 * @brief 对外部传入的音频编解码参数 source 执行深拷贝操作
 * @param source 指向源 AVCodecParameters 的指针，如 AVStream::codecpar
 * @return 拥有独立副本所有权的 AudioCodecParameters 实例
 * @throw std::invalid_argument 输入 source 指针为空时抛出
 * @throw std::bad_alloc 分配 AVCodecParameters 结构体显示内存失败时抛出
 * @throw std::runtime_error 调用 avcodec_parameters_copy 拷贝参数或 extradata 失败时抛出
 * @note 内部使用 avcodec_parameters_copy，保证包括 extradata 二进制缓冲在内的完整复制
 */
VideoCodecParameters VideoCodecParameters::CopyFrom(const AVCodecParameters* source) {
    if (source == nullptr) {
        throw std::invalid_argument("VideoCodecParameters: source pointer is nullptr");
    }

    // 1. 申请临时的视频参数内存
    VideoCodecParameters result;
    result.video_parameters_.reset(avcodec_parameters_alloc());

    // 2. 申请失败
    if (!result.video_parameters_) {
        throw std::bad_alloc();
    }

    // 3. 申请成功
    if (avcodec_parameters_copy(result.video_parameters_.get(), source) < 0) {
        // 拷贝失败
        throw std::runtime_error("VideoCodecParameters: avcodec_parameters_copy failed");
    }

    return result;
}
}  // namespace mediacore
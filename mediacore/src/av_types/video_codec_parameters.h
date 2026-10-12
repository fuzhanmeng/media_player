/**
 * @file video_codec_parameters.h
 * @brief 保存一份可以交给视频解码器使用的 FFmpeg 参数
 * @details
 * FFmpeg 把一条流的编码参数放在 AVStream::codecpar 中
 * Demuxer 打开媒体时会把视频流的详细参数保存一份，存入这个类
 * Decoder 使用这份副本创建 AVCodecContext，因此 Decoder 不需要保存 Demuxer 内部的 AVstream 指针
 */

#pragma once

struct AVCodecParameters;

namespace mediacore {

class FfmpegDemuxer;

/**
 * @class VideoCodecParameters
 * @brief 独占一份 AVCodecParameters 的小包装类
 * @details
 * AVCodecParameters 本身是 FFmpeg 分配的 C 结构体，这里保存了其指针，并负责在析构时调用
 * avcodec_parameters_free()，类外只能读取参数，不可复制这份资源
 */
class VideoCodecParameters {
public:
    /**
     * @brief 创建一个不包含参数的空对象
     * @details 空对象的 Get() 返回 nullptr，IsValid() 返回 false
     */
    VideoCodecParameters() noexcept = default;

    /**
     * @brief 释放自己拥有的 FFmpeg 参数
     * @details 如果对象为空则什么也不做
     */
    ~VideoCodecParameters();

    /**
     * @brief 禁止复制
     */
    VideoCodecParameters(const VideoCodecParameters&) = delete;

    /**
     * @brief 禁止移动复制
     */
    VideoCodecParameters& operator=(const VideoCodecParameters&) = delete;

    /**
     * @brief 移动构造函数，用另一个对象的资源初始化当前对象
     * @param other 被移动的对象
     */
    VideoCodecParameters(VideoCodecParameters&& other) noexcept;

    /**
     * @brief 移动赋值运算符，用另一个对象的资源替换当前对象的资源
     * @param other 被移动的对象
     */
    VideoCodecParameters& operator=(VideoCodecParameters&& other) noexcept;

    /**
     * @brief 返回底层的参数指针，空对象返回 nullptr
     * @return 有参数时返回只读指针；空对象返回 nullptr
     */
    const AVCodecParameters* Get() const noexcept { return av_parameters_; }

    /**
     * @brief 判断是否持有参数副本
     * @return 持有返回 true，否则返回 false
     */
    bool IsValid() const noexcept { return av_parameters_ != nullptr; }

private:
    friend class FfmpegDemuxer;

    /**
     * @brief 接收一块已经由 FFmpeg 分配好的参数内存
     * @param parameters 要交给本对象管理的指针，可以为空
     */
    explicit VideoCodecParameters(AVCodecParameters* av_parameters) noexcept : av_parameters_(av_parameters) {}

    AVCodecParameters* av_parameters_{nullptr};  ///< 当前对象独自负责释放的参数指针
};
}  // namespace mediacore

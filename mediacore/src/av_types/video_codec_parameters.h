/**
 * @file video_codec_parameters.h
 * @brief 保存一份可以交给视频解码器使用的 FFmpeg 参数
 * @details
 * FFmpeg 把一条流的编码参数放在 AVStream::codecpar 中
 * Demuxer 打开媒体时会把视频流的详细参数保存一份，存入这个类
 * Decoder 使用这份副本创建 AVCodecContext，因此 Decoder 不需要保存 Demuxer 内部的 AVstream 指针
 */

#pragma once

#include <memory>

struct AVCodecParameters;

namespace mediacore {

/**
 * @class VideoCodecParameters
 * @brief 独占型视频编解码参数封装容器
 * @details 提供了只读访问接口，封装了 FFmpeg 底层 AVcodecParameters 的深拷贝与生命周期管理
 */
class VideoCodecParameters {
public:
    /**
     * @brief 默认构造函数
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
     * @brief 将 Demuxer 解复用得到的音频编码参数拷贝过来
     * @param source 音频编码参数来源
     */
    static VideoCodecParameters CopyFrom(const AVCodecParameters* source);

    /**
     * @brief 返回底层的参数指针，空对象返回 nullptr
     * @return 有参数时返回只读指针；空对象返回 nullptr
     */
    [[nodiscard]] const AVCodecParameters* Get() const noexcept { return video_parameters_.get(); }

    /**
     * @brief 判断是否持有参数副本
     * @return 持有返回 true，否则返回 false
     */
    [[nodiscard]] bool IsValid() const noexcept { return video_parameters_ != nullptr; }

    /**
     * @brief 显式布尔类型转换重载，等价于 IsValid()
     */
    explicit operator bool() const noexcept { return IsValid(); }

    /**
     * @brief 重载箭头运算符，提供对底层结构体成员的直接只读访问
     */
    const AVCodecParameters* operator->() const noexcept { return video_parameters_.get(); }

private:
    /**
     * @struct Deleter
     * @brief 适配 avcodec_parameters_free 的自定义释放仿函数
     */
    struct Deleter {
        void operator()(AVCodecParameters* p) const noexcept;
    };

    std::unique_ptr<AVCodecParameters, Deleter> video_parameters_{nullptr};
};
}  // namespace mediacore

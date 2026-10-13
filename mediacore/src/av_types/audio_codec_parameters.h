/**
 * @file audio_codec_parameters
 * @brief 深拷贝的音频 AVCodecParameters 的 RAII 资源管理类，用于保存 Demuxer 后得到的音频
 * 编码参数，以便后续打开音频解码器时使用
 * @details
 * 1. 生命周期解耦：从AVStream->codecpar 完整的深拷贝的结构体，完全解耦 FFmpeg
 * 2. 独占语义：仅支持移动构造和移动赋值，构建完毕后移交给唯一的音频解码实例
 * 3. RAII 自动化回收：内部封装了 avcodec_parameters_free，生命周期终止时自动释放内存
 */

#pragma once

#include <memory>

struct AVCodecParameters;
namespace mediacore {

/**
 * @class AudioCodecParameters
 * @brief 独占型音频编解码参数封装容器
 * @details 
 * 提供了只读访问接口，封装了 FFmpeg 底层 AVcodecParameters 的深拷贝与生命周期管理
 */
class AudioCodecParameters {
public:
    /**
     * @brief 默认构造函数
     */
    AudioCodecParameters() noexcept = default;

    /**
     * @brief 默认析构函数
     */
    ~AudioCodecParameters() = default;

    /**
     * @brief 禁止拷贝构造函数
     */
    AudioCodecParameters(const AudioCodecParameters&) = delete;

    /**
     * @brief 禁止拷贝赋值运算符
     */
    AudioCodecParameters& operator=(const AudioCodecParameters&) = delete;

    /**
     * @brief 移动构造函数
     */
    AudioCodecParameters(AudioCodecParameters&&) noexcept;

    /**
     * @brief 移动赋值运算符
     */
    AudioCodecParameters& operator=(AudioCodecParameters&&) noexcept;

    /**
     * @brief 将 Demuxer 解复用得到的音频编码参数拷贝过来
     * @param source 音频编码参数来源
     */
    static AudioCodecParameters CopyFrom(const AVCodecParameters* source);

    /**
     * @brief 获取 AVCodecParameter 类的只读指针
     */
    [[nodiscard]] const AVCodecParameters* Get() const noexcept { return audio_parameters_.get(); }

    /**
     * @brief 校验当前对象内部是否包含有效的参数数据
     * @return 存在有效参数返回 true，不存在有效参数返回 false
     */
    [[nodiscard]] bool IsValid() const noexcept { return audio_parameters_ != nullptr; }

    /**
     * @brief 显示布尔类型转换重载。等价于 IsValid
     */
    explicit operator bool() const noexcept { return IsValid(); }

    /**
     * @brief 重载箭头运算符，提供对底层结构体成员的直接只读接口
     */
    const AVCodecParameters* operator->() const noexcept { return audio_parameters_.get(); }

private:
    /**
     * @struct Deleter
     * @brief 适配 avcodec_parameters_free 的自定义释放仿函数
     */
    struct Deleter {
        void operator()(AVCodecParameters* p) const noexcept;
    };

    std::unique_ptr<AVCodecParameters, Deleter> audio_parameters_{nullptr};
};
}  // namespace mediacore
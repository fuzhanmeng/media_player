/**
 * @file audio_format.h
 * @brief Packed PCM 音频帧内存物理布局描述值类型定义
 * @details
 * 负责描述交错排布（Packed）PCM 音频数据的几何与量化属性：
 * 1. 基础三要素：采样率（Sample Rate）、声道数（Channels）、单个采样点字节量化深度（Bytes Per Sample）；
 * 2. 依赖隔离：纯标准库构建，不引入 FFmpeg 的 AVSampleFormat 枚举或第三方数据结构；
 * 3. 强值语义：具备完整的 constexpr / noexcept 保证与默认相等性比较支持。
 */

#pragma once

namespace mediacore {

/**
 * @struct AudioFormat
 * @brief Packed PCM 音频布局描述结构体
 */
struct AudioFormat final {
    int sample_rate_hz_{0};  ///< 采样频率（单位：Hz，例如 44100、48000）
    int channel_count_{0};  ///< 声道总数（例如 1 代表单声道，2 代表双声道，6 代表 5.1 环绕声）
    int bytes_per_sample_{0};  ///< 单个声道单个采样点占用的字节数（例如 S16 为 2，Float/S32 为 4）

    /**
     * @brief 校验音频物理布局三要素是否均为合法的正数
     * @return 采样率、声道数与采样点字节数均严格大于 0 时返回 true，否则返回 false
     */
    constexpr bool IsValid() const noexcept {
        return sample_rate_hz_ > 0 && channel_count_ > 0 && bytes_per_sample_ > 0;
    }

    /**
     * @brief 计算跨所有声道的一个完整音频帧（Audio Frame）所占字节大小
     * @return 格式合法时返回 channel_count_ * bytes_per_sample_，否则安全返回 0
     * @note 音频领域中 1 Frame = 包含所有声道各自采样点的一个集合点
     */
    constexpr int BytesPerFrame() const noexcept { return IsValid() ? (channel_count_ * bytes_per_sample_) : 0; }

    /**
     * @brief 计算每秒播放产生的 PCM 原始字节吞吐率（Byte Rate）
     * @return 格式合法时返回 sample_rate_hz_ * BytesPerFrame()，否则安全返回 0
     * @note 常用于根据缓冲时长（毫秒）换算所需内存队列字节大小
     */
    constexpr int BytesPerSecond() const noexcept { return sample_rate_hz_ * BytesPerFrame(); }

    /**
     * @brief 相等性比较操作符
     */
    constexpr bool operator==(const AudioFormat& other) const noexcept {
        return sample_rate_hz_ == other.sample_rate_hz_ && channel_count_ == other.channel_count_ &&
            bytes_per_sample_ == other.bytes_per_sample_;
    }

    /**
     * @brief 不等性比较操作符
     */
    constexpr bool operator!=(const AudioFormat& other) const noexcept { return !(*this == other); }
};

}  // namespace mediacore
/**
 * @file pcm_frame.h
 * @brief 解码/重采样与底层音频输出（Audio Sink）之间的标准化 PCM 载荷定义
 * @details
 * 统一规范音频管线内传递的PCM音频帧格式
 * 1. 固定格式：48000 Hz 采样率、S16LE、双声道立体声、交错排布
 * 2. 采样级精准时钟：使用起始采样点绝对索引表示播放时间
 * 3. 纪元生命周期隔离（epoch_）：记录流生成代际，支持 Seek 之后快速过滤历史滞留帧
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mediacore {

/**
 * @struct PcmFrame
 * @brief 保存一段 PCM 声音数据，以及这段声音的播放位置
 * @details
 * 一个 PcmFrame 可以包含多个采样帧，时长由数据量决定
 * PcmFrame 通过 std::move 移动数据，避免重复复制
 */
struct PcmFrame {
    static constexpr int kSampleRate = 48000;  ///< 每秒 48000 个采样时刻
    static constexpr int kChannels = 2;  ///< 标准双声道立体声
    static constexpr int kMaxBlockFrames = 240; ///< 每个 PCM 块最多包含 240 采样帧

    std::vector<std::int16_t> samples_;  ///< 交错排列的 PCM 原始采样点数据
    std::int64_t start_sample_{0};  ///< 本数据块在媒体时间轴上的起始位置，以采样帧为单位
    std::uint64_t epoch_{0};  ///< 流版本世代号，用于 Seek 过后丢弃过期的版本号

    /**
     * @brief 获取本数据块包含多少组同时刻的左右声道数据
     * @return 采样帧数，即采样值总数除以声道数
     * @note 例如，480 个数值组成 240 组左右声道数据，返回 240。
     */ 
    [[nodiscard]] std::size_t FrameCount() const noexcept { return samples_.size() / kChannels; }

    /**
     * @brief 检查数据是否非空，并且左右声道能够成对排列
     * @return 若样本数量 > 0 且为声道数的整数倍返回 true
     */
    [[nodiscard]] bool IsValid() const noexcept { return !samples_.empty() && (samples_.size() % kChannels == 0); }

    /**
     * @brief 把采样帧的位置或数量换算成微秒
     * @param samples 采样帧编号或采样帧数量，每 48000 个对应 1 秒
     * @return 对应的时间；不足 1 微秒的部分舍去，负值向 0 取整
     * @details
     * 例如，48000 对应 1000000 微秒，240 对应 5000 微秒
     * 先分别计算整数秒和不足一秒的部分，避免直接将输入乘以 100 万
     * 使用累计采样帧数换算时间，可以避免反复累加已取整的时长带来的误差
     * @pre 输入须使换算结果处于 64 位有符号整数范围内；函数不检查超范围输入
     */
    [[nodiscard]] static std::int64_t SamplesToUs(std::int64_t samples) noexcept {
        return (samples / kSampleRate) * 1000000 + (samples % kSampleRate) * 1000000 / kSampleRate;
    }

    /**
     * @brief 获取这段声音在媒体时间轴上的起始时间（PTS）
     * @return 起始位置对应的微秒数
     * @note 声音可能仍在设备缓冲区中等待播放；实际播放进度还需结合缓冲区剩余数据计算
     */
    [[nodiscard]] std::int64_t PtsUs() const noexcept { return SamplesToUs(start_sample_); }

    /**
     * @brief 获取这段声音按正常速度播放时的时长
     * @return 时长，单位为微秒： 如 240 个采样帧对应 5000 微秒，即 5 毫秒
     */
    [[nodiscard]] std::int64_t DurationUs() const noexcept {
        return SamplesToUs(static_cast<std::int64_t>(FrameCount()));
    }
};

}  // namespace mediacore
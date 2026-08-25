/**
 * @file media_data_flag.h
 * @brief packet 以及 frame 的标志位封装函数
 * @details
 * 定义帧级别的元信息标志（kKeyFrame/Flush），被 Packet/VideoFrame/AudioFrame共享
 * 使用 enum class 而非普通 enum，避免隐式转换为 int
 */

#pragma once

#include <cstdint>

namespace mediacore {

/**
 * @enum MediaDataFlag
 * @brief 帧标志位，支持位运算组合
 * @details 用于标记音视频帧的元信息和控制信令
 */
enum class MediaDataFlag : uint32_t {
    kNone = 0,  ///< 无标志（默认）
    kKeyFrame = 1 << 0,  ///< 关键帧（I/IDR 帧，可独立解码，无需参考其他帧）
    kFlush = 1 << 1,  ///< Seek 后刷新标记（通知解码器清空状态，丢弃旧帧）
};

/**
 * @brief 位或运算：组合多个标志
 * @details 用法： MediaDataFlag::kKeyFrame | MediaDataFlag::kFlush
 */
constexpr MediaDataFlag operator|(MediaDataFlag a, MediaDataFlag b) {
    return static_cast<MediaDataFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/**
 * @brief 位与运算：检查标志是否存在
 * @details 用法：(flags & MediaDataFlag::kFlush) != MediaDataFlag::kNone
 */
constexpr MediaDataFlag operator&(MediaDataFlag a, MediaDataFlag b) {
    return static_cast<MediaDataFlag>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/**
 * @brief 位非运算：翻转标志
 * @details 用法：~MediaDataFlag::kKeyFrame（生成除 kKeyFrame 外的所有位）
 */
constexpr MediaDataFlag operator~(MediaDataFlag a) {
    return static_cast<MediaDataFlag>(~static_cast<uint32_t>(a));
}

/**
 * @brief 位或赋值：追加标志
 * @details 用法：flags |= MediaDataFlag::kFlush
 */
inline MediaDataFlag& operator|=(MediaDataFlag& a, MediaDataFlag b) {
    a = a | b;
    return a;
}

/**
 * @brief 位与赋值：保留指定标志（常与 ~ 配合移除标志）
 * @details 用法：flags &= ~MediaDataFlag::kKeyFrame（移除 kKeyFrame 标志）
 */
inline MediaDataFlag& operator&=(MediaDataFlag& a, MediaDataFlag b) {
    a = a & b;
    return a;
}

}  // namespace mediacore

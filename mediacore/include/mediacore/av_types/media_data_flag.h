/**
 * @file media_data_flag.h
 * @brief 媒体数据包（Packet）与媒体帧（Frame）通用标志位定义与位运算操作符重载
 * @details
 * 负责定义数据包与像素帧的元信息属性及流控状态标记：
 * 1. 作用范围：被 Packet、VideoFrame、AudioFrame 共同依赖并共享；
 * 2. 强类型安全：基于 enum class 定义，防止隐式转换为整数引发逻辑污染；
 * 3. 紧凑存储：底层类型采用 std::uint8_t，消除冗余内存开销并规避 Clang-Tidy 尺寸警告。
 */

#pragma once

#include <cstdint>

namespace mediacore {

/**
 * @enum MediaDataFlag
 * @brief 媒体流载荷的元数据与状态控制标志位（支持按位组合）
 * @note 使用 std::uint8_t 作为紧凑底层类型，最多支持扩展至 8 个标志位
 */
enum class MediaDataFlag : std::uint8_t {
    kNone = 0,  ///< 无标志（初始默认状态）
    kKeyFrame = 1 << 0,  ///< 关键帧标记（I 帧/IDR 帧，可独立解码，无需参考其它帧）
    kFlush = 1 << 1,  ///< 状态刷新标记（指示解码器清空内部参考帧缓存并重置时钟基准）
};

/**
 * @brief 位或运算：组合多个标志位
 * @param a 左操作数
 * @param b 右操作数
 * @return 合并后的标志位组合
 */
constexpr MediaDataFlag operator|(MediaDataFlag a, MediaDataFlag b) noexcept {
    return static_cast<MediaDataFlag>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/**
 * @brief 位与运算：掩码过滤
 * @param a 左操作数
 * @param b 掩码标志
 * @return 匹配保留的标志位
 */
constexpr MediaDataFlag operator&(MediaDataFlag a, MediaDataFlag b) noexcept {
    return static_cast<MediaDataFlag>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

/**
 * @brief 位异或运算：切换/翻转指定标志位
 * @param a 左操作数
 * @param b 右操作数
 * @return 异或计算结果
 */
constexpr MediaDataFlag operator^(MediaDataFlag a, MediaDataFlag b) noexcept {
    return static_cast<MediaDataFlag>(static_cast<std::uint8_t>(a) ^ static_cast<std::uint8_t>(b));
}

/**
 * @brief 按位取反运算：翻转全量掩码位
 * @param a 目标标志位
 * @return 取反后的标志掩码
 * @note 显式强转截断整型提升（Integer Promotion）产生的多余高位
 */
constexpr MediaDataFlag operator~(MediaDataFlag a) noexcept {
    return static_cast<MediaDataFlag>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(a)));
}

/**
 * @brief 复合位或赋值：追加指定标志
 * @param a 被修改的目标引用
 * @param b 待追加的标志
 * @return 修改后的标志引用
 */
constexpr MediaDataFlag& operator|=(MediaDataFlag& a, MediaDataFlag b) noexcept {
    a = a | b;
    return a;
}

/**
 * @brief 复合位与赋值：过滤保留指定标志
 * @param a 被修改的目标引用
 * @param b 掩码标志
 * @return 修改后的标志引用
 */
constexpr MediaDataFlag& operator&=(MediaDataFlag& a, MediaDataFlag b) noexcept {
    a = a & b;
    return a;
}

/**
 * @brief 复合位异或赋值：翻转指定标志
 * @param a 被修改的目标引用
 * @param b 待翻转的标志
 * @return 修改后的标志引用
 */
constexpr MediaDataFlag& operator^=(MediaDataFlag& a, MediaDataFlag b) noexcept {
    a = a ^ b;
    return a;
}

/**
 * @brief 快速辅助判定：检查指定标志位是否被置位
 * @param flags 目标复合标志集合
 * @param flag 待检验的单个标志位
 * @return true 表示该标志位已置位，false 表示未置位
 */
constexpr bool HasFlag(MediaDataFlag flags, MediaDataFlag flag) noexcept {
    return (flags & flag) != MediaDataFlag::kNone;
}

}  // namespace mediacore
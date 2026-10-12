/**
 * @file frame_size.h
 * @brief 画面像素几何尺寸（宽与高）强类型基础值定义
 * @details
 * 将视频宽高抽象为具名强类型结构体：
 * 1. 防御易混淆参数：规避裸 int 接口中 width 与 height 传参顺序颠倒的隐患（针对 Clang-Tidy bugprone-easily-swappable-parameters 规则）；
 * 2. 强值语义：具备完整的 constexpr / noexcept 保证与默认比较支持；
 * 3. 零开销抽象：纯标量结构，与两个独立 int 占用相同的栈/寄存器开销。
 */

#pragma once

namespace mediacore {

/**
 * @struct FrameSize
 * @brief 画面二维像素物理尺寸结构体
 */
struct FrameSize final {
    int width_{0};   ///< 画面宽度（像素）
    int height_{0};  ///< 画面高度（像素）

    /**
     * @brief 校验宽高是否均为合法正数
     * @return 宽度与高度均严格大于 0 时返回 true，否则返回 false
     */
    constexpr bool IsValid() const noexcept {
        return width_ > 0 && height_ > 0;
    }

    /**
     * @brief 计算图像总像素量（面积）
     * @return 宽高有效时返回 width_ * height_，否则安全返回 0
     */
    constexpr int Area() const noexcept {
        return IsValid() ? (width_ * height_) : 0;
    }

    /**
     * @brief 相等性比较操作符
     */
    constexpr bool operator==(const FrameSize& other) const noexcept {
        return width_ == other.width_ && height_ == other.height_;
    }

    /**
     * @brief 不等性比较操作符
     */
    constexpr bool operator!=(const FrameSize& other) const noexcept {
        return !(*this == other);
    }
};

}  // namespace mediacore
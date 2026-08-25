// mediacore/include/mediacore/av_types/frame_size.h
/**
 * @file frame_size.h
 * @brief 基础值类型：画面尺寸（宽 x 高）
 * @details
 * 把 (width, height) 打包成具名字段，调用点必须按字段名显式赋值，避免两个
 * 相邻 int 参数被调用方误传颠倒（对应 clang-tidy easily-swappable-parameters 检查）
 */

#pragma once

namespace mediacore {

/**
 * @brief 画面尺寸（像素）
 * @details 纯数据聚合，构造时不做合法性校验，宽高是否 > 0 由使用方按需调用 IsValid() 检查
 */
struct FrameSize {
    int width_{0};  ///< 画面宽度（像素）
    int height_{0};  ///< 画面高度（像素）

    /// @brief 判断尺寸是否为正（可用于构造有效画面）
    /// @return true 表示宽高均 > 0
    bool IsValid() const { return width_ > 0 && height_ > 0; }
};

}  // namespace mediacore

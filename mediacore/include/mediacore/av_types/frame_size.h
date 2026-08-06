// mediacore/include/mediacore/av_types/frame_size.h
/**
 * @file frame_size.h
 * @brief 基础值类型：画面尺寸（宽 x 高）
 * @details 用于消除 "相邻同类型 int 参数易被调用方写反" 的隐患（clang-tidy
 *          easily-swappable-parameters）把 (width, height) 打包成一个具名聚合，
 *          调用点以字段名显式赋值，宽高不再可能被静默交换
 *
 *          这是一个可跨 DTO 复用的基础类型：VideoFrame / StreamInfo、编码器分辨率、
 *          纹理尺寸等均可持有它，避免到处裸传 (w, h)零成本抽象——编译后与裸传两个 int 等价，无运行时开销
 */

#pragma once

namespace mediacore {

/**
 * @brief 画面尺寸（像素）
 * @details 纯被动数据聚合（struct + 公开成员），无复杂不变量合法性（>0）由使用方
 *          在需要时校验，本类型不强制约束，保持 "零成本值语义"
 */
struct FrameSize {
    int width_{0};   ///< 画面宽度（像素）
    int height_{0};  ///< 画面高度（像素）

    /// @brief 判断尺寸是否为正（可用于构造有效画面）
    /// @return true 表示宽高均 > 0
    bool IsValid() const { return width_ > 0 && height_ > 0; }
};

}  // namespace mediacore

/**
 * @file video_frame.h
 * @brief 解码后的视频图像帧自包含值类型定义（YUV420P 像素格式）
 * @details
 * 这是一个完全自包含的像素图像载荷对象：
 * 1. 内存解耦与跨线程安全：构造时一次性分配一块连续的对齐堆内存，Y/U/V 三平面的首地址仅为此内存块上的偏移；
 *    数据本身彻底解除对底层 FFmpeg AVFrame 生命周期的依赖，可安全跨线程队列流转或跨模块长期持有；
 * 2. 内存对齐：使用 FFmpeg 的 av_mallocz 进行分配，天然满足 SIMD / 硬件指令集向量化对齐边界；
 * 3. 时序与世代追踪：携带呈现时间戳（PTS）与 Seek 世代号（epoch_），供下游渲染节点校验过期与丢弃。
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>

#include "frame_size.h"
#include "mediacore/logger.h"

extern "C" {
// av_mallocz / av_free：FFmpeg 对齐内存分配与清零/释放函数
// 返回按 SIMD 友好边界对齐的连续内存块，必须成对使用 av_free 释放（严禁与标准库 malloc/free 混用）
#include <libavutil/mem.h>
}

namespace mediacore {

/**
 * @struct VideoFrame
 * @brief 承载单帧 YUV420P 像素数据的图像帧结构体
 */
struct VideoFrame {
    int width_{0};  ///< 画面宽度（像素）
    int height_{0};  ///< 画面高度（像素）

    /// YUV420P 三个平面的像素内存首地址（0: Y, 1: U, 2: V）
    std::array<std::uint8_t*, 3> data_{{nullptr, nullptr, nullptr}};

    /// YUV420P 三个平面各自每行的字节步幅（stride / pitch）
    std::array<int, 3> linesize_{{0, 0, 0}};

    /**
     * @brief 托管像素图像内存的智能指针
     * @details 实际分配的连续内存所有权由此托管；data_ 数组中的指针仅引用此内存的不同偏移位置。
     */
    std::shared_ptr<std::uint8_t> buffer_{nullptr};

    /**
     * @brief 画面呈现时间戳（PTS，单位：微秒）
     * @details 使用 std::optional 表达时间戳存在性，未解析出或缺失时间戳时为空（不强行伪造为 0）。
     */
    std::optional<std::int64_t> pts_us_{};

    /// 媒体流世代号（用于过滤 Seek 之前的过期旧帧）
    std::uint64_t epoch_{0};

    /**
     * @brief 默认构造函数：构造一个空视频帧
     * @details 空帧不持有像素内存，IsValid() 返回 false，常用于临时占位或默认值初始化。
     */
    VideoFrame() = default;

    /**
     * @brief 含参构造函数：按 YUV420P 格式分配连续连续对齐内存并初始化三平面
     * @param size 画面尺寸（宽、高像素）
     * @throws std::bad_alloc 当底层 SIMD 对齐内存分配失败时抛出
     * @details
     * YUV420P 布局计算规则：
     * 1. Y 平面（亮度）：宽度为 width_，高度为 height_，大小为 width_ * height_；
     * 2. U/V 平面（色度）：水平与垂直方向均为 2:1 下采样；
     * 3. 向上取整：若宽高为奇数，色度平面的宽高需通过 (size + 1) / 2 向上对齐取整，防止行尾越界。
     */
    explicit VideoFrame(FrameSize size) : width_(size.width_), height_(size.height_) {
        // 计算色度平面尺寸（2:1 下采样，向上取整）
        const int uv_width = (size.width_ + 1) / 2;
        const int uv_height = (size.height_ + 1) / 2;

        const int y_size = size.width_ * size.height_;
        const int uv_size = uv_width * uv_height;
        const int total_size = y_size + uv_size * 2;  // Y + U + V 总字节数

        // 使用 FFmpeg 提供的 SIMD 边界对齐分配函数分配内存并自动清零
        std::uint8_t* raw_buffer = static_cast<std::uint8_t*>(av_mallocz(static_cast<std::size_t>(total_size)));

        if (raw_buffer == nullptr) {
            LOG_ERROR("VideoFrame allocation failed: {}x{}", size.width_, size.height_);
            throw std::bad_alloc();
        }

        // 由 shared_ptr 托管生命周期，绑定自定义删除器调用 av_free
        buffer_ = std::shared_ptr<std::uint8_t>(raw_buffer, [](std::uint8_t* p) {
            LOG_TRACE("VideoFrame buffer released: {}", static_cast<const void*>(p));
            av_free(p);
        });

        // 在单块连续内存上规划三平面起始指针（弱引用偏移）
        data_[0] = raw_buffer;  ///< Y 亮度平面起始地址
        data_[1] = raw_buffer + y_size;  ///< U 色度平面起始地址
        data_[2] = raw_buffer + y_size + uv_size;  ///< V 色度平面起始地址

        // 紧凑排布格式：Y 行跨距等于画面宽度，UV 行跨距等于色度平面宽度
        linesize_[0] = size.width_;
        linesize_[1] = uv_width;
        linesize_[2] = uv_width;
    }

    /**
     * @brief 校验帧数据结构是否完整并可安全提交至渲染层或 GPU 纹理上传
     * @return true 表示结构与内存有效；空帧或行跨距越界时返回 false
     */
    bool IsValid() const noexcept {
        if (width_ <= 0 || height_ <= 0) {
            return false;
        }

        if (!buffer_) {
            return false;
        }

        if (data_[0] == nullptr || data_[1] == nullptr || data_[2] == nullptr) {
            return false;
        }

        const int uv_width = (width_ + 1) / 2;
        // 校验行跨距（stride）必须大于或等于各平面的实际物理宽度，防止纹理上传读取越界
        if (linesize_[0] < width_ || linesize_[1] < uv_width || linesize_[2] < uv_width) {
            return false;
        }

        return true;
    }
};

}  // namespace mediacore
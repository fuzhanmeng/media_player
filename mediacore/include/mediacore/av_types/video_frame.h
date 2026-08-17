/**
 * @file video_frame.h
 * @brief 视频帧数据对象，保存一帧 YUV420P 图像和随帧传递的时间戳/世代号/控制标志
 * @details
 * VideoFrame 是 Decoder 的输出给音画同步、渲染的原始帧，即 YUV420P
 * 数据流
 * Demuxer 读取 AVPacket -> Decoder 解码 -> VideoFrame -> Renderer / Sync / Filter
 *
 * 生命周期
 * - 构造时按 YUV420P 布局分配一块连续内存
 * - data_ 里的三个裸指针指向 buffer_ 内部的 Y / U / V 平面，不单独拥有内存
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>  // std::bad_alloc

#include "frame_size.h"
#include "mediacore/av_types/media_data_flag.h"
#include "mediacore/logger.h"

extern "C" {
// av_malloc / av_free：FFmpeg 的对齐内存分配 / 释放函数
// av_malloc 返回按 SIMD 友好边界对齐的内存块，必须用 av_free 释放（与 malloc/free 不可混用）
#include <libavutil/mem.h>
}

namespace mediacore {

/**
 * @struct VideoFrame
 * @brief 解码后的单帧 YUV420P 视频帧 DTO
 * @details
 * 当前渲染器 按照 YUV420P 三平面上传 OpenGL 纹理，所以 VideoFrame 固定保存 Y/U/V 三个平面
 */
struct VideoFrame {
    int width_{0};  ///< 画面宽度（像素）
    int height_{0};  ///< 画面高度（像素）

    /// YUV420P 三平面首地址
    std::array<uint8_t*, 3> data_{{nullptr, nullptr, nullptr}};

    /// YUV420P 三平面每行字节数（stride / pitch）
    std::array<int, 3> linesize_{{0, 0, 0}};

    /**
     * @brief buffer_ 是存储图像的实际的内存
     * @details data_ 只是指向这块内存中的三个便宜位置，data_不实际拥有内存，buffer_ 还活着，指针就有效
     * 当最后一个 std::shared_ptr 引用释放时，这块内存也随之释放
     */
    std::shared_ptr<uint8_t> buffer_{nullptr};

    /// 显示时间戳，单位微秒，后续音画同步和进度显示都使用这个值
    int64_t pts_us_{0};

    /// Seek 世代号（带外信令），用于丢弃旧帧、清空历史缓存、重置解码器参考帧
    uint64_t epoch_{0};

    ///< 随帧传递的控制标志，例如 kFlush / kEOF / kKeyFrame
    MediaDataFlag flags_{MediaDataFlag::kNone};

    /**
     * @brief 构造一个空视频帧
     * @details 空帧不持有图像内存，IsValid() 返回 false，它可用于临时变量或者控制信令帧
     */
    VideoFrame() = default;

    /**
     * @brief 含参构造函数，按 YUV420P 布局一次性分配连续内存，并切分为 Y/U/V 三平面
     * @param width 视频宽度（像素）
     * @param height 视频高度（像素）
     * @throws std::bad_alloc 底层内存分配失败时抛出
     * @details
     * YUV420P 有三个平面：
     * - Y 平面保存亮度，大小约为 width * height；
     * - U/V 平面保存色度，水平方向和垂直方向都是 2:1 下采样；
     * - 奇数宽高需要向上取整，否则最后一列或最后一行色度数据会越界。
     */
    explicit VideoFrame(FrameSize size) : width_(size.width_), height_(size.height_) {
        const int uv_width = (size.width_ + 1) / 2;  ///< UV 平面宽度（色度水平 2:1 下采样，向上取整）

        const int uv_height = (size.height_ + 1) / 2;  ///< UV 平面高度（色度垂直 2:1 下采样，向上取整）

        const int y_size = size.width_ * size.height_;  ///< Y 平面字节数
        const int uv_size = uv_width * uv_height;  ///< 单个色度平面总字节数
        const int total_size = y_size + uv_size * 2;  ///< Y + U + V 总字节数

        // av_malloc 分配 SIMD 对齐内存，失败返回 nullptr
        uint8_t* raw_buffer = static_cast<uint8_t*>(av_mallocz(static_cast<size_t>(total_size)));

        if (raw_buffer == nullptr) {
            LOG_ERROR("VideoFrame allocation failed: {}x{}", size.width_, size.height_);
            throw std::bad_alloc();
        }

        // 交给 shared_ptr 托管，自定义删除器保证 av_free 释放
        buffer_ = std::shared_ptr<uint8_t>(raw_buffer, [](uint8_t* p) {
            LOG_TRACE("VideoFrame buffer released: {}", static_cast<const void*>(p));

            av_free(p);
        });

        // 在同一块连续内存上切除三平面首地址（指针语义为 "引用块内存偏移"，不额外拥有内存）
        data_[0] = raw_buffer;  ///< Y 平面
        data_[1] = raw_buffer + y_size;  ///< U 平面
        data_[2] = raw_buffer + y_size + uv_size;  ///< V 平面

        // 紧凑布局： Y行跨距=宽， UV 行跨距 = UV平面宽
        linesize_[0] = size.width_;
        linesize_[1] = uv_width;
        linesize_[2] = uv_width;
    }

    /**
     * @brief 追加一个带内标志
     * @param flag 要追加的标志，例如 MediaDataFlag::kEOF
     */
    void AddFlag(MediaDataFlag flag) { flags_ |= flag; };

    /**
     * @brief 检查当前帧是否携带指定控制标志。
     * @param flag 要检查的标志。
     * @return true 表示包含该标志，false 表示不包含。
     */
    bool HasFlag(MediaDataFlag flag) const { return (flags_ & flag) != MediaDataFlag::kNone; }

    /**
     * @brief 校验本帧是否为可安全渲染的有效数据帧
     * @return true结构完整、可安全上传 GPU， false 为空帧或者不符合需求
     * @details
     * 控制信令帧 Flush/EOF 通常是空帧， IsValid() 返回 false 属于是预期行为，调用方如果关心控制信令，应
     * 先调用 HasFlag()，再决定是否渲染
     */
    bool IsValid() const {
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
        // 行跨距不得小于对应平面宽度，否则纹理上传越界
        if (linesize_[0] < width_ || linesize_[1] < uv_width || linesize_[2] < uv_width) {
            return false;
        }

        return true;
    }
};

}  // namespace mediacore

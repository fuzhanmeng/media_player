/**
 * @file video_frame.h
 * @brief 视频帧的数据传输对象 DTO，DAG 管线中在节点间流动的 智能指针
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <new>  // std::bad_alloc

#include "frame_size.h"  // mediacore::FrameSize：具名宽高，避免相邻 int 参数被写反
#include "mediacore/logger.h"

extern "C" {
// av_malloc / av_free：FFmpeg 的对齐内存分配 / 释放函数
// av_malloc 返回按 SIMD 友好边界对齐的内存块，必须用 av_free 释放（与 malloc/free 不可混用）
#include <libavutil/mem.h>
}

namespace mediacore {

/**
 * @brief 帧的带内控制标志（Flush / EOF 等信令，沿数据通道随帧顺序传播）
 * @details 使用位掩码（enum + 位运算），便于一帧同时携带多个标志
 *          - kNone ：普通数据帧，无特殊语义
 *          - kFlush：Seek 后注入，通知下游清空历史缓存 / 重置解码器参考帧
 *          - kEOF  ：数据流结束，末端节点据此判定全局播放完成
 * @note DAG 架构里，带外信令（Seek 世代号）走 epoch 原子广播；带内信令（Flush/EOF）走本标志随帧传播，二者分工明确
 */
enum class FrameFlag : uint32_t {
    kNone = 0,
    kFlush = 1u << 0,
    kEOF = 1u << 1,
};

/// @brief 位或：组合多个标志
inline FrameFlag operator|(FrameFlag a, FrameFlag b) {
    return static_cast<FrameFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

/// @brief 位与：检测是否包含某标志
inline FrameFlag operator&(FrameFlag a, FrameFlag b) {
    return static_cast<FrameFlag>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/// @brief 位或赋值：向已有标志集追加标志
inline FrameFlag& operator|=(FrameFlag& a, FrameFlag b) {
    a = a | b;
    return a;
}

/**
 * @brief 视频帧 DTO
 */
struct VideoFrame {
    // ================= 图像属性 =================

    int width_{0};  ///< 画面宽度（像素）
    int height_{0};  ///< 画面高度（像素）

    // ================= 数据与内存布局（YUV420P，平面存储）=================

    /// YUV420P 三平面首地址
    std::array<uint8_t*, 3> data_{{nullptr, nullptr, nullptr}};

    /// YUV420P 三平面每行字节数（stride / pitch）
    std::array<int, 3> linesize_{{0, 0, 0}};

    /// 底层连续内存块的唯一持有者，shared_ptr + 自定义删除器（av_free）统一管理生命周期
    std::shared_ptr<uint8_t> buffer_{nullptr};

    // ================= DAG 调度与同步元信息 =================

    /// 绝对显示时间戳 - 微秒, 音画同步与进度显示的依据
    int64_t pts_us_{0};

    /// Seek 世代号（带外信令），用于丢弃旧帧、清空历史缓存、重置解码器参考帧
    int64_t epoch_{0};

    /// 带内控制标志 Flush/EOF
    FrameFlag flags_{FrameFlag::kNone};

    /**
     * @brief 默认构造函数
     */
    VideoFrame() = default;

    /**
     * @brief 含参构造函数，按 YUV420P 布局一次性分配连续内存，并切分为 Y/U/V 三平面
     * @param width 视频宽度（像素）
     * @param height 视频高度（像素）
     * @throws std::bad_alloc 底层内存分配失败时抛出
     * @note 内存流转：单次 av_malloc 分配 (Y + U + V) 连续大块，再将 data[0..2] 指向块内偏移；裸指针交由
     *       shared_ptr 托管，绑定删除器 av_free，最后一个持有者析构时自动释放。一次分配（而非三次）既减少
     *       碎片、又 cache 友好、利于后续 SIMD 处理。UV 平面尺寸按 (width+1)/2 * (height+1)/2 向上取整，
     *       兼容奇数分辨率，防止越界。
     */
    VideoFrame(FrameSize size) : width_(size.width_), height_(size.height_) {
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

    // ================= 标志位操作 =================
    /**
     * @brief 追加一个带内标志
     */
    void AddFlag(FrameFlag flag) { flags_ |= flag; };

    /**
     * @brief 检测是否携带指定标志位
     * @return true -- 表示携带标志位
     */
    bool HasFlag(FrameFlag flag) { return (flags_ & flag) != FrameFlag::kNone; }

    // ================= 有效性校验 =================
    /**
     * @brief 校验本帧是否为可安全渲染的有效数据帧
     * @return true结构完整、可安全上传 GPU， false 为空帧或者不符合需求
     * @note 控制信令帧 Flush/EOF 通常是空帧， IsValid() 返回 false 属于是预期行为，调用方应该先
             检查 HasFlag 再决定是否渲染
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

/**
 * @file video_frame.h
 * @brief 解码后的视频图像帧自包含值类型定义
 * @details
 * VideoFrame 是 MediaCore 中统一的视频帧载体
 * 支持两种帧类型：
 *
 * 1. 软件帧：
 *    - 使用 buffer_ 管理 CPU 内存
 *    - data_ 指向 YUV420P 三个平面
 *    - 渲染时通过 OpenGL 上传像素数据
 *
 * 2. 硬件帧：
 *    - 使用 hardware_ 管理 HardwareFrame
 *    - HardwareFrame 持有 DMA-BUF fd、plane 信息和 AVFrame 引用
 *    - 渲染时通过 EGL 导入 DMA-BUF，创建 EGLImage
 *    - OpenGL 使用 EGLImage 对应的纹理进行渲染
 *
 * VideoFrame 本身不负责解码，也不负责 EGL 导入和 OpenGL 渲染。
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>  // std::numeric_limits：尺寸上限检查
#include <memory>
#include <new>
#include <optional>

#include "frame_size.h"
#include "mediacore/av_types/hardware_frame.h"
#include "mediacore/logger.h"

extern "C" {
// av_mallocz / av_free：FFmpeg 对齐内存分配与清零/释放函数
// 返回按 SIMD 友好边界对齐的连续内存块，必须成对使用 av_free 释放（严禁与标准库 malloc/free 混用）
#include <libavutil/mem.h>
}

namespace mediacore {

/**
 * @enum VideoColorSpace
 * @brief 视频源的色彩空间
 */
enum class VideoColorSpace : std::uint8_t {
    kUnspecified = 0,
    kBt601 = 1,
    kBt709 = 2,
    kBt2020 = 3,
};

/**
 * @enum VideoColorRange
 * @brief 视频源的像素取值范围
 */
enum class VideoColorRange : std::uint8_t {
    kUnspecified = 0,
    kLimited = 1,
    kFull = 2,
};

/**
 * @struct VideoFrame
 * @brief 承载一帧视频图像的数据结构
 */
struct VideoFrame {
    // 1. =============== 基本视频信息 ===============

    int width_{0};  ///< 图像宽度（像素）
    int height_{0};  ///< 图像高度（像素）

    /**
     * @brief YUV420P 三个平面的内存地址
     *
     * data_[0]：Y 亮度平面
     * data_[1]：U 色度平面
     * data_[2]：V 色度平面
     *
     * 仅软件帧使用，硬件帧中全部为 nullptr。
     */
    std::array<std::uint8_t*, 3> data_{{nullptr, nullptr, nullptr}};

    /**
     * @brief YUV420P 三个平面的行跨度
     *
     * 表示一行实际占用多少字节。
     * 仅软件帧使用，硬件帧中全部为 0。
     */
    std::array<int, 3> linesize_{{0, 0, 0}};

    /**
     * @brief 软件帧像素内存
     *
     * 使用 shared_ptr 管理 FFmpeg 分配的内存。
     * data_ 中的指针只是指向这块内存不同位置的地址。
     *
     * 仅软件帧使用，硬件帧中为空。
     */
    std::shared_ptr<std::uint8_t> buffer_{nullptr};

    // 2. =============== 视频时间信息 ===============

    /**
     * @brief 呈现时间戳
     *
     * 单位：微秒。
     * 没有有效 PTS 时为空。
     */
    std::optional<std::int64_t> pts_us_{};

    /**
     * @brief 媒体流世代号
     *
     * Seek 后世代号会发生变化。
     * 下游节点可以利用它过滤 Seek 之前的旧帧。
     */
    std::uint64_t epoch_{0};

    // 3. =============== 视频色彩信息 ===============

    /// 视频源的色彩空间
    VideoColorSpace color_space_{VideoColorSpace::kUnspecified};

    /// 视频源的像素取值范围
    VideoColorRange color_range_{VideoColorRange::kUnspecified};

    // 4. =============== 硬件帧资源 ===============

    /**
     * @brief 硬件帧描述
     * @details
     * 非空表示当前 VideoFrame 是硬件解码帧
     * HardwareFrame 负责管理：
     * DMA-BUF fd
     * DMA-BUF 对象信息
     * 图像 plane 信息
     * DRM FourCC 格式
     * AVFrame 引用
     * HardwareFrame 不会额外复制一份 GPU 图像数据，它主要负责管理硬件图像资源及其生命周期
     *
     */
    std::unique_ptr<HardwareFrame> hardware_{nullptr};

    // 5. =============== 构造与析构 ===============

    /**
     * @brief 默认构造函数：构造一个空视频帧
     */
    VideoFrame() = default;

    /**
     * @brief 软件帧构造函数
     *
     * 根据图像尺寸分配一块连续内存，
     * 并规划 Y、U、V 三个平面。
     *
     * @param size 图像尺寸
     * @throws std::bad_alloc 尺寸非法或内存分配失败
     */
    explicit VideoFrame(FrameSize size) : width_(size.width_), height_(size.height_) {
        // 检查图像尺寸是否合法
        if (width_ <= 0 || height_ <= 0) {
            LOG_ERROR("VideoFrame invalid size: {}x{}", width_, height_);
            throw std::bad_alloc();
        }

        // 色度平面宽高为原图的一半，向上取整。
        // 使用这种写法，避免 width + 1 发生整数溢出。
        const int uv_width = width_ / 2 + width_ % 2;
        const int uv_height = height_ / 2 + height_ % 2;

        // 使用 64 位整数计算内存大小，避免 int 乘法溢出。
        const std::int64_t y_size_64 = static_cast<std::int64_t>(width_) * height_;

        const std::int64_t uv_size_64 = static_cast<std::int64_t>(uv_width) * uv_height;

        const std::int64_t total_size_64 = y_size_64 + uv_size_64 * 2;

        // 检查总内存大小是否合法。
        if (total_size_64 <= 0 || static_cast<std::uint64_t>(total_size_64) > std::numeric_limits<std::size_t>::max()) {
            LOG_ERROR("VideoFrame allocation size overflow: {}x{}", width_, height_);

            throw std::bad_alloc();
        }

        // data_ 使用指针偏移，因此平面大小必须能够用 int 表示。
        if (y_size_64 > std::numeric_limits<int>::max() || uv_size_64 > std::numeric_limits<int>::max()) {
            LOG_ERROR("VideoFrame plane size out of range: {}x{}", width_, height_);

            throw std::bad_alloc();
        }

        const int y_size = static_cast<int>(y_size_64);

        const int uv_size = static_cast<int>(uv_size_64);

        const std::size_t total_size = static_cast<std::size_t>(total_size_64);

        // 使用 FFmpeg 的内存分配函数分配对齐内存。
        // av_mallocz 会自动将内存清零。
        auto* raw_buffer = static_cast<std::uint8_t*>(av_mallocz(total_size));

        if (raw_buffer == nullptr) {
            LOG_ERROR("VideoFrame allocation failed: {}x{}", width_, height_);

            throw std::bad_alloc();
        }

        // 使用 shared_ptr 管理内存。
        // 释放时调用 av_free，而不是标准库 free。
        buffer_ = std::shared_ptr<std::uint8_t>(raw_buffer, [](std::uint8_t* p) {
            LOG_TRACE("VideoFrame buffer released: {}", static_cast<const void*>(p));

            av_free(p);
        });

        // 在同一块连续内存中规划三个平面。
        data_[0] = raw_buffer;
        data_[1] = raw_buffer + y_size;
        data_[2] = raw_buffer + y_size + uv_size;

        // 紧凑排布：每行没有额外填充。
        linesize_[0] = width_;
        linesize_[1] = uv_width;
        linesize_[2] = uv_width;
    }

    // 6. =============== 拷贝与移动 ===============

    /**
     * @brief 禁止拷贝
     * @details
     * VideoFrame 可能持有独立的硬件资源，不允许直接复制
     */
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;

    /**
     * @brief 移动构造函数
     * @param other 资源的拥有者
     * @details
     * 使用 other 去创建一个不存的对象
     */
    VideoFrame(VideoFrame&& other) noexcept { MoveFrom(std::move(other)); }

    /**
     * @brief 移动赋值函数
     * @param other 资源的拥有者
     * @details
     * 将 other 对象资源转移到另一个已经存在的对象
     */
    VideoFrame& operator=(VideoFrame&& other) noexcept {
        if (this != &other) {
            MoveFrom(std::move(other));
        }

        return *this;
    }

    /**
     * @brief 析构函数
     */
    ~VideoFrame() = default;

    // 7. 有效性检查
    /**
     * @brief 检查视频帧是否有效
     *
     * 软件帧：
     *   检查像素内存、平面地址和行跨度。
     *
     * 硬件帧：
     *   检查 HardwareFrame、DMA-BUF 对象和 plane 信息。
     *
     * @return true 表示基本结构有效
     */
    bool IsValid() const noexcept {
        // 1. 检查基本尺寸
        if (width_ <= 0 || height_ <= 0) {
            return false;
        }
        // 2. 硬件分支
        if (hardware_ != nullptr) {
            if (buffer_ != nullptr) {
                return false;
            }

            // 硬件帧不应该存在 CPU 像素指针和行跨度
            for (std::size_t i = 0; i < data_.size(); ++i) {
                if (data_[i] != nullptr || linesize_[i] != 0) {
                    return false;
                }
            }

            // 可见尺寸允许小于驱动对齐后的存储尺寸，但不能超出存储边界
            // 例如 1080p H.264：VideoFrame 的可见高度是 1080，而 VA-API 导出的存储高度是 1088（按 16 行对齐）
            // 这里若写成"必须相等"，所有高度不是 16 倍数的视频其硬件帧都会判为无效，
            // paintGL 会静默丢弃每一帧，画面只剩清屏色；720p 恰好对齐，所以不容易被发现
            // 可见区域固定从图像左上角开始，渲染端用纹理坐标缩放去掉右侧/底部的对齐填充
            if (width_ > hardware_->width_ || height_ > hardware_->height_) {
                return false;
            }

            // 必须存在 DAM-BUF 对象和 plane 描述
            if (hardware_->dma_buf_objects_.empty() || hardware_->planes_.empty()) {
                return false;
            }

            // 检查 DMA-BUF 对象
            for (const auto& object : hardware_->dma_buf_objects_) {
                if (object.fd_ < 0 || object.size_bytes_ == 0) {
                    return false;
                }
            }

            // 检查 plane 信息
            for (const auto& plane : hardware_->planes_) {
                if (plane.dma_buf_index_ >= hardware_->dma_buf_objects_.size()) {
                    return false;
                }

                if (plane.pitch_bytes_ == 0) {
                    return false;
                }

                const auto& object = hardware_->dma_buf_objects_[plane.dma_buf_index_];

                // plane 的起始偏移必须位于对象有效范围内
                if (plane.offset_bytes_ >= object.size_bytes_) {
                    return false;
                }
            }
            return true;
        }

        // 2. 软件帧分支
        // 软件帧必须持有像素内存。
        if (buffer_ == nullptr) {
            return false;
        }

        // 三个平面地址都必须有效。
        if (data_[0] == nullptr || data_[1] == nullptr || data_[2] == nullptr) {
            return false;
        }

        const int uv_width = width_ / 2 + width_ % 2;

        // 检查每个平面的行跨度。
        if (linesize_[0] < width_ || linesize_[1] < uv_width || linesize_[2] < uv_width) {
            return false;
        }

        return true;
    }

private:
    // 8. 移动辅助函数

    /**
     * @brief 将另一个 VideoFrame 的资源移动到当前对象
     *
     * 移动完成后：
     * - 当前对象接管原对象的资源。
     * - 原对象恢复为空帧状态。
     *
     * 特别注意：
     * data_ 是普通裸指针数组，不会自动清空，
     * 所以移动完成后必须显式清理源对象的指针。
     */
    void MoveFrom(VideoFrame&& other) noexcept {
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);

        data_ = other.data_;
        linesize_ = other.linesize_;

        buffer_ = std::move(other.buffer_);

        pts_us_ = other.pts_us_;
        other.pts_us_.reset();

        epoch_ = std::exchange(other.epoch_, 0);

        color_space_ = other.color_space_;
        color_range_ = other.color_range_;

        hardware_ = std::move(other.hardware_);

        // 清理原对象的裸指针。
        other.data_.fill(nullptr);
        other.linesize_.fill(0);

        other.color_space_ = VideoColorSpace::kUnspecified;

        other.color_range_ = VideoColorRange::kUnspecified;
    }
};

}  // namespace mediacore

/**
 * @file hardware_frame.cpp
 * @brief HardwareSurface 的资源释放与移动语义实现
 */

#include "mediacore/av_types/hardware_frame.h"

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace mediacore {

/**
 * @brief 析构函数，释放当前对象持有的全部资源
 * @details
 * 调用 ReleaseResources() 统一释放资源
 *   1. 关闭所有有效的 DMA-BUF 文件描述符
 *   2. 释放 FFmpeg AVFrame 引用
 */
HardwareFrame::~HardwareFrame() {
    ReleaseResources();
}

/**
 * @brief 移动构造函数，将另一个对象持有的资源转移到当前对象
 * @param other 被移动的源对象
 * @details
 * 使用 other 的资源创建一个不存在的对象
 */
HardwareFrame::HardwareFrame(HardwareFrame&& other) noexcept
    : width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)),
      layer_format_(std::exchange(other.layer_format_, 0)),
      dma_buf_objects_(std::move(other.dma_buf_objects_)),
      planes_(std::move(other.planes_)),
      av_frame_ref_(std::exchange(other.av_frame_ref_, nullptr)) {
    other.dma_buf_objects_.clear();
    other.planes_.clear();
}

/**
 * @brief 移动赋值运算符
 * @param other 被移动的源对象
 * @details
 * 使用 other 的资源转移到一个已经存在的对象
 */
HardwareFrame& HardwareFrame::operator=(HardwareFrame&& other) noexcept {
    // 1. 避免自复制
    if (this == &other) {
        return *this;
    }

    // 2. 释放当前对象的资源
    ReleaseResources();

    // 3. 接管源对象的基本图像信息
    width_ = std::exchange(other.width_, 0);
    height_ = std::exchange(other.height_, 0);
    layer_format_ = std::exchange(other.layer_format_, 0);

    // 4. 接管源对象的 DMA-BUF 对象和平面信息
    dma_buf_objects_ = std::move(other.dma_buf_objects_);
    planes_ = std::move(other.planes_);
    other.dma_buf_objects_.clear();
    other.planes_.clear();

    // 5. 接管 AVFrame 引用，并清空源对象的指针
    av_frame_ref_ = std::exchange(other.av_frame_ref_, nullptr);

    return *this;
}

/**
 * @brief 释放当前对象持有的全部资源
 * @details
 *   1. 遍历 DMA-BUF 对象列表，关闭所有有效 fd
 *   2. 清空 DMA-BUF 对象和平面列表
 *   3. 释放 AVFrame 引用，使 VA Surface 在 fd 关闭后才可能回到池中
 *   4. 重置图像尺寸和格式信息
 */

void HardwareFrame::ReleaseResources() noexcept {
    // 逐个关闭本对象持有的 DMA-BUF 文件描述符。
    for (std::size_t object_index = 0; object_index < dma_buf_objects_.size(); ++object_index) {
        auto& dma_buf_object = dma_buf_objects_[object_index];
        if (dma_buf_object.fd_ >= 0) {
            // 即使调用方传入了验证会拒绝的重复 fd，也只关闭一次。
            for (std::size_t later_index = object_index + 1; later_index < dma_buf_objects_.size(); ++later_index) {
                if (dma_buf_objects_[later_index].fd_ == dma_buf_object.fd_) {
                    dma_buf_objects_[later_index].fd_ = -1;
                }
            }
            ::close(dma_buf_object.fd_);
            dma_buf_object.fd_ = -1;
        }
    }

    // 清空资源描述信息。
    dma_buf_objects_.clear();
    planes_.clear();

    // 最后释放 FFmpeg 帧引用；此时 Surface 对应的独立 fd 已关闭。
    if (av_frame_ref_ != nullptr) {
        av_frame_free(&av_frame_ref_);
    }

    // 将图像信息恢复为空状态。
    width_ = 0;
    height_ = 0;
    layer_format_ = 0;
}

/**
 * @brief 验证 DMA-BUF 布局的完整性和安全性
 * @return true 表示布局有效且可以安全导入，false 表示存在错误或不支持的配置
 *
 */
bool HardwareFrame::ValidateDmaBufLayout() const noexcept {
    constexpr std::size_t kMaximumDmaBufObjects = kMaxHardwareDmaBufObjects;
    constexpr std::size_t kPlaneCount = 2;
    constexpr std::uint64_t kMaximumByteOffset = std::numeric_limits<std::uint64_t>::max();

    // 第 1 步：基础校验 - 检查图像尺寸和容器大小

    // 图像尺寸必须为正数
    if (width_ <= 0 || height_ <= 0) {
        return false;
    }

    // 必须至少有一个 DMA-BUF 对象，且不超过最大限制
    if (dma_buf_objects_.empty() || dma_buf_objects_.size() > kMaximumDmaBufObjects) {
        return false;
    }

    // NV12/P010 必须恰好 2 个平面（Y + UV）
    if (planes_.size() != kPlaneCount) {
        return false;
    }

    // 第 2 步：格式识别 - 确定像素格式并计算每分量字节数

    // bytes_per_component: 每个颜色分量占用的字节数
    // - NV12: 8-bit，每分量 1 字节
    // - P010: 10-bit 存储在 16-bit 容器中，每分量 2 字节
    std::uint64_t bytes_per_component = 0;
    if (layer_format_ == kDrmFormatNv12) {
        bytes_per_component = 1;  // NV12: 8-bit YUV
    } else if (layer_format_ == kDrmFormatP010) {
        bytes_per_component = 2;  // P010: 10-bit YUV (存储在 16-bit 中)
    } else {
        // 不支持的格式（例如 YUV444P、ARGB 等）
        return false;
    }

    // 第 3 步：计算理论平面尺寸 - 根据图像格式计算每个平面的预期大小

    // 使用 64 位整数计算，防止整数溢出
    const std::uint64_t image_width = static_cast<std::uint64_t>(width_);
    const std::uint64_t image_height = static_cast<std::uint64_t>(height_);

    // 色度平面尺寸：YUV 4:2:0 采样，色度宽高为亮度的一半（向上取整）
    const std::uint64_t chroma_width = image_width / 2 + image_width % 2;
    const std::uint64_t chroma_height = image_height / 2 + image_height % 2;

    // 计算每个平面一行的最小字节数（不含对齐）
    // - Y 平面 (plane 0): width × bytes_per_component
    //   例如 NV12 1920×1080: 1920 × 1 = 1920 字节/行
    //   例如 P010 1920×1080: 1920 × 2 = 3840 字节/行
    const std::uint64_t luma_row_bytes = image_width * bytes_per_component;

    // - UV 平面 (plane 1): chroma_width × bytes_per_component × 2
    //   NV12/P010 的 UV 是交织存储（U 和 V 相邻），所以 × 2
    //   例如 NV12: 960 × 1 × 2 = 1920 字节/行
    //   例如 P010: 960 × 2 × 2 = 3840 字节/行
    const std::uint64_t chroma_row_bytes = chroma_width * bytes_per_component * 2;

    // 存储两个平面的理论尺寸，用于后续验证
    const std::uint64_t plane_row_bytes[kPlaneCount] = {luma_row_bytes, chroma_row_bytes};
    const std::uint64_t plane_rows[kPlaneCount] = {image_height, chroma_height};

    // 第 4 步：验证 DMA-BUF 对象的有效性

    // 标记哪些 DMA-BUF 对象被平面引用（用于检测未使用的对象）
    bool object_is_used[kMaximumDmaBufObjects] = {};

    // 遍历所有 DMA-BUF 对象，逐个验证
    for (std::size_t object_index = 0; object_index < dma_buf_objects_.size(); ++object_index) {
        const auto& dma_buf_object = dma_buf_objects_[object_index];

        // 4.1 检查 fd 有效性和缓冲区大小
        if (dma_buf_object.fd_ < 0 || dma_buf_object.size_bytes_ == 0) {
            return false;
        }

        // 4.2 使用 fcntl 验证 fd 是否真实可访问
        // 使用 F_GETFD 获取文件描述符标志，这是最轻量的验证方式
        // 重试循环处理 EINTR（系统调用被信号中断）
        int descriptor_flags = -1;
        do {
            descriptor_flags = ::fcntl(dma_buf_object.fd_, F_GETFD);
        } while (descriptor_flags < 0 && errno == EINTR);

        if (descriptor_flags < 0) {
            // fd 无效或已关闭
            return false;
        }

        // 4.3 检查 fd 唯一性：同一个 fd 不能在列表中重复出现
        // 如果重复，可能导致：
        // - 析构时重复 close() 同一个 fd
        // - EGL 导入时创建多个 EGLImage 引用同一资源
        for (std::size_t previous_index = 0; previous_index < object_index; ++previous_index) {
            if (dma_buf_objects_[previous_index].fd_ == dma_buf_object.fd_) {
                return false;  // 发现重复 fd
            }
        }
    }

    // 第 5 步：验证 DRM Format Modifier

    // Modifier 描述图像数据在内存中的实际布局方式
    // - LINEAR (0x0): 线性排列，按行顺序存储
    // - Intel 4K tiled (0x0100000000000009): 128 字节 × 32 行分块存储

    // 所有对象必须使用相同的 modifier（从第一个对象获取）
    const std::uint64_t drm_format_modifier = dma_buf_objects_[0].format_modifier_;

    // 本项目只支持两种 modifier
    if (drm_format_modifier != kDrmFormatModifierLinear && drm_format_modifier != kDrmFormatModifierIntel4Tiled) {
        // 不支持的 modifier 包括：
        // - Intel X/Y/Yf tiled
        // - CCS (Color Control Surface) 压缩
        // - AMD/NVIDIA 专有布局
        // - INVALID modifier
        return false;
    }

    // Intel 4K tiled 按完整 tile 足迹验证。其他 tile、压缩或辅助平面
    // 布局不能由当前元数据安全计算，因此在 modifier 检查时拒绝。

    // 记录 tile 尺寸（LINEAR 时为 0）
    std::uint64_t tile_width_bytes = 0;
    std::uint64_t tile_height_rows = 0;
    if (drm_format_modifier == kDrmFormatModifierIntel4Tiled) {
        // Intel 4K tiled: 每个 tile 是 128 字节宽 × 32 行高 = 4096 字节
        tile_width_bytes = 128;
        tile_height_rows = 32;
    }

    // 第 6 步：验证每个平面的布局

    // 记录每个平面在 DMA-BUF 中的起始和结束位置（用于重叠检测）
    std::uint64_t plane_begin_offset_bytes[kPlaneCount] = {};
    std::uint64_t plane_end_offset_bytes[kPlaneCount] = {};

    // 遍历两个平面（Y 和 UV）
    for (std::size_t plane_index = 0; plane_index < kPlaneCount; ++plane_index) {
        const auto& dma_buf_plane = planes_[plane_index];

        // 6.1 验证平面格式匹配
        // 根据整体格式和平面索引，获取该平面应有的 DRM FourCC
        // - NV12: plane 0 应该是 R8，plane 1 应该是 GR88
        // - P010: plane 0 应该是 R16，plane 1 应该是 GR1616
        const std::uint32_t expected_fourcc = GetDrmPlaneFourcc(layer_format_, plane_index);

        if (expected_fourcc == 0 || dma_buf_plane.drm_format_ != expected_fourcc) {
            return false;  // 平面格式不匹配
        }

        // 6.2 验证 DMA-BUF 对象索引有效
        if (dma_buf_plane.dma_buf_index_ >= dma_buf_objects_.size()) {
            return false;  // 索引越界
        }

        // 获取该平面引用的 DMA-BUF 对象
        const auto& dma_buf_object = dma_buf_objects_[dma_buf_plane.dma_buf_index_];

        // 标记该对象被使用（用于后续检测未使用的对象）
        object_is_used[dma_buf_plane.dma_buf_index_] = true;

        // 6.3 验证 modifier 一致性
        // 所有平面必须使用相同的 modifier
        if (dma_buf_object.format_modifier_ != drm_format_modifier) {
            return false;
        }

        // 6.4 验证 pitch 和 offset
        const std::uint64_t pitch_bytes = dma_buf_plane.pitch_bytes_;
        const std::uint64_t offset_bytes = dma_buf_plane.offset_bytes_;

        // bytes_per_texel: 每个纹理元素（texel）占用的字节数
        // - Y 平面: 每个像素 1 个分量（NV12: 1 字节，P010: 2 字节）
        // - UV 平面: 每个像素 2 个分量（U 和 V 交织）
        const std::uint64_t bytes_per_texel = bytes_per_component * (plane_index == 0 ? 1 : 2);

        // a) pitch 必须至少能容纳一行的数据
        if (pitch_bytes < plane_row_bytes[plane_index]) {
            return false;
        }

        // b) pitch 必须是纹理元素大小的倍数（对齐要求）
        // 例如 P010 的 UV 平面，每个 texel 是 4 字节（2×2 字节），pitch 必须是 4 的倍数
        if (pitch_bytes % bytes_per_texel != 0) {
            return false;
        }

        // c) offset 也必须对齐到纹理元素边界
        if (offset_bytes % bytes_per_texel != 0) {
            return false;
        }

        // d) offset 不能超过 DMA-BUF 的总大小
        if (offset_bytes >= dma_buf_object.size_bytes_) {
            return false;
        }

        // 记录平面起始位置
        // 第 8 步的「同一 DMA-BUF 内平面是否重叠」要用 begin/end 两端比较。
        // 漏掉这一行时 begin 恒为 0，同一 object 里的 Y/UV 两个平面会被误判为重叠，
        // ValidateDmaBufLayout() 对任何真实帧都返回 false，零拷贝永远走不通。
        plane_begin_offset_bytes[plane_index] = offset_bytes;

        // 6.5 计算平面结束位置并检查边界
        // 根据不同的 modifier 采用不同的计算方式
        if (drm_format_modifier == kDrmFormatModifierLinear) {
            // ===== LINEAR 布局：按行线性排列 =====

            // 计算前 N-1 行占用的空间
            // 例如 1080p Y 平面：前 1079 行，每行 pitch 字节
            const std::uint64_t preceding_rows = plane_rows[plane_index] - 1;

            // 防止整数溢出：检查 preceding_rows × pitch_bytes 是否会溢出
            // 如果 pitch_bytes 太大，使得 offset + preceding_rows × pitch 超过 uint64_t 上限，则拒绝
            if (preceding_rows != 0 && pitch_bytes > (kMaximumByteOffset - offset_bytes) / preceding_rows) {
                return false;  // 乘法会溢出
            }

            // 计算最后一行的起始位置
            const std::uint64_t last_row_offset = offset_bytes + preceding_rows * pitch_bytes;

            // 最后一行的数据从 last_row_offset 开始，长度为 plane_row_bytes
            // 检查最后一行是否会溢出
            if (plane_row_bytes[plane_index] > kMaximumByteOffset - last_row_offset) {
                return false;  // 加法会溢出
            }

            // 平面结束位置 = 最后一行起始 + 最后一行长度
            plane_end_offset_bytes[plane_index] = last_row_offset + plane_row_bytes[plane_index];

        } else {
            // ===== Intel 4K Tiled 布局：按 128×32 tile 分块存储 =====

            // a) pitch 必须是 tile 宽度的倍数（128 字节）
            if (pitch_bytes % tile_width_bytes != 0) {
                return false;
            }

            // b) offset 必须对齐到 4KB 边界（一个完整 tile 的大小）
            if (offset_bytes % 4096 != 0) {
                return false;
            }

            // c) 高度向上取整到完整 tile 行
            // 例如 1080 行，tile_height_rows = 32
            // aligned_rows = ceil(1080/32) × 32 = 34 × 32 = 1088 行
            //
            // 存储宽度由 pitch 描述；高度向完整 tile 行向上取整。
            // image_height 来自正 int，向上取整的中间值不会接近 uint64_t 上限。
            const std::uint64_t aligned_rows =
                ((plane_rows[plane_index] + tile_height_rows - 1) / tile_height_rows) * tile_height_rows;

            // d) 防止整数溢出：检查 aligned_rows × pitch_bytes
            if (pitch_bytes > (kMaximumByteOffset - offset_bytes) / aligned_rows) {
                return false;
            }

            // 平面结束位置 = offset + pitch × 对齐后的行数
            plane_end_offset_bytes[plane_index] = offset_bytes + pitch_bytes * aligned_rows;
        }

        // 6.6 验证平面不超出 DMA-BUF 边界
        if (plane_end_offset_bytes[plane_index] > dma_buf_object.size_bytes_) {
            return false;  // 平面数据超出 DMA-BUF 大小
        }
    }  // 结束平面循环

    // 第 7 步：验证所有 DMA-BUF 对象都被使用

    // 检查是否有未被任何平面引用的 DMA-BUF 对象
    // 如果存在未使用的对象，说明：
    // - 数据结构不一致
    // - 可能存在资源泄漏
    for (std::size_t object_index = 0; object_index < dma_buf_objects_.size(); ++object_index) {
        if (!object_is_used[object_index]) {
            return false;  // 发现未被使用的 DMA-BUF 对象
        }
    }

    // 第 8 步：检查平面重叠（仅当两个平面在同一个 DMA-BUF 中时）

    // 如果 Y 平面和 UV 平面共享同一个 DMA-BUF 对象
    if (planes_[0].dma_buf_index_ == planes_[1].dma_buf_index_) {
        // 检查两个平面的内存区间是否重叠
        // 重叠条件：plane0_begin < plane1_end AND plane1_begin < plane0_end
        //
        // 例如：
        //   Y 平面: [0, 2073600)
        //   UV 平面: [2073600, 3110400)
        //   不重叠：0 < 3110400 (true) AND 2073600 < 2073600 (false) → 验证通过
        //
        //   Y 平面: [0, 2073600)
        //   UV 平面: [2000000, 3000000)
        //   重叠：0 < 3000000 (true) AND 2000000 < 2073600 (true) → 验证失败
        if (plane_begin_offset_bytes[0] < plane_end_offset_bytes[1] &&
            plane_begin_offset_bytes[1] < plane_end_offset_bytes[0]) {
            return false;  // 发现平面重叠
        }
    }

    // 如果两个平面在不同的 DMA-BUF 对象中，则不需要检查重叠
    // 因为它们是独立的内存区域

    // 所有检查都通过，这是一个有效且安全的 DMA-BUF 布局
    return true;
}

/**
 * @brief 将 DRM FourCC 格式标识转换为可读名称
 *
 * @param drm_format 32 位 DRM FourCC 格式标识
 *
 * @return
 *   已知格式：返回对应的格式名称，例如 "NV12"、"P010"。
 *   未知格式：如果四个字节都是可打印字符，返回四字符名称；
 *             否则返回十六进制格式字符串。
 *
 * @details
 * 主要用于日志输出和调试。
 *
 * 对于未识别的格式：
 *   1. 从低位到高位依次取出四个字节。
 *   2. 判断四个字节是否都是可打印字符。
 *   3. 如果是，按 FourCC 字符顺序输出。
 *   4. 如果不是，输出 32 位十六进制数。
 *
 * 未知格式的字符串保存在当前线程的局部缓冲区中。
 * 返回的指针在同一线程下一次调用本函数时可能被覆盖。
 */
const char* GetDrmFourccName(std::uint32_t drm_format) noexcept {
    switch (drm_format) {
        case kDrmFormatNv12:
            return "NV12";

        case kDrmFormatP010:
            return "P010";

        case CreateDrmFourcc('Y', 'V', '1', '2'):
            return "YV12";

        case kDrmFormatR8:
            return "R8";

        case kDrmFormatGr88:
            return "GR88";

        case kDrmFormatR16:
            return "R16";

        case kDrmFormatGr1616:
            return "GR1616";

        default:
            break;
    }

    // 保存未知 FourCC 的格式化结果。
    static thread_local char buffer[32];

    // 按照 FourCC 的字节顺序提取四个字符。
    const char c0 = static_cast<char>(drm_format & 0xFFU);
    const char c1 = static_cast<char>((drm_format >> 8U) & 0xFFU);
    const char c2 = static_cast<char>((drm_format >> 16U) & 0xFFU);
    const char c3 = static_cast<char>((drm_format >> 24U) & 0xFFU);

    // isprint() 的参数必须转换为 unsigned char，
    // 避免 char 为负数时产生未定义行为。
    const bool is_printable = std::isprint(static_cast<unsigned char>(c0)) &&
        std::isprint(static_cast<unsigned char>(c1)) && std::isprint(static_cast<unsigned char>(c2)) &&
        std::isprint(static_cast<unsigned char>(c3));

    if (is_printable) {
        // 四个字节都是可打印字符，直接输出 FourCC 名称。
        snprintf(buffer, sizeof(buffer), "%c%c%c%c", c0, c1, c2, c3);
    } else {
        // 存在不可打印字符，使用十六进制形式输出。
        snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(drm_format));
    }

    return buffer;
}

}  // namespace mediacore
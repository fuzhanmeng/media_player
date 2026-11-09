/**
 * @file lhardware_frame.h
 * @brief 硬件解码帧的数据结构：描述 DMA-BUF 内存、图像平面和 FFmpeg 帧引用
 * @note
 *
 * 硬解帧到底存在哪里:
 * 硬件解码-GPU解码：输出的是 GPU 显存里的一块 surface（表面），CPU不能使用指针访问
 * 想知道这块显存的 surface 什么样，唯一的办法是让驱动导出一份描述

 * DMA-BUF 是什么：
 * DMA-BUF 是 Linux 内核提供的[跨设备的共享缓冲区] 机制，一个 DMA-BUF 用 fd 来表示
 * fd 是一个整数，背后指向了一块内核管理的物理内存
 * 这块内存可以同时被 GPU 解码器写、被 OpenGL 读，中间不需要任何的 CPU 拷贝 ,这就是「零拷贝」的物理基础
 *
 * VA-API 导出的三层结构
 * VA-API 可以通过 VADRMPRIMESurfaceDescriptor 描述导出的图像：
 *     object
 *       |
 *       | 一个 DMA-BUF 对象，通常对应一个 fd
 *       |
 *       +-----------------------+
 *                               |
 *     layer                     |
 *       |                       |
 *       | 描述一组特定格式的图像 |
 *       |                       |
 *       +-----------------------+
 *         |                   |
 *         v                   v
 *       plane 0             plane 1
 *         Y                   UV
 * 例如 NV12：
 *
 *     object[0]: fd = 5
 *
 *     layer[0]: format = NV12
 *         plane[0]: Y
 *             object_index = 0
 *             offset       = 0
 *             pitch        = 1280
 *
 *         plane[1]: UV
 *             object_index = 0
 *             offset       = 942080
 *             pitch        = 1280
 *
 * 两个 plane 都引用 object[0]，所以它们共用同一个 DMA-BUF 对象
 *
 *
 * 注意：
 *
 *     object：描述 DMA-BUF 内存对象。
 *     layer ：描述一组具有特定格式的图像平面。
 *     plane ：描述一个具体的采样平面。
 *
 * 不能认为一个 plane 就对应一个 fd。
 *
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVFrame;

namespace mediacore {

/**
 * @brief 单帧支持的最大图像平面数量
 * @details
 * HardwareFrame::planes_ 设置的容量限制
 * 常见格式：
 *
 *     NV12     -> 2 个平面：Y、UV
 *     P010     -> 2 个平面：Y、UV
 *     YUV420P  -> 3 个平面：Y、U、V
 */
const std::size_t kMaxHardwarePlanes = 4;

/** @brief 本项目允许描述的最大 DMA-BUF 对象数，与 DRM PRIME 导出数组容量一致 */
constexpr std::size_t kMaxHardwareDmaBufObjects = 4;

/**
 * @brief 根据四个字符生成 DRM FourCC 格式标识
 *
 * @param first_character 第一个字符
 * @param second_character 第二个字符
 * @param third_character 第三个字符
 * @param fourth_character 第四个字符
 *
 * @return 由四个字符组合而成的 32 位格式标识
 *
 * @details
 * FourCC 是 Four Character Code 的缩写，它使用四个字符表示一种数据格式
 *
 */
constexpr std::uint32_t CreateDrmFourcc(char first_character,
                                        char second_character,
                                        char third_character,
                                        char fourth_character) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(first_character)) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(second_character)) << 8) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(third_character)) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(fourth_character)) << 24);
}

// -------------------------- 整体图像格式 --------------------------

/// NV12：8 bit YUV 4:2:0，包含 Y 和 UV 两个平面。
constexpr std::uint32_t kDrmFormatNv12 = CreateDrmFourcc('N', 'V', '1', '2');

/// P010：10 bit 有效数据的 YUV 4:2:0，包含 Y 和 UV 两个平面。
constexpr std::uint32_t kDrmFormatP010 = CreateDrmFourcc('P', '0', '1', '0');

// -------------------------- 单个图像平面的格式 --------------------------

/// R8：单通道 8 bit，通常用于 NV12 的 Y 平面。
constexpr std::uint32_t kDrmFormatR8 = CreateDrmFourcc('R', '8', ' ', ' ');

/// GR88：两个 8 bit 通道，通常用于 NV12 的 UV 平面。
constexpr std::uint32_t kDrmFormatGr88 = CreateDrmFourcc('G', 'R', '8', '8');

/// R16：单通道 16 bit，通常用于 P010 的 Y 平面。
constexpr std::uint32_t kDrmFormatR16 = CreateDrmFourcc('R', '1', '6', ' ');

/// GR32：两个 16 bit 通道，通常用于 P010 的 UV 平面。
constexpr std::uint32_t kDrmFormatGr1616 = CreateDrmFourcc('G', 'R', '3', '2');

// 本项目允许进行结构验证的 modifier 集合，不代表运行时 EGL/GPU 导入支持。
constexpr std::uint64_t kDrmFormatModifierLinear = 0x0000000000000000ULL;
// Intel 4K tiled：每个 4 KiB tile 为 128 字节 x 32 行。其他 Intel X/Y/Yf/CCS
// 布局以及 INVALID/未知 modifier 均不在当前支持集合内。
constexpr std::uint64_t kDrmFormatModifierIntel4Tiled = 0x0100000000000009ULL;

/**
 * @brief 根据整体图像格式和当前平面编号，获取该平面的 DRM 格式
 * @param layer_format 整个图像层的格式，例如 NV12 / P010
 * @param plane_index  当前平面的下标，从 0 开始
 * @return 对应平面的 DRM FourCC，不支持的格式或平面下标返回 0
 */
constexpr std::uint32_t GetDrmPlaneFourcc(std::uint32_t layer_format, std::size_t plane_index) noexcept {
    if (plane_index > 1) {
        return 0;
    }

    if (layer_format == kDrmFormatNv12) {
        return plane_index == 0 ? kDrmFormatR8 : kDrmFormatGr88;
    }

    if (layer_format == kDrmFormatP010) {
        return plane_index == 0 ? kDrmFormatR16 : kDrmFormatGr1616;
    }

    return 0;
}

/**
 * @struct DmaBufObject - DMA-BUF 对象
 * @brief 描述一个 DAM-BUF 对象
 * @details
 * DMA-BUF 是 Linux 内核提供的跨设备的共享缓冲区机制，可以通过文件描述符 fd 访问
 * 一个 DMA-BUF 对象可以被多个图像平面共同使用，DMA-BUF 有 1~N 个
 * DmaBufObject 本身只是一个普通结构体，不会自动关闭 fd，HardwareFrame 负责关闭
 * VA-API 导出 DMA-BUF 后，调用方需要管理导出的 fd，可以通过 dup 创建一个独立的 fd 副本
 */
struct DmaBufObject {
    // DMA-BUF 文件描述符
    int fd_{-1};

    // DMA-BUf 对象的大小，单位字节
    std::uint32_t size_bytes_{0};

    /**
     * @brief DRM Format Modifier
     * @details
     * Modifier 用于描述图像在内存中的实际存储布局
     * FourCC 主要说明图像是什么格式，例如 NV12 / P010
     * DRM_FORMAT_MOD_LINEAR： 线性布局，数据通常按行排列
     * 非 LINEAR modifier：可能表示 tiled、压缩或厂商定义的其他特殊布局
     */
    std::uint64_t format_modifier_{0};
};

/**
 * @struct DmaBufPlane
 * @brief 描述图像中的一个采样面
 * @details
 * 一张 NV12 图像可能由多个平面组成
 * 例如 NV12：
 * plane[0]: Y 平面 plane[1]: UV 平面
 * 每个平面需要知道：
 *
 *     1. 自己使用哪个 DMA-BUF 对象？
 *     2. 数据从这个对象的什么位置开始？
 *     3. 每一行占用多少字节？
 *     4. 这个平面使用什么像素格式？
 * 它通过 dma_buf_index_ 引用 HardwareFrame 中的 DMA-BUF 对象
 * 例如： dma_buf_objects[0].fd_ = 5
 * planes_[0].dma_buf_index_ = 0
 * planes_[1].dma_buf_index_ = 0
 * 表示两个平面都使用 dma_buf_objects_[0],也就是都使用 fd=5 对应的 DMA-BUF
 */
struct DmaBufPlane {
    /**
     * @brief DMA-BUF 对象在 HardwareFrame::dma_buf_objects_ 中的下标
     */
    std::uint32_t dma_buf_index_{0};

    // 当前平面数据在对应 DMA-BUF 对象中的起始位置
    std::uint32_t offset_bytes_{0};

    /**
     * @brief 当前平面每一行占用的字节数
     * @details
     * pitch 也常被称为 stride，它表示内存中相邻两行之间的跨度，pitch 不一定等于图像宽度
     */
    std::uint32_t pitch_bytes_{0};

    /**
     * @brief 当前平面的 DRM FourCC 格式标识
     * @details
     *     NV12 的 Y 平面  -> R8
     *     NV12 的 UV 平面 -> GR88
     *
     *     P010 的 Y 平面  -> R16
     *     P010 的 UV 平面 -> GR32
     */
    std::uint32_t drm_format_{0};
};

/**
 * @class HardwareFrame
 * @brief 描述一帧硬件解码图像及其相关资源
 * @details
 * 保存一帧硬件图像的描述信息和资源引用,让解码线程可以把硬件帧安全地传递给渲染线程
 * 为什么要持有一个 AVFrame 的引用：VA-API 解码得到的图像通常来自一个 Surface 池
 * FFmpeg 会通过引用计数来管理这些 Surface 的生命周期，要保证这一帧在没有被渲染之前
 * 一直持有对 Surface #N 的引用，保证其不会被后来的帧覆盖
 */
class HardwareFrame {
public:
    /** @brief 创建一个不持有任何 DMA-BUF 和 AVFrame 资源的空对象 */
    HardwareFrame() = default;

    /** @brief 析构函数 */
    ~HardwareFrame();

    /**
     * @brief 禁止拷贝/拷贝赋值运算符
     * @details
     * 本对象管理独占资源，普通拷贝会复制 fd 数值和 AVFrame 指针，但不会自动创建独立的资源所有权
     */
    HardwareFrame(const HardwareFrame&) = delete;

    /**
     * @brief 禁止拷贝赋值
     *
     * @details
     * 与拷贝构造相同，禁止通过赋值复制资源所有权。
     */
    HardwareFrame& operator=(const HardwareFrame&) = delete;

    /**
     * @brief 移动构造函数
     * @param other 资源的原所有者
     * @details 使用 other 创建一个对象并转移资源
     */
    HardwareFrame(HardwareFrame&& other) noexcept;

    /**
     * @brief 移动赋值
     * @param other 资源的原所有者
     * @return 将 other 对象的资源转移到另一个已经存在的对象
     */
    HardwareFrame& operator=(HardwareFrame&& other) noexcept;

    /**
     * @brief 检查项目支持的 DMA-BUF 元素以及可访问范围
     * @param param Description of parameter
     * @return Description of return value
     */

    /// 图像的宽度
    int width_{0};

    // 图像的高度
    int height_{0};

    /**
     * @brief 整个图像 layer 的 DRM FourCC 格式
     * @details
     * 例如：
     *
     *     kDrmFormatNv12
     *     kDrmFormatP010
     *
     * 它描述整个 layer 的像素格式
     */
    std::uint32_t layer_format_{0};

    /**
     * @brief 当前图像引用的全部 DMA-BUF 对象
     * @details
     * 每个元素描述一个 DMA-BUF 对象，通常一个图像只需要一个 DMA-BUF 对象，但某些导出布局可能使用多个对象
     */
    std::vector<DmaBufObject> dma_buf_objects_;

    /**
     * @brief 当前图像包含的全部采样面
     * @details
     * 常见格式：
     *
     *     NV12     -> 2 个平面：Y、UV
     *     P010     -> 2 个平面：Y、UV
     *     YUV420P  -> 3 个平面：Y、U、V
     * 每个平面通过 dma_buf_index_ 指定自己使用哪个 DMA-BUF 对象，同一个 DMA-BUF 对象可以被多个平面引用
     */
    std::vector<DmaBufPlane> planes_;

    // 当前硬件解码帧的 AVFrame 引用
    AVFrame* av_frame_ref_{nullptr};

    /**
     * @brief 检查项目支持的 DMA-BUF 元数据及可访问范围
     *
     * 只接受双平面的 NV12/P010，以及 LINEAR 和 Intel 4K tiled
     * （128 字节 x 32 行）。Intel X/Y/Yf、CCS、INVALID 和未知 modifier 均拒绝
     *
     * 本函数只做结构验证，不检查 AVFrame 引用，也不表示 EGL/GPU 驱动
     * 支持对应格式或 modifier。Renderer 仍须查询运行时能力并验证导入结果
     */
    bool ValidateDmaBufLayout() const noexcept;

private:
    /**
     * @brief 释放当前对象持有的全部资源
     * @details
     * 主要负责：
     * 1. 关闭 dma_buf_objects_ 中的所有 fd
     * 2. 释放 av_frame_ref_ 持有的 AVFrame 引用
     * 3. 将已经释放的资源置空
     */
    void ReleaseResources() noexcept;
};

/**
 * @brief 将 DRM FourCC 转换为便于阅读的格式化名称
 * @param drm_format DRM FourCC 格式标识
 * @return 格式名字符串：例如 "NV12"、"P010"；未知格式返回四字符名或十六进制
 * @note 未知格式使用 thread_local 缓冲区，返回指针只在当前线程下一次调用前有效，只适合立刻用于日志
 *
 * 它是与 HardwareFrame 无关的自由函数：之前声明成了私有成员（且拼写为 DrmFourCCName），
 * 而 .cpp 里定义的是另一个名字的自由函数，解码器调用 GetDrmFourccName 时找不到声明。
 */
const char* GetDrmFourccName(std::uint32_t drm_format) noexcept;

// Process-lifetime observations of existing releases, not an ownership registry.
struct HardwareFrameReleaseStats {
    std::uint64_t fd_close_success{0};
    std::uint64_t fd_close_failed{0};
    std::uint64_t av_frame_refs_released{0};
};
HardwareFrameReleaseStats GetHardwareFrameReleaseStats() noexcept;

}  // namespace mediacore

/**
 * @file opengl_view.cpp
 * @brief OpenGLView 视频渲染控件的实现
 * @details
 * 本文件实现软件帧和硬件帧两条渲染路径：
 * 1. 线程边界：
 *    OnReceiveVideoFrame() 可以由后台工作线程调用，只更新受互斥锁保护的帧信箱，
 *    然后通过 Qt 队列信号请求 GUI 线程重绘
 *    OpenGL / EGL 操作由 GUI 线程执行，并且必须在合适的 GL Context 为 current 时进行
 *    这些操作分布在 initializeGL()、paintGL() 及其调用的辅助函数中，
 *    资源清理则由 ReleaseOpenGlResources() 等函数完成
 *
 * 2. 软件帧路径：
 *    将 CPU 内存中的 Y/U/V 像素数据上传到 OpenGL Texture，
 *    再由 Shader 完成 YUV → RGB 转换并绘制到 Framebuffer
 *
 * 3. 硬件帧路径：
 *    VA-API 解码后的图像存储通过 DMA-BUF 导出，Renderer 再通过 EGL 导入为 EGLImage，
 *    并关联到 OpenGL Texture，供 Shader 读取。
 *    正常导入成功时，目标是直接共享原有图像存储，避免整帧先下载到 CPU 再上传到 GPU
 *    是否能直接共享取决于设备、驱动、FourCC、Modifier 和 Plane 布局是否兼容
 *
 * 4. 回退路径：
 *    硬件图像导入或渲染失败时，尝试将当前硬件帧下载到 CPU，转换成软件帧后渲染
 *
 * 5. 资源生命周期：
 *    OpenGL 绘制命令通常异步执行。提交绘制后通过 GL Fence 检查 GPU 是否已完成读取，
 *    Fence 完成之前保留对应的 EGLImage、Texture 和 VideoFrame，避免底层 VA Surface 被提前复用
 */

#include "opengl_view.h"

#include "deferred_resource_queue.h"

#include <QtGui/qopenglcontext_platform.h>

#include <QMatrix3x3>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QTimer>
#include <QVector2D>
#include <QVector3D>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "mediacore/av_types/hardware_frame.h"
#include "mediacore/av_types/video_frame.h"
#include "mediacore/logger.h"
namespace {

/**
 * @brief 顶点着色器源码（GLSL 3.30 Core Profile）
 * @details
 * 顶点着色器针对全屏矩形的 4 个网格顶点执行：
 * 1. 坐标透传：CPU 传入的顶点坐标 aPos 已经是归一化设备坐标（NDC, [-1.0, 1.0]），
 *    无需模型-视图-投影（MVP）变换矩阵计算，直接补全为 4D 齐次裁剪坐标 gl_Position；
 * 2. 纹理插值传递：将纹理 UV 坐标 aTexCoord 透传至输出管道，由光栅化硬件在片元间自动完成透视校正插值
 */
constexpr auto kVertexShaderSource = R"(#version 330 core

layout(location = 0) in vec2 aPos;      // 顶点位置输入插槽（属性槽 0）
layout(location = 1) in vec2 aTexCoord; // 纹理坐标输入插槽（属性槽 1）

out vec2 vTexCoord;                     // 输出变量：插值后传给片段着色器

void main() {
    // 决定顶点在屏幕空间的位置（z=0.0，齐次分量 w=1.0）
    gl_Position = vec4(aPos, 0.0, 1.0);

    // 透传纹理坐标
    vTexCoord = aTexCoord;
}
)";

/**
 * @brief YUV420P 至 RGB 色彩空间转换片段着色器源码（GLSL 3.30 Core Profile）
 * @details
 * 片段着色器在 GPU 上对每个光栅化像素并行执行：
 * 1. 独立单通道纹理采样：分别从 Y、U、V 三张 GL_R8 单通道纹理中采样获取原始分量；
 * 2. 色彩范围归一：先用 u_yuvOffset 减去「黑电平 / 色度中点」，再用 u_yuvScale 拉伸到满量程，
 *    还原为 Y∈[0,1]、U/V∈[-0.5,0.5] 的 full-range 数值。offset/scale 由 CPU 按帧的色彩范围算好传入：
 *      · full-range ：offset=(0, 128/255, 128/255)，scale=(1, 1, 1)，只做色度居中；
 *      · limited-range（电视范围）：offset=(16/255, 128/255, 128/255)，scale=(255/219, 255/224, 255/224)，
 *        把 Y∈[16,235]、UV∈[16,240] 拉伸回满量程。漏掉这一步，黑色会停在 16/255 而不是 0，画面发灰；
 * 3. 硬件矩阵乘法：通过 uniform mat3 矩阵一步完成色彩空间投射，输出最终 Alpha 为 1.0 的 RGB 像素。
 */
constexpr auto kFragmentShaderSource = R"(#version 330 core

in vec2 vTexCoord;       // 顶点着色器插值而来的纹理 UV 坐标
out vec4 fragColor;      // 输出：该片元最终的 RGBA 物理颜色

uniform sampler2D texY;  // Y 亮度平面纹理采样器（绑定至纹理单元 0）
uniform sampler2D texU;  // U 色度平面纹理采样器（绑定至纹理单元 1）
uniform sampler2D texV;  // V 色度平面纹理采样器（绑定至纹理单元 2）

// 由 CPU 传入的 3x3 色彩转换矩阵（BT.709 标准）
uniform mat3 u_colorMatrix;
uniform vec3 u_yuvOffset;  // 黑电平与色度中点（归一化值）
uniform vec3 u_yuvScale;   // 把 limited-range 拉伸到满量程的系数

void main() {
    // 1. 采样并组装原始 YUV 向量（使用 .r 读取红色通道，即单通道 R8 存储格式）
    vec3 yuv = vec3(texture(texY, vTexCoord).r,
                    texture(texU, vTexCoord).r,
                    texture(texV, vTexCoord).r);

    // 2. 色彩范围归一：减黑电平/色度中点，再按范围拉伸
    yuv = (yuv - u_yuvOffset) * u_yuvScale;

    // 3. 硬件级矩阵乘法 (YUV -> RGB)
    vec3 rgb = u_colorMatrix * yuv;

    // 4. 输出不透明颜色
    fragColor = vec4(rgb, 1.0);
}
)";

/**
 * @brief 零拷贝硬件路径专用片段着色器
 *
 * 【与上面软件着色器的三点区别】
 *
 * 1. 两个 sampler 而不是三个
 *    NV12 / P010 都是「一亮度 + 一交织色度」的两平面格式：
 *      texY  : Y 平面，单通道（NV12 用 GL_R8，P010 用 GL_R16）
 *      texUV : UV 交织平面，双通道（NV12 用 GL_RG8，P010 用 GL_RG16）
 *              一个 texel 里 .r 是 U、.g 是 V
 *
 *    实测验证（09_memory_object_fd）：uv.r == U、uv.g == V，顺序正确。
 *
 * 2. 位深归一化
 *    GL_R16/GL_RG16 是归一化定点格式，采样返回的是 v/65535 而不是原始整数。
 *    P010 的有效 10-bit 数据存在 16-bit 容器的高位（code10 << 6），
 *    所以采样值天然接近 code10/1023 —— 只需乘一个接近 1 的因子即可精确还原。
 *
 *    u_bitDepthScale：
 *      · 8-bit（NV12）传 1.0                —— 数据已经是 code8/255
 *      · 10-bit（P010）传 65535/(64*1023) ≈ 1.000962
 *
 *    ⚠️ 常见错误：写成「采样值 × 255/1023」会让数值缩到约四分之一，画面严重过暗。
 *       采样值已经是归一化的，不存在「先转成 8-bit 尺度」这一步。
 *
 * 3. 色彩范围处理与软件路径共用同一套约定
 *    先归一化位深，再用 u_yuvOffset / u_yuvScale 做 limited→full 的黑电平校正，
 *    最后乘同一个 u_colorMatrix。CPU 侧按「帧的色彩范围 + 位深（NV12=8，P010=10）」计算这两个向量，
 *    保证两条路径对同一个片源渲染出一致的画面。
 */
constexpr auto kHardwareFrameFragmentShaderSource = R"(#version 330 core

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D texY;     // Y 平面（GL_R8 或 GL_R16）
uniform sampler2D texUV;    // UV 交织平面（GL_RG8 或 GL_RG16）
uniform mat3 u_colorMatrix;      // YUV→RGB 矩阵，与软件路径共用同一组常量
uniform float u_bitDepthScale;   // 8-bit 传 1.0；10-bit 传 65535/(64*1023)
uniform vec3 u_yuvOffset;        // 黑电平与色度中点，已按位深归一化
uniform vec3 u_yuvScale;         // 把 limited-range 拉伸到满量程的系数
uniform vec2 u_yScale;
uniform vec2 u_yBias;
uniform vec2 u_uvScale;
uniform vec2 u_uvBias;

void main() {
    // 采样并做位深归一化，得到 [0,1] 范围的原始 YUV 分量
    float y  = texture(texY, vTexCoord * u_yScale + u_yBias).r * u_bitDepthScale;
    vec4  uv = texture(texUV, vTexCoord * u_uvScale + u_uvBias);
    float u  = uv.r * u_bitDepthScale;
    float v  = uv.g * u_bitDepthScale;

    // 减黑电平/色度中点，再按色彩范围拉伸（与软件路径同一约定）
    vec3 yuv = (vec3(y, u, v) - u_yuvOffset) * u_yuvScale;

    // 色彩矩阵乘法，与软件路径完全一致
    fragColor = vec4(u_colorMatrix * yuv, 1.0);
}
)";

/**
 * @brief 全屏四边形几何顶点数据定义：4 个顶点 × (2维位置 + 2维纹理坐标)
 * @details
 * 1. 屏幕坐标（NDC）：范围 [-1.0, 1.0]，(-1, -1) 为左下角，(1, 1) 为右上角，填满整个渲染视口；
 * 2. 纹理坐标（UV）：图像首行数据（物理顶部）在内存首地址，而 OpenGL 纹理原点默认在左下角，
 *    因此对 V 坐标进行轴向颠倒映射（顶部为 0.0，底部为 1.0），避免画面倒置。
 */
constexpr std::array<float, 16> kVertices = {
    // NDC 坐标 (X, Y)  // 纹理坐标 (U, V，V 轴已做防颠倒倒置)
    -1.0f,
    -1.0f,
    0.0f,
    1.0f,  // 屏幕左下角 -> 画面底部
    1.0f,
    -1.0f,
    1.0f,
    1.0f,  // 屏幕右下角 -> 画面底部
    1.0f,
    1.0f,
    1.0f,
    0.0f,  // 屏幕右上角 -> 画面顶部
    -1.0f,
    1.0f,
    0.0f,
    0.0f  // 屏幕左上角 -> 画面顶部
};

/**
 * @brief 索引缓冲数据（EBO）：指定顶点图元组装为双三角形四边形的顺序
 * @details 采用逆时针（CCW）绕序组装两个标准三角形以覆盖全屏矩形。
 */
constexpr std::array<unsigned int, 6> kIndices = {
    0,
    1,
    2,  // 第一个三角形：左下 -> 右下 -> 右上
    2,
    3,
    0  // 第二个三角形：右上 -> 左上 -> 左下
};

/**
 * @brief ITU-R BT.709 标准 HDTV 色彩空间转换矩阵（列优先，Column-Major）
 * @details
 * 数学公式（full range）：
 * R = Y + 1.5748 * V
 * G = Y - 0.1873 * U - 0.4681 * V
 * B = Y + 1.8556 * U
 *
 * OpenGL/Qt 的 mat3 按列优先存储：第 1 列是 Y 系数，第 2 列 U 系数，第 3 列 V 系数
 */
constexpr std::array<float, 9> kMatrixBt709 = {
    1.0f,
    1.0f,
    1.0f,  // 第 1 列：Y 的加权系数（R/G/B 都需要 Y 分量）
    0.0f,
    -0.1873f,
    1.8556f,  // 第 2 列：U 的加权系数（只影响 G 和 B）
    1.5748f,
    -0.4681f,
    0.0f  // 第 3 列：V 的加权系数（只影响 R 和 G）
};

/**
 * @brief ITU-R BT.601 标准清晰度（SD）色彩空间转换矩阵（列优先）
 * @details
 * R = Y + 1.402 * V
 * G = Y - 0.344136 * U - 0.714136 * V
 * B = Y + 1.772 * U
 */
constexpr std::array<float, 9> kMatrixBt601 = {1.0f, 1.0f, 1.0f, 0.0f, -0.344136f, 1.772f, 1.402f, -0.714136f, 0.0f};

/**
 * @brief ITU-R BT.2020 超高清（UHD）色彩空间转换矩阵（列优先）
 * @details
 * R = Y + 1.4746 * V
 * G = Y - 0.164553 * U - 0.571353 * V
 * B = Y + 1.8814 * U
 */
constexpr std::array<float, 9> kMatrixBt2020 = {1.0f, 1.0f, 1.0f, 0.0f, -0.164553f, 1.8814f, 1.4746f, -0.571353f, 0.0f};

/**
 * @brief 把「按列优先书写的 3x3 数组」转换成可直接传给 setUniformValue 的 QMatrix3x3
 * @param column_major_values 9 个 float，依次是第 1 列、第 2 列、第 3 列（即上面 kMatrixBt* 的书写顺序）
 * @return GLSL 里 mat3 能正确解释的矩阵
 *
 * 这里必须转置，原因是两个约定相反：
 *   · QMatrix3x3(const float*) 的构造函数按「行优先」读入：m[列][行] = values[行 * 3 + 列]；
 *   · 而 kMatrixBt* 是按「列优先」书写的（第 1 个三元组是 Y 的系数列）。
 * 直接 QMatrix3x3(array.data()) 会得到转置矩阵：YUV(200,100,150) 本应得到 RGB(235,195,149)，
 * 实际算出 (195,47,255)，画面整体偏紫。QMatrix3x3 的 constData() 内部是列优先，
 * 所以先构造再 transposed() 就等价于把数组原样按列优先上传。
 */
QMatrix3x3 ToColumnMajorGlslMatrix(const std::array<float, 9>& column_major_values) {
    return QMatrix3x3(column_major_values.data()).transposed();
}

/**
 * @struct YuvRangeParameters
 * @brief 着色器里「(采样值 - offset) * scale」所需的两个向量，用来把任意色彩范围还原成满量程
 */
struct YuvRangeParameters {
    QVector3D offset_;  ///< (Y 黑电平, U 中点, V 中点)，已除以该位深的最大码值，范围 [0,1]
    QVector3D scale_;  ///< (Y 拉伸, U 拉伸, V 拉伸)
};

/**
 * @brief 按色彩范围与位深计算 YuvRangeParameters
 * @param color_range 帧的色彩范围；kUnspecified 按 limited 处理（绝大多数视频是电视范围）
 * @param bit_depth 有效位深：NV12 与 YUV420P 为 8，P010 为 10
 * @return offset 与 scale
 *
 * 公式（以 8-bit 为例，10-bit 把 16/128/219/224 乘 4、最大码值换成 1023）：
 *   full-range   ：Y 与 U/V 都占满 [0,255]，只需把色度中点 128 移到 0 → offset=(0,128/255,128/255)，scale=1
 *   limited-range：Y∈[16,235]（219 档）、U/V∈[16,240]（224 档）
 *                  → offset=(16/255,128/255,128/255)，scale=(255/219,255/224,255/224)
 */
YuvRangeParameters GetYuvRangeParameters(mediacore::VideoColorRange color_range, int bit_depth) {
    const float shift = static_cast<float>(1 << (bit_depth - 8));  // 8-bit 为 1，10-bit 为 4
    const float max_code = static_cast<float>((1 << bit_depth) - 1);  // 8-bit 为 255，10-bit 为 1023
    const float chroma_center = 128.0F * shift / max_code;

    if (color_range == mediacore::VideoColorRange::kFull) {
        return {QVector3D(0.0F, chroma_center, chroma_center), QVector3D(1.0F, 1.0F, 1.0F)};
    }

    const float luma_black = 16.0F * shift / max_code;
    return {QVector3D(luma_black, chroma_center, chroma_center),
            QVector3D(max_code / (219.0F * shift), max_code / (224.0F * shift), max_code / (224.0F * shift))};
}

/**
 * @brief 检查指定的 EGL 扩展是否存在于扩展列表中
 * @param extension_list EGL 返回的扩展名称字符串，多个扩展之间使用空格分隔
 * @param requested_extension 要检查的扩展名称，例如 EGL_EXT_image_dma_buf_import
 * @return 找到完整匹配的扩展名称返回 true，否则返回 false。
 */
bool HasEglExtension(const char* extension_list, const char* requested_extension) {
    // 检查输入是否合法。
    // 扩展列表为空，或者请求的扩展名称为空，都视为不支持。
    if (extension_list == nullptr || requested_extension == nullptr || requested_extension[0] == '\0') {
        return false;
    }

    // 获取需要检查的扩展名称长度。
    const std::size_t requested_length = std::strlen(requested_extension);

    // 从扩展列表的开头开始查找。
    const char* extension = extension_list;

    // 查找请求的扩展名称是否出现在扩展列表中。
    while ((extension = std::strstr(extension, requested_extension)) != nullptr) {
        // 检查匹配位置前面是否为扩展名称的起始位置。
        // 如果是字符串开头，或者前一个字符是空格，说明匹配从一个完整名称开始。
        const bool starts_at_token = extension == extension_list || extension[-1] == ' ';

        // 检查匹配位置后面的字符。
        const char following_character = extension[requested_length];

        // 只有前后都是有效边界，才说明找到了完整的扩展名称。
        // 避免将其他扩展名称中的一部分误认为目标扩展。
        if (starts_at_token && (following_character == '\0' || following_character == ' ')) {
            return true;
        }

        // 当前匹配不是完整扩展名称，继续从本次匹配位置之后查找。
        extension += requested_length;
    }

    // 遍历完扩展列表，仍未找到完整匹配的扩展名称。
    return false;
}

/**
 * @brief 将 uint32_t 数值的二进制表示复制到 EGLint 中
 * @param value 需要编码的 32 位 无符号数值，通常用于表示 DRM FourCC
 * @return 保留原始 32 位二进制内容的 EGLint
 * @details
 * EGL 的属性列表使用 EGLint 保存属性值，而 DRM FourCC 通常使用 uint32_t 表
 * 两者虽然都是 32 位整数，但符号不同：
 *   uint32_t：无符号整数
 *   EGLint  ：有符号整数
 * @note
 * 主要用于将 DRM FourCC 等 32 位数值放入 EGL 属性列表
 */
EGLint EncodeUint32AsEglInt(std::uint32_t value) {
    // 确保 EGLint 和 uint32_t 都是 32 位
    static_assert(sizeof(EGLint) == sizeof(value));

    // 初始化 EGLint 接收变量
    EGLint encoded_value = 0;

    // 原样复制 32 位二进制内容，不进行有符号和无符号之间的数值转换。
    std::memcpy(&encoded_value, &value, sizeof(encoded_value));
    return encoded_value;
}

}  // namespace

class OpenGLView::DeferredGlFrameBatch final : public DeferredResourceBatch {
public:
    DeferredGlFrameBatch(std::unique_ptr<QOpenGLContext> context,
                         std::unique_ptr<QOffscreenSurface> surface,
                         EGLDisplay display,
                         PFNEGLDESTROYIMAGEKHRPROC destroy_image)
        : surface_(std::move(surface)),
          context_(std::move(context)),
          display_(display),
          destroy_image_(destroy_image) {}

    void Adopt(std::vector<std::unique_ptr<PendingFrameResources>>& resources) noexcept {
        resources_.swap(resources);
    }

    bool ReclaimIfSafe() noexcept override {
        if (resources_.empty()) {
            return true;
        }
        if (context_ == nullptr || surface_ == nullptr || !context_->isValid() ||
            !context_->makeCurrent(surface_.get())) {
            return false;
        }

        QOpenGLExtraFunctions* functions = context_->extraFunctions();
        if (functions == nullptr) {
            context_->doneCurrent();
            return false;
        }
        functions->initializeOpenGLFunctions();

        for (auto it = resources_.begin(); it != resources_.end();) {
            PendingFrameResources& resources = **it;
            if (!resources.gpu_use_is_complete_) {
                if (resources.fence_ == nullptr) {
                    ++it;
                    continue;
                }

                const GLenum status = functions->glClientWaitSync(resources.fence_, 0, 0);
                if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
                    ++it;
                    continue;
                }
                resources.gpu_use_is_complete_ = true;
                functions->glDeleteSync(resources.fence_);
                resources.fence_ = nullptr;
            }

            functions->glDeleteTextures(static_cast<GLsizei>(resources.textures_.size()), resources.textures_.data());
            resources.textures_ = {0, 0};

            bool images_destroyed = true;
            for (EGLImageKHR& image : resources.egl_images_) {
                if (image == EGL_NO_IMAGE_KHR) {
                    continue;
                }
                if (destroy_image_ == nullptr || display_ == EGL_NO_DISPLAY ||
                    destroy_image_(display_, image) != EGL_TRUE) {
                    images_destroyed = false;
                    break;
                }
                image = EGL_NO_IMAGE_KHR;
            }
            if (!images_destroyed) {
                ++it;
                continue;
            }

            // The frame owns the DMA-BUF FDs and VA surface reference; release it only
            // after the Fence and both EGLImage imports have been retired.
            resources.video_frame_.reset();
            it = resources_.erase(it);
        }

        context_->doneCurrent();
        return resources_.empty();
    }

private:
    std::vector<std::unique_ptr<PendingFrameResources>> resources_;
    std::unique_ptr<QOffscreenSurface> surface_;
    std::unique_ptr<QOpenGLContext> context_;
    EGLDisplay display_{EGL_NO_DISPLAY};
    PFNEGLDESTROYIMAGEKHRPROC destroy_image_{nullptr};
};

namespace {

class DeferredGlResourceReaper final : public QObject {
public:
    DeferredGlResourceReaper() {
        timer_.setInterval(50);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] {
            queue_.Reap();
            if (queue_.Empty()) {
                timer_.stop();
            }
        });
    }

    bool Defer(std::unique_ptr<DeferredResourceBatch>& batch) {
        if (!queue_.Defer(batch)) {
            hardware_path_permanently_disabled_ = true;
            return false;
        }
        timer_.start();
        return true;
    }

    bool CanRenderHardware() const noexcept {
        return !hardware_path_permanently_disabled_ && queue_.Empty();
    }

    void DisableHardwarePermanently() noexcept { hardware_path_permanently_disabled_ = true; }

private:
    DeferredResourceQueue queue_;
    QTimer timer_;
    bool hardware_path_permanently_disabled_{false};
};

DeferredGlResourceReaper& GetDeferredGlResourceReaper() {
    // Keep the reaper alive through QApplication teardown. Unconfirmed GPU resources
    // must remain owned until their Fence completes or the process exits.
    static auto* reaper = new DeferredGlResourceReaper();
    return *reaper;
}

}  // namespace

/**
 * @brief 构造函数：初始化视窗控件并绑定跨线程渲染信号槽
 * @param parent Qt 父对象指针
 * @details
 * 使用 Qt::QueuedConnection 将 FrameArrived 信号连接至自身的 update() 槽；
 * 确保后台工作线程无论何时 emit 该信号，最终的绘制驱动 update() 严格在 Qt GUI 主线程执行。
 */
OpenGLView::OpenGLView(QWidget* parent) : QOpenGLWidget(parent) {
    renderer_statistics_started_at_ = std::chrono::steady_clock::now();
    renderer_interval_started_at_ = renderer_statistics_started_at_;
    connect(this, &OpenGLView::FrameArrived, this, QOverload<>::of(&QOpenGLWidget::update), Qt::QueuedConnection);
}

/**
 * @brief 析构函数：调用统一清理逻辑释放本控件持有的 GL / EGL 资源。
 * @details
 * 实际清理由 ReleaseOpenGlResources() 完成：优先使用 QOpenGLWidget 自身 Context，
 * 自身 Context 不可用时尝试使用共享的 retirement_context_。
 * 如果无法确认 GPU 已经停止读取某帧，则隔离保留该帧资源，避免提前释放 DMA-BUF fd 和 AVFrame 引用。
 * @note 不应假定析构函数执行时原 Context 一定仍可用。
 */
OpenGLView::~OpenGLView() {
    const double average_download_ms = renderer_download_count_ == 0
        ? 0.0
        : renderer_download_total_ms_ / static_cast<double>(renderer_download_count_);
    LOG_INFO("RendererDownloadStats reason=destroyed downloads={} failures={} avg_download_ms={:.3f} "
             "max_download_ms={:.3f}",
             renderer_download_count_,
             renderer_download_failures_,
             average_download_ms,
             renderer_download_max_ms_);
    ReleaseOpenGlResources();
}

/**
 * @brief 接收上游投递的软件帧或硬件帧（IVideoSink 实现）。
 * @param video_frame 通过 shared_ptr 共享所有权的视频帧。
 * @note 该函数由上游工作线程调用，只更新帧信箱并通知 GUI 线程，不执行 OpenGL / EGL 操作。
 */
void OpenGLView::OnReceiveVideoFrame(std::shared_ptr<mediacore::VideoFrame> video_frame) {
    // 1. 丢弃空帧，避免把无效对象写入信箱。
    if (video_frame == nullptr) {
        LOG_WARN("Video frame ignored: null frame");
        return;
    }

    LOG_TRACE("Video frame received: {}x{}", video_frame->width_, video_frame->height_);

    // 2. 在锁的保护下替换信箱中的帧。
    //    move 只转移 shared_ptr，不复制视频像素；被替换的旧帧会减少一份引用。
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        current_frame_ = std::move(video_frame);
    }

    // 3. 释放互斥锁后再发送信号，避免 GUI 线程处理重绘时与信箱锁产生不必要的竞争。
    emit FrameArrived();
}

/**
 * @brief 保存 Decoder 与 Renderer 共享的硬件渲染能力标志
 * @param capability 由 Player 创建并同时交给 Decoder、Renderer 的原子布尔对象
 * @note Renderer 更新该标志，Decoder 读取该标志；本函数只保存 shared_ptr，不创建新的状态对象
 */
void OpenGLView::SetHardwareRenderCapability(const std::shared_ptr<std::atomic<bool>>& capability) {
    // 保存共享状态对象；后续 initializeGL() 和资源清理函数会更新它。
    hardware_render_capability_ = capability;
}

/**
 * @brief 初始化 QOpenGLWidget 使用的 OpenGL / EGL 渲染环境。
 * @note Qt 调用本函数时，控件对应的 OpenGL Context 已经是 current。
 * @note glGenTextures() 只创建纹理对象名称；软件纹理存储在首次上传像素数据时分配。
 */
void OpenGLView::initializeGL() {
    LOG_INFO("OpenGL initialization started");

    // 1. 初始化 Qt 提供的 OpenGL 3.3 Core 函数入口。
    initializeOpenGLFunctions();

    // 2. 设置清屏颜色（RGBA）。
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);

    // 3. 断开上一次初始化留下的 Context 销毁连接，避免重复连接同一个清理回调。
    if (context_destroyed_connection_) {
        disconnect(context_destroyed_connection_);
    }
    context_destroyed_connection_ = connect(context(),
                                            &QOpenGLContext::aboutToBeDestroyed,
                                            this,
                                            &OpenGLView::ReleaseOpenGlResources,
                                            Qt::DirectConnection);

    // 4. 提前创建共享的离屏 Context，供原 Widget Context 不可用时回收共享 GL 资源。
    //    创建失败时，InitializeEglDmaBufImport() 会拒绝启用 DMA-BUF 硬件渲染。
    if (!CreateRetirementContext(context())) {
        LOG_WARN("Could not create a shared retirement OpenGL context; DMA-BUF rendering is disabled");
    }

    // 5. 检查当前 Qt Context 是否为 EGL Context，并初始化 DMA-BUF 导入能力。
    //    只有 EGL 导入能力完整可用，才继续编译硬件帧 Shader。
    hardware_retirement_suspended_ = !GetDeferredGlResourceReaper().CanRenderHardware();
    if (InitializeEglDmaBufImport()) {
        hardware_shader_ready_ = BuildHardwareFrameShader();
        if (!hardware_shader_ready_) {
            LOG_WARN("Hardware frame shader failed to build; hardware frames will use software download");
        }
    } else {
        hardware_shader_ready_ = false;
        LOG_INFO("Qt EGL DMA-BUF import is unavailable; hardware frames will use software download");
    }

    // 6. 初始化软件渲染所需的 Shader。
    shader_ready_ = BuildSoftwareFrameShader();

    // 7. 创建硬件和软件路径共用的全屏四边形几何数据。
    CreateFullscreenQuadGeometry();

    // 8. 创建软件 Y/U/V 纹理对象。这里创建的是对象名称，像素存储稍后上传时分配。
    CreateSoftwareFrameTextures();

    // 9. 到这里，EGL 导入函数、硬件 Shader 和绘制几何数据都已经完成初始化。
    // 再通知 Decoder 是否可以尝试导出 DMA-BUF，避免 Decoder 提前开始导出。
    // 软件 Shader 是否成功不决定硬件路径能否运行；硬件路径使用独立的 hardware_shader_program_。
    if (hardware_render_capability_ != nullptr) {
        const bool hardware_path_ready = egl_import_.is_available_ && hardware_shader_ready_ &&
            hardware_shader_program_ != nullptr && vao_.isCreated() && vbo_.isCreated() && ebo_.isCreated() &&
            !hardware_retirement_suspended_;
        hardware_render_capability_->store(hardware_path_ready, std::memory_order_release);
        LOG_INFO("Hardware render capability flag set to {}", hardware_path_ready);
    }

    if (shader_ready_) {
        LOG_INFO("OpenGL initialization finished");
    } else {
        LOG_ERROR("OpenGL initialization finished; software shader is unavailable");
    }
}

/**
 * @brief 创建一个与 QOpenGLWidget Context 共享资源的离屏 Context。
 * @param share_context 作为共享源的 QOpenGLWidget Context。
 * @return 创建成功且确认共享关系有效时返回 true。
 * @note 调用时 Widget Context 应为 current；函数结束前会尝试恢复 Widget Context。
 */
bool OpenGLView::CreateRetirementContext(QOpenGLContext* share_context) {
    // 1. 清除上一次创建的离屏 Context 和 Surface。
    retirement_context_.reset();
    retirement_surface_.reset();

    // 2. 没有有效的共享源 Context 时，无法建立共享资源组。
    if (share_context == nullptr || !share_context->isValid()) {
        return false;
    }

    // 3. 创建新的离屏 OpenGL Context，并指定与 Widget Context 相同的格式。
    auto retirement_context = std::make_unique<QOpenGLContext>();
    retirement_context->setFormat(share_context->format());
    retirement_context->setShareContext(share_context);
    if (!retirement_context->create()) {
        LOG_WARN("Failed to create shared retirement OpenGL context");
        return false;
    }

    // 4. 为离屏 Context 创建绑定目标。QOffscreenSurface 不显示到窗口，只用于 makeCurrent()。
    auto retirement_surface = std::make_unique<QOffscreenSurface>();
    retirement_surface->setFormat(retirement_context->format());
    retirement_surface->create();
    if (!retirement_surface->isValid()) {
        LOG_WARN("Failed to create retirement offscreen surface");
        return false;
    }

    // 5. 临时切换到离屏 Context，确认它与 Widget Context 确实处于同一个共享资源组。
    if (!retirement_context->makeCurrent(retirement_surface.get())) {
        LOG_WARN("Failed to make retirement OpenGL context current");
        return false;
    }
    const bool shares_resources = QOpenGLContext::areSharing(share_context, retirement_context.get());
    retirement_context->doneCurrent();

    // 6. 恢复 Widget Context。
    //    后续 initializeGL() 还要在 Widget Context 中创建 Shader、VAO、VBO 和纹理。
    makeCurrent();
    if (QOpenGLContext::currentContext() != share_context) {
        LOG_WARN("Failed to restore the QOpenGLWidget context after retirement setup");
        return false;
    }
    // 7. 重新初始化 Qt OpenGL 函数包装器，使后续调用使用当前 Widget Context。
    //    资源清理切换到 retirement Context 时，也会再次初始化该函数包装器。
    initializeOpenGLFunctions();

    // 8. 只有确认资源共享关系有效，才保存这两个对象供后续清理使用。
    if (!shares_resources) {
        LOG_WARN("Retirement OpenGL context was created without a shared resource group");
        return false;
    }

    retirement_context_ = std::move(retirement_context);
    retirement_surface_ = std::move(retirement_surface);
    return true;
}

/**
 * @brief 视窗几何尺寸重置响应
 * @param width 控件新的物理像素宽度
 * @param height 控件新的物理像素高度
 * @details 动态调整 glViewport 视口，映射归一化设备坐标至物理窗口区域。
 */
void OpenGLView::resizeGL(int width, int height) {
    // 将 OpenGL 视口设置为控件当前的物理像素尺寸。
    glViewport(0, 0, width, height);
    LOG_INFO("OpenGL viewport resized to {}x{}", width, height);
}

/**
 * @brief 根据当前视频帧类型选择硬件或软件渲染路径
 * @note Qt 调用本函数时，QOpenGLWidget 的绘制 Context 和 Framebuffer 已经准备好
 * @note 硬件和软件路径使用不同的 Shader，必须分别检查各自的就绪状态
 */
void OpenGLView::paintGL() {
    // 0. 清除当前 QOpenGLWidget 的绘制目标
    glClear(GL_COLOR_BUFFER_BIT);

    // 1. 检查此前提交给 GPU 的帧是否已经完成读取。
    //    Fence 完成后才释放对应的 Texture、EGLImage 和 VideoFrame 引用；
    //    如果 VideoFrame 的最后一个引用被释放，其 HardwareFrame 才可能释放 fd / AVFrame 引用
    ReclaimCompletedFrameResources();

    // 硬件帧和软件帧使用不同的 Shader。只要其中一条路径可用，就允许继续检查当前帧。
    const bool software_path_ready = shader_ready_ && shader_program_ != nullptr;
    const bool hardware_path_ready =
        egl_import_.is_available_ && hardware_shader_ready_ && hardware_shader_program_ != nullptr &&
        !hardware_retirement_suspended_ && GetDeferredGlResourceReaper().CanRenderHardware();
    if (!software_path_ready && !hardware_path_ready) {
        LOG_TRACE("Paint skipped because neither hardware nor software shader path is ready");
        return;
    }

    // 2. 从帧信箱中复制一份 shared_ptr，锁外继续处理时保证 VideoFrame 仍然存活
    std::shared_ptr<mediacore::VideoFrame> frame;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame = current_frame_;
    }

    // 校验帧数据有效性（过滤 Flush / 空帧控制信令）
    if (frame == nullptr || !frame->IsValid()) {
        LOG_TRACE("Paint skipped because no valid frame is available");
        return;
    }

    // 3. 根据帧的色彩空间选择 YUV → RGB 转换矩阵。
    //    元数据未指定时，暂按分辨率推断：≥720p 使用 BT.709，否则使用 BT.601
    const auto* matrix = &kMatrixBt709;  // 默认 BT.709
    switch (frame->color_space_) {
        case mediacore::VideoColorSpace::kBt709:
            matrix = &kMatrixBt709;
            break;
        case mediacore::VideoColorSpace::kBt601:
            matrix = &kMatrixBt601;
            break;
        case mediacore::VideoColorSpace::kBt2020:
            matrix = &kMatrixBt2020;
            break;
        case mediacore::VideoColorSpace::kUnspecified:
        default:
            // 推断：高清及以上用 BT.709，标清用 BT.601
            matrix = (frame->height_ >= 720) ? &kMatrixBt709 : &kMatrixBt601;
            break;
    }

    // ==================== 分叉：硬件帧 vs 软件帧 ====================
    //
    // frame->hardware_ != nullptr：尝试通过 DMA-BUF 导入路径直接共享原有图像存储
    // frame->hardware_ == nullptr：CPU 内存中的软件帧，需要上传 Y/U/V 三个平面
    //
    // 硬件导入或绘制失败时，只有软件 Shader 可用才执行下载回退
    std::optional<mediacore::VideoFrame> downloaded_frame;
    const mediacore::VideoFrame* frame_to_render = frame.get();
    if (frame->hardware_ != nullptr) {
        if (hardware_path_ready && RenderHardwareFrameWithEglImages(frame, *matrix)) {
            return;
        }

        // 没有可用的软件 Shader 时，无法执行下载后的软件渲染回退。
        if (!software_path_ready) {
            LOG_WARN("Hardware rendering failed and software fallback is unavailable");
            return;
        }

        downloaded_frame.emplace();
        if (!DownloadHardwareFrameToSoftwareFrame(*frame, *downloaded_frame)) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_hardware_frame_failure_log_ >= std::chrono::seconds(5)) {
                last_hardware_frame_failure_log_ = now;
                LOG_WARN("Hardware frame could not be imported or downloaded for software rendering");
            }
            return;
        }
        frame_to_render = &*downloaded_frame;
    } else if (!software_path_ready) {
        // 当前是软件帧，但软件 Shader 不可用。
        return;
    }

    // 4. 读取色彩范围信息，用于日志，以及下面计算黑电平校正参数
    //    Limited range (video range): Y [16,235], UV [16,240]
    //    Full range (pc range): Y/UV [0,255]
    //    kUnspecified 默认按 Limited 处理（最保守，大部分视频都是 Limited）
    const bool is_full_range = (frame_to_render->color_range_ == mediacore::VideoColorRange::kFull);

    // 5. 软件渲染路径：将 CPU 内存中的 Y/U/V 三个平面上传到对应 OpenGL 纹理。
    LOG_TRACE("Painting frame: {}x{}, color_space={}, color_range={}, using {}",
              frame_to_render->width_,
              frame_to_render->height_,
              static_cast<int>(frame_to_render->color_space_),
              static_cast<int>(frame_to_render->color_range_),
              is_full_range ? "full-range" : "limited-range");
    UploadSoftwareFrameTextures(*frame_to_render);

    // 6. 绑定软件 Shader，并传入本帧使用的 YUV → RGB 转换矩阵与色彩范围参数
    //    软件帧一律是 8-bit YUV420P（硬件帧下载回退时也已转换成 YUV420P），所以位深固定为 8
    const YuvRangeParameters range_parameters = GetYuvRangeParameters(frame_to_render->color_range_, 8);
    shader_program_->bind();
    shader_program_->setUniformValue("u_colorMatrix", ToColumnMajorGlslMatrix(*matrix));
    shader_program_->setUniformValue("u_yuvOffset", range_parameters.offset_);
    shader_program_->setUniformValue("u_yuvScale", range_parameters.scale_);

    // 7. 将 Y/U/V 三张纹理分别绑定到 Shader 使用的纹理单元 0、1、2
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[1]);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[2]);

    // 8. 绑定全屏四边形几何数据，执行绘制命令
    vao_.bind();
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    // 9. 解绑 VAO 和 Shader，避免影响后续 Qt 或其他 OpenGL 绘制操作
    vao_.release();
    // 必须用 -> 调用 QOpenGLShaderProgram::release()（解除 GL 绑定）
    // shader_program_ 是 unique_ptr：写成 shader_program_.release() 会调用
    // unique_ptr::release()，那会把裸指针丢出去并把成员置空——着色器既不会被
    // delete，下一帧也会因判空直接跳过，表现为"只显示第一帧"
    shader_program_->release();
}

/**
 * @brief 创建并链接软件帧使用的 YUV420P 三平面 Shader
 * @return 顶点 Shader、片段 Shader 均编译成功且程序链接成功时返回 true
 */
bool OpenGLView::BuildSoftwareFrameShader() {
    // 1. 创建 Qt Shader Program 对象
    shader_program_ = std::make_unique<QOpenGLShaderProgram>();

    // 2. 编译顶点 Shader，负责输出全屏四边形的位置和纹理坐标
    if (!shader_program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSource)) {
        LOG_ERROR("Vertex shader compilation failed: {}", shader_program_->log().toStdString());
        shader_program_.reset();
        return false;
    }

    // 3. 编译片段 Shader，负责采样 Y/U/V 三张纹理并转换成 RGB
    if (!shader_program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShaderSource)) {
        LOG_ERROR("Fragment shader compilation failed: {}", shader_program_->log().toStdString());
        shader_program_.reset();
        return false;
    }

    // 4. 链接顶点和片段 Shader，形成可以执行绘制的 Shader Program
    if (!shader_program_->link()) {
        LOG_ERROR("Shader program link failed: {}", shader_program_->log().toStdString());
        shader_program_.reset();
        return false;
    }

    // 5. 绑定程序，并将三个采样器分别固定到纹理单元 0、1、2
    shader_program_->bind();
    shader_program_->setUniformValue("texY", 0);
    shader_program_->setUniformValue("texU", 1);
    shader_program_->setUniformValue("texV", 2);
    shader_program_->release();

    LOG_INFO("Shader program compiled and linked");
    return true;
}

/**
 * @brief 创建全屏四边形使用的 VAO、VBO 和 EBO
 * @note 每个顶点由 2 个位置分量和 2 个纹理坐标分量组成
 */
void OpenGLView::CreateFullscreenQuadGeometry() {
    // 1. 创建并绑定 VAO，用于保存顶点属性配置。
    vao_.create();
    vao_.bind();

    // 2. 创建 VBO，并上传四个顶点的位置和纹理坐标。
    vbo_.create();
    vbo_.bind();
    vbo_.allocate(kVertices.data(), static_cast<int>(sizeof(float) * kVertices.size()));

    // 3. 创建 EBO，并上传两个三角形使用的顶点索引。
    ebo_.create();
    ebo_.bind();
    ebo_.allocate(kIndices.data(), static_cast<int>(sizeof(unsigned int) * kIndices.size()));

    // 4. 配置顶点数据格式：每个顶点 4 个 float，前两个是位置，后两个是纹理坐标。
    const int stride = 4 * static_cast<int>(sizeof(float));

    // 属性槽 0：顶点位置（x、y）。
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // 属性槽 1：纹理坐标（u、v），起始偏移为两个 float。
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(2 * sizeof(float)));  // NOLINT
    glEnableVertexAttribArray(1);

    vao_.release();
    LOG_DEBUG("OpenGL geometry prepared");
}

/**
 * @brief 创建软件帧使用的 Y/U/V 三张二维纹理对象
 * @note 本函数只创建纹理对象并设置采样参数，不分配视频像素存储
 */
void OpenGLView::CreateSoftwareFrameTextures() {
    // 1. 生成三张纹理对象的 OpenGL 名称，分别用于 Y、U、V 平面。
    glGenTextures(static_cast<GLsizei>(texture_ids_.size()), texture_ids_.data());
    LOG_DEBUG("OpenGL texture handles generated: {}, {}, {}", texture_ids_[0], texture_ids_[1], texture_ids_[2]);

    // 2. 逐张绑定纹理，并设置缩放过滤和边界寻址方式
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texture_ids_[i]);

        // 纹理缩放双线性平滑插值过滤
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // 纹理坐标截断模式，防止边界渗色
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    LOG_INFO("OpenGL textures prepared");
}

/**
 * @brief 将软件帧的 Y/U/V 三个平面像素数据上传到 OpenGL Texture
 * @param frame 待上传的软件视频帧
 * @note 同尺寸帧复用纹理存储，但每帧仍然需要上传新的 CPU 像素数据
 */
void OpenGLView::UploadSoftwareFrameTextures(const mediacore::VideoFrame& frame) {
    // 1. 读取当前视频帧尺寸，并判断纹理存储是否需要重新分配。
    const int frame_width = frame.width_;
    const int frame_height = frame.height_;

    // 检查分辨率是否发生变化
    const bool need_realloc =
        !texture_allocated_ || frame_width != last_frame_width_ || frame_height != last_frame_height_;

    if (need_realloc) {
        LOG_INFO("Reallocating video textures for frame size {}x{}", frame_width, frame_height);
    }

    // 2. 设置像素解包对齐为 1 字节，避免 OpenGL 默认 4 字节对齐导致行起始位置计算错误
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 3. 计算三个平面的尺寸。YUV420P 的 U/V 宽高分别为 Y 平面的向上取整一半
    const int uv_width = (frame_width + 1) / 2;
    const int uv_height = (frame_height + 1) / 2;

    const std::array<int, 3> plane_widths = {frame_width, uv_width, uv_width};
    const std::array<int, 3> plane_heights = {frame_height, uv_height, uv_height};

    // 4. 依次上传 Y、U、V 三个平面。
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texture_ids_[i]);

        // 告知 OpenGL CPU 内存中每行的实际步幅（包含 Padding 字节数）
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize_[i]);

        if (need_realloc) {
            // 分辨率变化：重新分配该平面的纹理存储，并同时上传当前像素数据。
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_R8,
                         plane_widths[i],
                         plane_heights[i],
                         0,
                         GL_RED,
                         GL_UNSIGNED_BYTE,
                         frame.data_[i]);
        } else {
            // 分辨率未变：复用已有纹理存储，只更新像素内容；这里仍然会发生 CPU 到 GPU 的数据上传
            glTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, 0, plane_widths[i], plane_heights[i], GL_RED, GL_UNSIGNED_BYTE, frame.data_[i]);
        }
    }

    // 5. 恢复默认行长状态，避免影响后续其他纹理上传
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // 6. 更新纹理存储分配状态与分辨率快照。后续同尺寸帧仍需上传新像素，只是不再重新分配存储
    if (need_realloc) {
        texture_allocated_ = true;
        last_frame_width_ = frame_width;
        last_frame_height_ = frame_height;
    }
}

// =========== 零拷贝硬件渲染路径 ===========
//
// 【整体思路】
//
// 软件路径：CPU 内存中的 YUV 像素 → glTexImage2D/glTexSubImage2D 上传 → Texture 采样
// 硬件路径：VA-API 图像存储 → DMA-BUF fd → EGLImage → GL_TEXTURE_2D → Shader 采样
//
// 硬件路径的目标是避免整帧像素先从 GPU 下载到 CPU，再从 CPU 上传回 GPU
// DMA-BUF fd、EGLImage 和 Texture 是不同层次的句柄/对象，不代表创建了三份像素副本
// 是否真正共享原有图像存储，仍取决于驱动对当前格式和 Modifier 的支持
//
// EGLImage 只借用 DMA-BUF fd，不接管 fd 所有权。每个平面按自身 FourCC、
// modifier、offset 和 pitch 创建一个 EGLImage，再绑定到独立 OpenGL Texture
// 绘制提交后插入 GL fence；Fence 完成前，待释放记录继续持有 EGLImage、纹理和 VideoFrame

/**
 * @brief 延迟释放记录持有的 EGLImage、Texture 和 VideoFrame 引用
 *
 * 必须先删除纹理并销毁 EGLImage，再释放 VideoFrame；
 * 这样 GPU 完成读取后，VA Surface 才可能回到解码器的 Surface 池。
 */
// PendingFrameResources 仅保存资源句柄；所有 EGL/GL 销毁必须由
// ReleasePendingFrameResources 在有效 Context 中显式完成。

/**
 * @brief 检查当前 Qt OpenGL 环境并初始化 EGL DMA-BUF 导入能力。
 * @return 当前 Context、所需扩展、函数入口和格式查询均可用时返回 true。
 * @note 必须在 OpenGLView 自己的 QOpenGLWidget Context 为 current 时调用。
 */
bool OpenGLView::InitializeEglDmaBufImport() {
    // 1. 清空旧的 Display、函数入口、格式列表和查询缓存，避免沿用上次 Context 的能力状态。
    egl_import_ = {};

    // 2. 获取当前线程正在使用的 Qt OpenGL Context。
    QOpenGLContext* current_context = QOpenGLContext::currentContext();
    if (current_context == nullptr || current_context != context() || retirement_context_ == nullptr ||
        retirement_surface_ == nullptr) {
        LOG_WARN("DMA-BUF EGL import requires this QOpenGLWidget context to be current");
        return false;
    }

#if QT_CONFIG(egl)
    // 3. 从 Qt Context 获取原生 EGL 接口，并确认 Qt 与 EGL 报告的是同一个 current Context / Display。
    auto* native_egl_context = current_context->nativeInterface<QNativeInterface::QEGLContext>();
    if (native_egl_context == nullptr || native_egl_context->nativeContext() == EGL_NO_CONTEXT ||
        native_egl_context->display() == EGL_NO_DISPLAY ||
        native_egl_context->nativeContext() != eglGetCurrentContext() ||
        native_egl_context->display() != eglGetCurrentDisplay()) {
        LOG_WARN("Qt context is not the current EGL context/display");
        return false;
    }
    egl_import_.display_ = native_egl_context->display();
#else
    LOG_WARN("This Qt build has no EGL native context interface");
    return false;
#endif

    // 4. 确认 EGLDisplay 已初始化，能够查询 EGL 版本。
    if (eglQueryString(egl_import_.display_, EGL_VERSION) == nullptr) {
        LOG_WARN("Qt returned an EGLDisplay that is not initialized");
        egl_import_ = {};
        return false;
    }
    // 5. 检查 DMA-BUF 导入和 Modifier 查询两个必需扩展是否都由当前 EGLDisplay 声明支持。
    const char* display_extensions = eglQueryString(egl_import_.display_, EGL_EXTENSIONS);
    if (!HasEglExtension(display_extensions, "EGL_EXT_image_dma_buf_import") ||
        !HasEglExtension(display_extensions, "EGL_EXT_image_dma_buf_import_modifiers")) {
        LOG_INFO("EGL DMA-BUF import and modifier extensions are both required");
        egl_import_ = {};
        return false;
    }

    // 6. 获取 EGL 扩展函数入口。获取到函数地址后，还需依靠上面的扩展检查确认功能可用。
    egl_import_.create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    egl_import_.destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    egl_import_.query_formats_ =
        reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
    egl_import_.query_modifiers_ =
        reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    egl_import_.image_target_texture_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        current_context->getProcAddress("glEGLImageTargetTexture2DOES"));

    // 7. 检查所有必需函数入口，以及 GL_OES_EGL_image 扩展是否可用。
    if (egl_import_.create_image_ == nullptr || egl_import_.destroy_image_ == nullptr ||
        egl_import_.query_formats_ == nullptr || egl_import_.query_modifiers_ == nullptr ||
        egl_import_.image_target_texture_ == nullptr || !current_context->hasExtension("GL_OES_EGL_image")) {
        LOG_WARN("Required EGLImage, modifier query, or GL_OES_EGL_image entry point is unavailable");
        egl_import_ = {};
        return false;
    }

    // 8. 先只查询可导入 DRM FourCC 的数量，为后续分配数组做准备。
    EGLint format_count = 0;
    if (egl_import_.query_formats_(egl_import_.display_, 0, nullptr, &format_count) != EGL_TRUE || format_count <= 0) {
        LOG_WARN("eglQueryDmaBufFormatsEXT returned no importable formats");
        egl_import_ = {};
        return false;
    }
    // 9. 根据驱动返回的数量分配格式列表；分配失败则关闭本次 EGL 导入能力。
    try {
        egl_import_.importable_formats_.resize(static_cast<std::size_t>(format_count));
    } catch (const std::bad_alloc&) {
        egl_import_ = {};
        return false;
    }
    // 10. 读取驱动实际支持的 FourCC 列表，并校验驱动返回的数量没有超出分配容量。
    if (egl_import_.query_formats_(
            egl_import_.display_, format_count, egl_import_.importable_formats_.data(), &format_count) != EGL_TRUE ||
        format_count < 0 || static_cast<std::size_t>(format_count) > egl_import_.importable_formats_.size()) {
        LOG_WARN("eglQueryDmaBufFormatsEXT failed while reading supported formats");
        egl_import_ = {};
        return false;
    }
    // 11. 按实际返回数量收缩列表，并标记 EGL DMA-BUF 导入基础能力初始化完成。
    egl_import_.importable_formats_.resize(static_cast<std::size_t>(format_count));
    egl_import_.is_available_ = true;
    LOG_INFO("Qt EGL context and EGL DMA-BUF modifier import functions are available");
    return true;
}

/**
 * @brief 查询某个 Plane 的 DRM FourCC 与 Modifier 组合是否可用于普通二维纹理。
 * @param drm_format 该 Plane 的 DRM FourCC。
 * @param drm_modifier 该 Plane 所属 DMA-BUF Object 的 Modifier。
 * @return 驱动声明支持该组合，且不是 external-only 时返回 true。
 * @note 查询结果会缓存；本函数需要在 EGL 导入能力初始化成功后调用。
 */
bool OpenGLView::QueryDmaBufPlaneImportSupport(std::uint32_t drm_format, std::uint64_t drm_modifier) {
    // 1. 先查询缓存，避免重复向 EGL 驱动请求相同格式和 Modifier 的支持情况。
    const auto cache_key = std::make_pair(drm_format, drm_modifier);
    const auto cached_result = egl_import_.format_modifier_support_.find(cache_key);
    if (cached_result != egl_import_.format_modifier_support_.end()) {
        return cached_result->second;
    }

    // 2. 将 DRM FourCC 的 32 位内容编码成 EGL 属性列表使用的 EGLint。
    const EGLint egl_drm_format = EncodeUint32AsEglInt(drm_format);
    const bool format_is_importable =
        std::find(egl_import_.importable_formats_.begin(), egl_import_.importable_formats_.end(), egl_drm_format) !=
        egl_import_.importable_formats_.end();
    if (!format_is_importable) {
        egl_import_.format_modifier_support_.emplace(cache_key, false);
        return false;
    }

    // 3. FourCC 可导入后，先查询该格式支持的 Modifier 数量。
    EGLint modifier_count = 0;
    if (egl_import_.query_modifiers_(egl_import_.display_, egl_drm_format, 0, nullptr, nullptr, &modifier_count) !=
            EGL_TRUE ||
        modifier_count < 0) {
        egl_import_.format_modifier_support_.emplace(cache_key, false);
        return false;
    }

    // 4. 为 Modifier 列表和 external_only 标志分配存储。
    //    两个数组按相同下标对应同一个 Modifier。
    std::vector<EGLuint64KHR> supported_modifiers;
    std::vector<EGLBoolean> external_only;
    try {
        supported_modifiers.resize(static_cast<std::size_t>(modifier_count));
        external_only.resize(static_cast<std::size_t>(modifier_count));
    } catch (const std::bad_alloc&) {
        egl_import_.format_modifier_support_.emplace(cache_key, false);
        return false;
    }
    // 5. 读取完整 Modifier 列表，并检查驱动返回数量没有越界。
    if (egl_import_.query_modifiers_(egl_import_.display_,
                                     egl_drm_format,
                                     modifier_count,
                                     supported_modifiers.data(),
                                     external_only.data(),
                                     &modifier_count) != EGL_TRUE ||
        modifier_count < 0 || static_cast<std::size_t>(modifier_count) > supported_modifiers.size()) {
        egl_import_.format_modifier_support_.emplace(cache_key, false);
        return false;
    }

    // 6. 查找目标 Modifier，并要求 external_only 为 EGL_FALSE，
    //    因为当前渲染代码使用 GL_TEXTURE_2D，而不是 GL_TEXTURE_EXTERNAL_OES。
    bool is_supported = false;
    for (EGLint modifier_index = 0; modifier_index < modifier_count; ++modifier_index) {
        const auto index = static_cast<std::size_t>(modifier_index);
        if (supported_modifiers[index] == drm_modifier && external_only[index] == EGL_FALSE) {
            is_supported = true;
            break;
        }
    }
    // 7. 缓存查询结果，供后续帧直接使用。
    egl_import_.format_modifier_support_.emplace(cache_key, is_supported);
    return is_supported;
}

/**
 * @brief 编译并链接硬件帧使用的双平面 Shader。
 * @return 编译和链接成功返回 true，否则返回 false。
 * @details Shader 分别采样 Y 纹理和交织 UV 纹理，并将 YUV 转换为 RGB。
 */
bool OpenGLView::BuildHardwareFrameShader() {
    // 1. 创建硬件帧 Shader Program。
    hardware_shader_program_ = std::make_unique<QOpenGLShaderProgram>();

    // 2. 编译全屏四边形使用的顶点 Shader。
    if (!hardware_shader_program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSource)) {
        LOG_ERROR("HW vertex shader compilation failed: {}", hardware_shader_program_->log().toStdString());
        hardware_shader_program_.reset();
        return false;
    }

    // 3. 编译硬件帧片段 Shader，分别采样 Y 和交织 UV 两个平面。
    if (!hardware_shader_program_->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                           kHardwareFrameFragmentShaderSource)) {
        LOG_ERROR("HW fragment shader compilation failed: {}", hardware_shader_program_->log().toStdString());
        hardware_shader_program_.reset();
        return false;
    }

    // 4. 链接两个 Shader，生成硬件渲染使用的程序。
    if (!hardware_shader_program_->link()) {
        LOG_ERROR("HW shader program link failed: {}", hardware_shader_program_->log().toStdString());
        hardware_shader_program_.reset();
        return false;
    }

    // 5. 将 Y 和 UV 两个采样器分别绑定到纹理单元 0 和 1。
    hardware_shader_program_->bind();
    hardware_shader_program_->setUniformValue("texY", 0);
    hardware_shader_program_->setUniformValue("texUV", 1);
    hardware_shader_program_->release();

    LOG_INFO("Hardware path shader compiled and linked");
    return true;
}

/**
 * @brief 尝试通过 DMA-BUF、EGLImage 和 OpenGL Texture 渲染一帧硬件视频。
 * @param video_frame 持有 HardwareFrame 的视频帧。
 * @param color_matrix 本帧使用的 YUV → RGB 3×3 转换矩阵，按列优先排列。
 * @return 绘制成功且资源已登记到待回收列表时返回 true；导入或绘制失败返回 false。
 * @note 必须在有效的 OpenGL Context 为 current 时调用。
 * @note 本函数不主动下载像素到 CPU；它尝试让 OpenGL 使用 DMA-BUF 对应的图像存储。
 */
bool OpenGLView::RenderHardwareFrameWithEglImages(const std::shared_ptr<mediacore::VideoFrame>& video_frame,
                                                  const std::array<float, 9>& color_matrix) {
    // 1. 检查输入帧、硬件帧载体以及 EGL DMA-BUF 导入能力是否有效。
    if (video_frame == nullptr || video_frame->hardware_ == nullptr || !egl_import_.is_available_ ||
        egl_import_.display_ == EGL_NO_DISPLAY) {
        return false;
    }

    // 2. 取得 HardwareFrame，并确认格式仅为当前 Shader 支持的 NV12 或 P010。
    const mediacore::HardwareFrame& hardware_frame = *video_frame->hardware_;
    const bool is_p010 = hardware_frame.layer_format_ == mediacore::kDrmFormatP010;
    if ((!is_p010 && hardware_frame.layer_format_ != mediacore::kDrmFormatNv12) || video_frame->width_ <= 0 ||
        video_frame->height_ <= 0 || hardware_frame.width_ <= 0 || hardware_frame.height_ <= 0 ||
        video_frame->width_ > hardware_frame.width_ || video_frame->height_ > hardware_frame.height_ ||
        hardware_frame.av_frame_ref_ == nullptr || hardware_frame.planes_.size() != 2 ||
        !hardware_frame.ValidateDmaBufLayout()) {
        return false;
    }

    // 3. 分别计算底层存储尺寸和实际可见尺寸。
    //    EGLImage 按底层 Plane 存储尺寸导入，Shader 通过缩放/偏移只采样可见区域。
    // 可见尺寸由 VideoFrame 保存，DMA-BUF 的 EGLImage 尺寸由硬件存储尺寸决定。
    // EGLImage 的 width/height 是平面像素尺寸；行尾对齐仍通过 pitch_bytes_ 描述。
    const std::uint32_t storage_width = static_cast<std::uint32_t>(hardware_frame.width_);
    const std::uint32_t storage_height = static_cast<std::uint32_t>(hardware_frame.height_);
    const std::uint32_t visible_width = static_cast<std::uint32_t>(video_frame->width_);
    const std::uint32_t visible_height = static_cast<std::uint32_t>(video_frame->height_);
    const std::array<std::uint32_t, 2> plane_storage_widths = {storage_width, storage_width / 2U + storage_width % 2U};
    const std::array<std::uint32_t, 2> plane_storage_heights = {storage_height,
                                                                storage_height / 2U + storage_height % 2U};
    const std::array<std::uint32_t, 2> plane_visible_widths = {visible_width, visible_width / 2U + visible_width % 2U};
    const std::array<std::uint32_t, 2> plane_visible_heights = {visible_height,
                                                                visible_height / 2U + visible_height % 2U};
    const std::array<std::uint32_t, 2> bytes_per_texel = {is_p010 ? 2U : 1U, is_p010 ? 4U : 2U};
    std::array<GLsizei, 2> texture_width{};
    std::array<GLsizei, 2> texture_height{};

    // 4. 逐个检查 Y、UV Plane 的 FourCC、Object 索引、pitch、尺寸和 EGL 参数范围。
    for (std::size_t plane_index = 0; plane_index < hardware_frame.planes_.size(); ++plane_index) {
        const mediacore::DmaBufPlane& dma_buf_plane = hardware_frame.planes_[plane_index];
        const std::uint32_t expected_plane_format =
            mediacore::GetDrmPlaneFourcc(hardware_frame.layer_format_, plane_index);
        if (expected_plane_format == 0 || dma_buf_plane.drm_format_ != expected_plane_format ||
            dma_buf_plane.dma_buf_index_ >= hardware_frame.dma_buf_objects_.size() || dma_buf_plane.pitch_bytes_ == 0 ||
            dma_buf_plane.pitch_bytes_ % bytes_per_texel[plane_index] != 0 ||
            dma_buf_plane.pitch_bytes_ / bytes_per_texel[plane_index] < plane_storage_widths[plane_index] ||
            plane_storage_widths[plane_index] < plane_visible_widths[plane_index] ||
            plane_storage_heights[plane_index] < plane_visible_heights[plane_index] ||
            plane_storage_widths[plane_index] > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max()) ||
            plane_storage_heights[plane_index] > static_cast<std::uint32_t>(std::numeric_limits<GLsizei>::max()) ||
            plane_storage_widths[plane_index] > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
            plane_storage_heights[plane_index] > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
            dma_buf_plane.offset_bytes_ > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max()) ||
            dma_buf_plane.pitch_bytes_ > static_cast<std::uint32_t>(std::numeric_limits<EGLint>::max())) {
            return false;
        }

        // 5. 找到该 Plane 所属的 DMA-BUF Object，并查询 EGL 是否支持它的 FourCC/Modifier。
        const mediacore::DmaBufObject& dma_buf_object = hardware_frame.dma_buf_objects_[dma_buf_plane.dma_buf_index_];
        bool plane_import_is_supported = false;
        try {
            plane_import_is_supported =
                QueryDmaBufPlaneImportSupport(dma_buf_plane.drm_format_, dma_buf_object.format_modifier_);
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (dma_buf_object.fd_ < 0 || !plane_import_is_supported) {
            LOG_INFO("EGL does not advertise DRM format 0x{:x} with modifier 0x{:x}",
                     dma_buf_plane.drm_format_,
                     dma_buf_object.format_modifier_);
            return false;
        }
        texture_width[plane_index] = static_cast<GLsizei>(plane_storage_widths[plane_index]);
        texture_height[plane_index] = static_cast<GLsizei>(plane_storage_heights[plane_index]);
    }

    // 6. 创建本帧的资源回收记录，并立即保留 VideoFrame。
    //    后面即使导入或绘制失败，也必须先确认 GPU 不再访问图像，才能释放这份引用。
    std::unique_ptr<PendingFrameResources> pending_resources;
    try {
        pending_resources = std::make_unique<PendingFrameResources>();
    } catch (const std::bad_alloc&) {
        return false;
    }
    // EGLImage 导入不会接管 fd 的关闭责任。
    // 在 GPU Fence 完成之前保留整个 VideoFrame，确保 DMA-BUF fd 和 AVFrame 引用都仍然有效。
    pending_resources->video_frame_ = video_frame;
    const auto rollback = [this, &pending_resources](bool wait_for_gpu) {
        if (pending_resources != nullptr && !wait_for_gpu) {
            // Import/setup failures happen before glDrawElements; EGL/GL object
            // setup alone has not submitted a GPU read of the DMA-BUF planes.
            pending_resources->gpu_use_is_complete_ = IsGpuResourceReleaseSafe(false, false);
        }
        if (pending_resources != nullptr && !ReleasePendingFrameResources(*pending_resources, wait_for_gpu)) {
            hardware_retirement_suspended_ = true;
            if (hardware_render_capability_ != nullptr) {
                hardware_render_capability_->store(false, std::memory_order_release);
            }
            try {
                pending_frame_resources_.push_back(std::move(pending_resources));
            } catch (const std::bad_alloc&) {
                LOG_ERROR("Could not retain a frame after an unconfirmed GL finish; keeping it until process exit");
                GetDeferredGlResourceReaper().DisableHardwarePermanently();
                static_cast<void>(pending_resources.release());
            }
        }
    };

    // 7. 清除进入本次导入前遗留的 GL 错误，避免把之前操作的错误误判为本次导入失败。
    while (glGetError() != GL_NO_ERROR) {
    }

    // 8. 分别为 Y、UV Plane 创建 EGLImage，并关联到各自的 OpenGL Texture。
    for (std::size_t plane_index = 0; plane_index < hardware_frame.planes_.size(); ++plane_index) {
        const mediacore::DmaBufPlane& dma_buf_plane = hardware_frame.planes_[plane_index];
        const mediacore::DmaBufObject& dma_buf_object = hardware_frame.dma_buf_objects_[dma_buf_plane.dma_buf_index_];
        const std::uint32_t modifier_low = static_cast<std::uint32_t>(dma_buf_object.format_modifier_ & 0xffffffffULL);
        const std::uint32_t modifier_high = static_cast<std::uint32_t>(dma_buf_object.format_modifier_ >> 32U);
        // 8.1 组装当前 Plane 的 DMA-BUF 导入属性：尺寸、FourCC、fd、offset、pitch 和 Modifier。
        //     这里每次只导入一个 Plane，所以使用 PLANE0 属性键；该 Plane 可以引用共享 Object fd。
        const EGLint image_attributes[] = {EGL_WIDTH,
                                           static_cast<EGLint>(plane_storage_widths[plane_index]),
                                           EGL_HEIGHT,
                                           static_cast<EGLint>(plane_storage_heights[plane_index]),
                                           EGL_LINUX_DRM_FOURCC_EXT,
                                           EncodeUint32AsEglInt(dma_buf_plane.drm_format_),
                                           EGL_DMA_BUF_PLANE0_FD_EXT,
                                           dma_buf_object.fd_,
                                           EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                           static_cast<EGLint>(dma_buf_plane.offset_bytes_),
                                           EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                           static_cast<EGLint>(dma_buf_plane.pitch_bytes_),
                                           EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                                           EncodeUint32AsEglInt(modifier_low),
                                           EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                                           EncodeUint32AsEglInt(modifier_high),
                                           EGL_NONE};

        // 8.2 将当前 Plane 对应的 DMA-BUF 导入 EGL，创建 EGLImage。
        //     EGLImage 使用该 fd 对应的存储，但 fd 的关闭责任仍由 HardwareFrame 持有。
        pending_resources->egl_images_[plane_index] = egl_import_.create_image_(
            egl_import_.display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, image_attributes);
        if (pending_resources->egl_images_[plane_index] == EGL_NO_IMAGE_KHR) {
            const EGLint egl_error = eglGetError();
            LOG_WARN("eglCreateImageKHR failed for plane {}: EGL error 0x{:x}", plane_index, egl_error);
            rollback(false);
            return false;
        }

        // 8.3 创建一个 OpenGL Texture 对象，用于后续绑定当前 Plane 的 EGLImage
        glGenTextures(1, &pending_resources->textures_[plane_index]);
        if (pending_resources->textures_[plane_index] == 0 || glGetError() != GL_NO_ERROR) {
            LOG_WARN("OpenGL texture allocation failed for DMA-BUF plane {}", plane_index);
            rollback(false);
            return false;
        }
        // 8.4 绑定 Texture 并设置采样过滤、边界处理参数。
        glBindTexture(GL_TEXTURE_2D, pending_resources->textures_[plane_index]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // 8.5 将 EGLImage 关联到当前绑定的 GL_TEXTURE_2D
        //     这一步建立 OpenGL Texture 对 EGLImage 图像存储的访问关系，不重新上传像素。
        egl_import_.image_target_texture_(GL_TEXTURE_2D,
                                          reinterpret_cast<GLeglImageOES>(pending_resources->egl_images_[plane_index]));
        const GLenum import_error = glGetError();
        if (import_error != GL_NO_ERROR) {
            LOG_WARN("glEGLImageTargetTexture2DOES failed for plane {}: GL error 0x{:x}", plane_index, import_error);
            rollback(false);
            return false;
        }

        // EGLImage 导入成功只说明驱动接受了 DMA-BUF 描述；还要检查
        // 纹理实际内部格式是否与 Shader 的归一化采样假设一致。
        // NV12 使用 R8/RG8，P010 使用 R16/RG16。若驱动映射成其他
        // 格式，继续绘制会产生错误的通道或位深解释，因此回退软件路径。
        GLint imported_internal_format = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &imported_internal_format);
        const GLenum expected_internal_format =
            is_p010 ? (plane_index == 0 ? GL_R16 : GL_RG16) : (plane_index == 0 ? GL_R8 : GL_RG8);
        const GLenum format_query_error = glGetError();
        if (format_query_error != GL_NO_ERROR ||
            imported_internal_format != static_cast<GLint>(expected_internal_format)) {
            LOG_WARN("Imported DMA-BUF plane {} has unexpected texture format 0x{:x}, expected 0x{:x}",
                     plane_index,
                     static_cast<unsigned int>(imported_internal_format),
                     static_cast<unsigned int>(expected_internal_format));
            rollback(false);
            return false;
        }
    }

    // 9. 配置硬件 Shader：色彩矩阵、NV12/P010 位深归一化系数、色彩范围参数和可见区域采样坐标。
    const GLfloat bit_depth_scale = is_p010 ? static_cast<GLfloat>(65535.0 / (64.0 * 1023.0)) : 1.0F;
    const YuvRangeParameters range_parameters = GetYuvRangeParameters(video_frame->color_range_, is_p010 ? 10 : 8);
    hardware_shader_program_->bind();
    hardware_shader_program_->setUniformValue("u_colorMatrix", ToColumnMajorGlslMatrix(color_matrix));
    hardware_shader_program_->setUniformValue("u_bitDepthScale", bit_depth_scale);
    hardware_shader_program_->setUniformValue("u_yuvOffset", range_parameters.offset_);
    hardware_shader_program_->setUniformValue("u_yuvScale", range_parameters.scale_);

    // 9.1 将归一化纹理坐标映射到可见 texel 的中心，避免采样到行尾或底部的存储对齐区域。
    const auto coordinate_transform = [](std::uint32_t visible_size, GLsizei storage_size) {
        const GLfloat inverse_storage_size = 1.0F / static_cast<GLfloat>(storage_size);
        return std::array<GLfloat, 2>{static_cast<GLfloat>(visible_size - 1U) * inverse_storage_size,
                                      0.5F * inverse_storage_size};
    };
    const auto luma_x_transform = coordinate_transform(plane_visible_widths[0], texture_width[0]);
    const auto luma_y_transform = coordinate_transform(plane_visible_heights[0], texture_height[0]);
    const auto chroma_x_transform = coordinate_transform(plane_visible_widths[1], texture_width[1]);
    const auto chroma_y_transform = coordinate_transform(plane_visible_heights[1], texture_height[1]);
    hardware_shader_program_->setUniformValue("u_yScale", QVector2D(luma_x_transform[0], luma_y_transform[0]));
    hardware_shader_program_->setUniformValue("u_yBias", QVector2D(luma_x_transform[1], luma_y_transform[1]));
    hardware_shader_program_->setUniformValue("u_uvScale", QVector2D(chroma_x_transform[0], chroma_y_transform[0]));
    hardware_shader_program_->setUniformValue("u_uvBias", QVector2D(chroma_x_transform[1], chroma_y_transform[1]));

    // 10. 将 Y 和 UV 两张 Texture 分别绑定到 Shader 的纹理单元 0 和 1。
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pending_resources->textures_[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, pending_resources->textures_[1]);
    // 11. 绑定全屏四边形几何数据，执行硬件帧绘制，然后恢复 VAO 和 Shader 绑定状态。
    vao_.bind();
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
    vao_.release();
    hardware_shader_program_->release();

    const GLenum draw_error = glGetError();
    if (draw_error != GL_NO_ERROR) {
        LOG_WARN("Hardware frame draw failed: GL error 0x{:x}", draw_error);
        rollback(true);
        return false;
    }

    // 12. 绘制命令已提交，但 GPU 可能仍在读取 EGLImage；在命令之后插入 Fence 作为完成标记。
    pending_resources->fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (pending_resources->fence_ == nullptr) {
        LOG_WARN("glFenceSync failed; waiting for the GPU before releasing the frame");
        rollback(true);
        return false;
    }
    // 13. 将 Fence、Texture、EGLImage 和 VideoFrame 一起放入待回收列表。
    //     后续只有 Fence 确认完成，ReclaimCompletedFrameResources() 才会释放这些资源。
    try {
        pending_frame_resources_.push_back(std::move(pending_resources));
    } catch (const std::bad_alloc&) {
        if (pending_resources != nullptr) {
            rollback(true);
        } else {
            glFinish();
        }
        return false;
    }
    return true;
}

/**
 * @brief 将硬件帧下载到 CPU，并转换成软件 YUV420P 帧。
 * @param hardware_video_frame 输入的硬件帧，必须持有有效 AVFrame 引用。
 * @param software_video_frame 输出的软件帧，成功时包含转换后的像素和原帧时间/色彩信息。
 * @return 下载、格式转换和输出帧校验均成功时返回 true。
 * @note 这是 DMA-BUF 导入或硬件绘制失败时的回退路径，会产生设备到 CPU 的图像传输。
 */
bool OpenGLView::DownloadHardwareFrameToSoftwareFrame(const mediacore::VideoFrame& hardware_video_frame,
                                                      mediacore::VideoFrame& software_video_frame) {
    // 1. 检查输入是否包含可下载的硬件帧和有效尺寸。
    if (hardware_video_frame.hardware_ == nullptr || hardware_video_frame.hardware_->av_frame_ref_ == nullptr ||
        hardware_video_frame.width_ <= 0 || hardware_video_frame.height_ <= 0) {
        return false;
    }

    // 2. 分配临时 AVFrame，用于接收从硬件帧传输到 CPU 的图像数据。
    AVFrame* downloaded = av_frame_alloc();
    if (downloaded == nullptr) {
        return false;
    }
    // 3. 请求 FFmpeg 将硬件帧数据传输到普通软件帧。
    const auto transfer_started_at = std::chrono::steady_clock::now();
    const int transfer_result = av_hwframe_transfer_data(downloaded, hardware_video_frame.hardware_->av_frame_ref_, 0);
    RecordHardwareFrameDownload(std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - transfer_started_at)
                                    .count(),
                                transfer_result >= 0);
    if (transfer_result < 0 || downloaded->format < 0 || downloaded->width <= 0 || downloaded->height <= 0) {
        av_frame_free(&downloaded);
        return false;
    }

    // 4. 创建像素格式转换上下文，将下载得到的格式转换为项目软件路径使用的 YUV420P。
    SwsContext* scaler = sws_getContext(downloaded->width,
                                        downloaded->height,
                                        static_cast<AVPixelFormat>(downloaded->format),
                                        hardware_video_frame.width_,
                                        hardware_video_frame.height_,
                                        AV_PIX_FMT_YUV420P,
                                        SWS_BILINEAR,
                                        nullptr,
                                        nullptr,
                                        nullptr);
    if (scaler == nullptr) {
        av_frame_free(&downloaded);
        return false;
    }

    // 5. 分配输出 VideoFrame 的 CPU 像素存储。
    try {
        software_video_frame =
            mediacore::VideoFrame(mediacore::FrameSize{hardware_video_frame.width_, hardware_video_frame.height_});
    } catch (const std::bad_alloc&) {
        sws_freeContext(scaler);
        av_frame_free(&downloaded);
        return false;
    }

    // 6. 执行像素格式转换，并释放临时 SwsContext 和下载得到的 AVFrame。
    const int rows = sws_scale(scaler,
                               downloaded->data,
                               downloaded->linesize,
                               0,
                               downloaded->height,
                               software_video_frame.data_.data(),
                               software_video_frame.linesize_.data());
    sws_freeContext(scaler);
    av_frame_free(&downloaded);
    if (rows != hardware_video_frame.height_) {
        software_video_frame = mediacore::VideoFrame{};
        return false;
    }

    // 7. 转换成功后复制原帧的可见尺寸、时间戳、epoch 和色彩元数据。
    software_video_frame.width_ = hardware_video_frame.width_;
    software_video_frame.height_ = hardware_video_frame.height_;
    software_video_frame.pts_us_ = hardware_video_frame.pts_us_;
    software_video_frame.epoch_ = hardware_video_frame.epoch_;
    software_video_frame.color_space_ = hardware_video_frame.color_space_;
    software_video_frame.color_range_ = hardware_video_frame.color_range_;
    return software_video_frame.IsValid();
}

void OpenGLView::RecordHardwareFrameDownload(double elapsed_ms, bool succeeded) {
    ++renderer_download_count_;
    ++renderer_download_interval_count_;
    renderer_download_total_ms_ += elapsed_ms;
    renderer_download_interval_ms_ += elapsed_ms;
    renderer_download_max_ms_ = std::max(renderer_download_max_ms_, elapsed_ms);
    renderer_download_interval_max_ms_ = std::max(renderer_download_interval_max_ms_, elapsed_ms);
    if (!succeeded) {
        ++renderer_download_failures_;
        ++renderer_download_interval_failures_;
    }

    const auto now = std::chrono::steady_clock::now();
    const double interval_seconds =
        std::chrono::duration<double>(now - renderer_interval_started_at_).count();
    if (interval_seconds < 5.0) {
        return;
    }

    const double average_download_ms = renderer_download_interval_count_ == 0
        ? 0.0
        : renderer_download_interval_ms_ / static_cast<double>(renderer_download_interval_count_);
    LOG_INFO("RendererDownloadStats reason=interval scope=interval transfer_count={} failures={} "
             "avg_transfer_ms={:.3f} max_transfer_ms={:.3f} interval_s={:.3f} "
             "cumulative_transfers={} cumulative_failures={}",
             renderer_download_interval_count_,
             renderer_download_interval_failures_,
             average_download_ms,
             renderer_download_interval_max_ms_,
             interval_seconds,
             renderer_download_count_,
             renderer_download_failures_);
    renderer_download_interval_count_ = 0;
    renderer_download_interval_failures_ = 0;
    renderer_download_interval_ms_ = 0.0;
    renderer_download_interval_max_ms_ = 0.0;
    renderer_interval_started_at_ = now;
}

/**
 * @brief 检查待释放列表，并回收 GPU 已经完成读取的硬件帧资源。
 * @note 只释放 Renderer 持有的 GL/EGL 对象和 VideoFrame 引用，不直接销毁 VA Surface。
 */
void OpenGLView::ReclaimCompletedFrameResources() {
    // 1. 没有待回收记录时无需查询 Fence。
    if (pending_frame_resources_.empty()) {
        if (hardware_retirement_suspended_ && GetDeferredGlResourceReaper().CanRenderHardware() &&
            egl_import_.is_available_ && hardware_shader_ready_ && hardware_shader_program_ != nullptr &&
            vao_.isCreated() && vbo_.isCreated() && ebo_.isCreated()) {
            hardware_retirement_suspended_ = false;
            if (hardware_render_capability_ != nullptr) {
                hardware_render_capability_->store(true, std::memory_order_release);
            }
        }
        return;
    }

    // 2. 非阻塞检查每一帧的 Fence。只有明确完成时才释放关联资源。
    for (auto it = pending_frame_resources_.begin(); it != pending_frame_resources_.end();) {
        PendingFrameResources* pending_resources = it->get();

        if (pending_resources->fence_ != nullptr) {
            const GLenum status = glClientWaitSync(pending_resources->fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            if (status == GL_TIMEOUT_EXPIRED) {
                ++it;  // GPU 还没画完，下次再查
                continue;
            }
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
                hardware_retirement_suspended_ = true;
                if (hardware_render_capability_ != nullptr) {
                    hardware_render_capability_->store(false, std::memory_order_release);
                }
                LOG_ERROR("OpenGL Fence status is indeterminate; retaining the frame and suspending DMA-BUF rendering");
                ++it;
                continue;
            }
            pending_resources->gpu_use_is_complete_ = true;
        } else if (!pending_resources->gpu_use_is_complete_) {
            hardware_retirement_suspended_ = true;
            if (hardware_render_capability_ != nullptr) {
                hardware_render_capability_->store(false, std::memory_order_release);
            }
            LOG_ERROR("OpenGL frame has no completion Fence; retaining it and suspending DMA-BUF rendering");
            ++it;
            continue;
        }

        if (!ReleasePendingFrameResources(*pending_resources, false)) {
            hardware_retirement_suspended_ = true;
            if (hardware_render_capability_ != nullptr) {
                hardware_render_capability_->store(false, std::memory_order_release);
            }
            ++it;
            continue;
        }
        it = pending_frame_resources_.erase(it);
    }

    // 3. 如果超过软上限且最旧的 Fence 仍未确认，暂停后续硬件导出，避免队列继续增长。
    constexpr std::size_t kMaxPendingReleases = 8;
    while (pending_frame_resources_.size() > kMaxPendingReleases) {
        PendingFrameResources* oldest_resources = pending_frame_resources_.front().get();

        if (oldest_resources->fence_ != nullptr) {
            // 最多等 100 ms（GLuint64 纳秒）
            const GLenum status = glClientWaitSync(
                oldest_resources->fence_, GL_SYNC_FLUSH_COMMANDS_BIT, static_cast<GLuint64>(100) * 1000ULL * 1000ULL);
            if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
                oldest_resources->gpu_use_is_complete_ = true;
                if (ReleasePendingFrameResources(*oldest_resources, false)) {
                    pending_frame_resources_.erase(pending_frame_resources_.begin());
                    continue;
                }
            }
        }

        hardware_retirement_suspended_ = true;
        if (hardware_render_capability_ != nullptr) {
            hardware_render_capability_->store(false, std::memory_order_release);
        }
        LOG_WARN("Pending hardware frame limit exceeded; keeping unconfirmed resources and suspending DMA-BUF rendering");
        break;
    }
}

/**
 * @brief 释放一帧待回收记录持有的 Fence、Texture、EGLImage 和 VideoFrame 引用。
 * @param pending_resources 需要释放的资源记录。
 * @param wait_for_gpu true 表示先等待当前 Context 已提交的 OpenGL 命令；false 表示 GPU 已确认完成。
 * @note 必须在能够访问这些 GL/EGL 资源的 Context 中调用。
 * @note 最后一个 VideoFrame 引用释放时，HardwareFrame 才可能释放 DMA-BUF fd 和 AVFrame 引用。
 */
bool OpenGLView::ReleasePendingFrameResources(PendingFrameResources& pending_resources, bool wait_for_gpu) {
    // 1. 如果调用方不能确认 GPU 已完成，先等待当前 Context 已提交的命令执行完毕。
    if (wait_for_gpu) {
        glFinish();
        bool context_lost = false;
        while (const GLenum error = glGetError()) {
            if (error == GL_CONTEXT_LOST) {
                context_lost = true;
            }
        }
        if (context_lost || QOpenGLContext::currentContext() == nullptr ||
            !QOpenGLContext::currentContext()->isValid()) {
            return false;
        }
        pending_resources.gpu_use_is_complete_ = true;
    }
    if (!IsGpuResourceReleaseSafe(true, pending_resources.gpu_use_is_complete_)) {
        return false;
    }
    // 2. 删除 GPU 完成标记。调用方负责保证此时不再依赖该 Fence 进行等待。
    if (pending_resources.fence_ != nullptr) {
        glDeleteSync(pending_resources.fence_);
        pending_resources.fence_ = nullptr;
    }
    // 3. 删除本帧创建的 OpenGL Texture。
    glDeleteTextures(static_cast<GLsizei>(pending_resources.textures_.size()), pending_resources.textures_.data());
    pending_resources.textures_ = {0, 0};
    // 4. 使用创建 EGLImage 时对应的 EGLDisplay 销毁两个 Plane 的 EGLImage。
    for (EGLImageKHR& egl_image : pending_resources.egl_images_) {
        if (egl_image != EGL_NO_IMAGE_KHR) {
            if (egl_import_.destroy_image_ == nullptr || egl_import_.display_ == EGL_NO_DISPLAY ||
                egl_import_.destroy_image_(egl_import_.display_, egl_image) != EGL_TRUE) {
                return false;
            }
            egl_image = EGL_NO_IMAGE_KHR;
        }
    }
    // 5. 最后释放 VideoFrame 引用。若这是最后一份引用，HardwareFrame 才会关闭 fd 并释放 AVFrame 引用。
    pending_resources.video_frame_.reset();
    return true;
}

/**
 * @brief 清理 OpenGLView 持有的 OpenGL、EGL 和待回收帧资源。
 * @note 由 Context 销毁信号和析构函数调用，因此必须允许重复执行。
 * @note 如果没有可用 Context 且 GPU 完成状态无法确认，待回收资源必须隔离保留，不能提前释放。
 */
void OpenGLView::ReleaseOpenGlResources() {
    // 1. 先撤销硬件渲染能力，通知 Decoder 不再开始新的 DMA-BUF 导出。
    if (hardware_render_capability_ != nullptr) {
        hardware_render_capability_->store(false, std::memory_order_release);
    }

    // 2. 断开 Context 销毁信号，避免清理过程中再次进入本函数。
    if (context_destroyed_connection_) {
        disconnect(context_destroyed_connection_);
        context_destroyed_connection_ = {};
    }

    // 3. 优先尝试恢复 QOpenGLWidget 自己的 Context，正常情况下资源由它创建和使用。
    QOpenGLContext* widget_context = context();
    bool widget_context_is_current = false;
    if (widget_context != nullptr && widget_context->isValid()) {
        makeCurrent();
        widget_context_is_current = QOpenGLContext::currentContext() == widget_context;
    }

    // 4. Widget Context 不可用时，尝试切换到初始化阶段创建的共享离屏 Context。
    QOpenGLContext* resource_context = widget_context_is_current ? widget_context : nullptr;
    if (resource_context == nullptr && retirement_context_ != nullptr && retirement_surface_ != nullptr &&
        retirement_context_->isValid()) {
        const bool retirement_context_is_current = retirement_context_->makeCurrent(retirement_surface_.get());
        if (retirement_context_is_current && QOpenGLContext::currentContext() == retirement_context_.get()) {
            resource_context = retirement_context_.get();
        }
    }

    if (resource_context != nullptr) {
        // 5. 已经获得可用 Context：重新初始化当前 Context 对应的 OpenGL 函数包装器。
        if (!widget_context_is_current) {
            initializeOpenGLFunctions();
        }

        // 6. Use each frame's Fence for a bounded wait. A context-wide glFinish() can block
        // indefinitely on a stalled GPU and would bypass the shared retirement reaper.
        // Fence failure or timeout leaves the complete resource record for that reaper.
        for (auto pending_resources = pending_frame_resources_.begin();
             pending_resources != pending_frame_resources_.end();) {
            PendingFrameResources& resources = **pending_resources;
            bool gpu_use_is_complete = resources.gpu_use_is_complete_;
            if (!gpu_use_is_complete && resources.fence_ != nullptr) {
                constexpr int kMaxFenceWaitAttempts = 10;
                for (int wait_attempt = 0; wait_attempt < kMaxFenceWaitAttempts; ++wait_attempt) {
                    const GLenum status = glClientWaitSync(resources.fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ULL);
                    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
                        gpu_use_is_complete = true;
                        break;
                    }
                    if (status != GL_TIMEOUT_EXPIRED) {
                        break;
                    }
                }
            }

            if (gpu_use_is_complete) {
                resources.gpu_use_is_complete_ = true;
                if (ReleasePendingFrameResources(resources, false)) {
                    pending_resources = pending_frame_resources_.erase(pending_resources);
                    continue;
                }
            }
            ++pending_resources;
        }

        // 8. 清理仍可访问的 GL 对象。帧资源未确认完成时，将由共享 retirement Context
        // 接手 Fence 轮询；队列非空期间新帧改走 CPU 下载，避免待回收资源继续累积。
        hardware_shader_program_.reset();
        shader_program_.reset();
        glDeleteTextures(static_cast<GLsizei>(texture_ids_.size()), texture_ids_.data());
        texture_ids_ = {0, 0, 0};
        if (widget_context_is_current) {
            vao_.destroy();
        }
        vbo_.destroy();
        ebo_.destroy();
        resource_context->doneCurrent();
    } else {
        LOG_WARN("No OpenGL context is available; deferred frame resources will remain owned until a context can reap them");
    }

    // 9. 将未确认的帧及其 DMA-BUF/AVFrame 所有权转交给进程级回收器。
    if (!pending_frame_resources_.empty()) {
        DeferPendingFrameResources();
    }

    // 10. 重置 CPU 侧初始化状态和 EGL 能力缓存，为下一次 Context 初始化做准备。
    texture_allocated_ = false;
    shader_ready_ = false;
    hardware_shader_ready_ = false;
    egl_import_ = {};
    retirement_surface_.reset();
    retirement_context_.reset();
}

void OpenGLView::DeferPendingFrameResources() {
    if (pending_frame_resources_.empty()) {
        return;
    }

    auto& reaper = GetDeferredGlResourceReaper();
    std::unique_ptr<DeferredResourceBatch> batch;
    try {
        auto deferred = std::make_unique<DeferredGlFrameBatch>(std::move(retirement_context_),
                                                       std::move(retirement_surface_),
                                                       egl_import_.display_,
                                                       egl_import_.destroy_image_);
        deferred->Adopt(pending_frame_resources_);
        batch = std::move(deferred);
    } catch (const std::bad_alloc&) {
        // Allocation failure leaves no safe owner capable of polling these Fences.
        // Retain every object until process exit and permanently disable new exports.
        LOG_ERROR("Could not allocate deferred GL resource batch; retaining DMA-BUF resources until process exit");
        reaper.DisableHardwarePermanently();
        for (auto& resources : pending_frame_resources_) {
            static_cast<void>(resources.release());
        }
        pending_frame_resources_.clear();
        static_cast<void>(retirement_surface_.release());
        static_cast<void>(retirement_context_.release());
        return;
    }

    if (!reaper.Defer(batch)) {
        LOG_ERROR("Could not enqueue deferred GL resource batch; retaining it until process exit");
        static_cast<void>(batch.release());
    }
}

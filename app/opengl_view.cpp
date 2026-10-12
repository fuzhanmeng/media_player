/**
 * @file opengl_view.cpp
 * @brief OpenGLView 视频渲染视窗核心实现
 * @details
 * 负责打通 CPU 解码像素内存与 GPU 硬件着色器管线：
 * 1. 线程隔离原则：所有 OpenGL 指令调用严格集中在 initializeGL / resizeGL / paintGL 中，
 *    确保仅在 Qt GUI 主事件循环线程执行；
 * 2. 跨线程信箱：后台工作线程仅通过 OnReceiveVideoFrame 写入互斥锁保护的单一最新帧槽位，
 *    并通过 Qt::QueuedConnection 唤醒 UI 线程重绘，绝不触碰任何 GL 状态；
 * 3. 硬件色彩空间转换：利用 OpenGL 3.3 Core Profile 与 GLSL 330 core 片段着色器，
 *    实现基于 BT.709 色彩矩阵的高性能 $YUV \rightarrow RGB$ 转换。
 */

#include "opengl_view.h"

#include <QMatrix3x3>
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

#include "mediacore/av_types/video_frame.h"
#include "mediacore/logger.h"

namespace {

/**
 * @brief 顶点着色器源码（GLSL 3.30 Core Profile）
 * @details
 * 顶点着色器针对全屏矩形的 4 个网格顶点执行：
 * 1. 坐标透传：CPU 传入的顶点坐标 aPos 已经是归一化设备坐标（NDC, [-1.0, 1.0]），
 *    无需模型-视图-投影（MVP）变换矩阵计算，直接补全为 4D 齐次裁剪坐标 gl_Position；
 * 2. 纹理插值传递：将纹理 UV 坐标 aTexCoord 透传至输出管道，由光栅化硬件在片元间自动完成透视校正插值。
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
 * 2. 色差零点矫正：U/V 色度分量在存储时是以 128（归一化为 0.5）为中点，减去 0.5 还原为 [-0.5, 0.5] 的有符号色差；
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

void main() {
    // 1. 采样并组装 YUV 向量
    vec3 yuv;
    // 使用 .r 读取红色通道（单通道 R8 存储格式）
    yuv.x = texture(texY, vTexCoord).r;
    
    // U 与 V 减去 0.5 偏置还原色差
    yuv.y = texture(texU, vTexCoord).r - 0.5;
    yuv.z = texture(texV, vTexCoord).r - 0.5;

    // 2. 硬件级矩阵乘法 (YUV -> RGB)
    vec3 rgb = u_colorMatrix * yuv;

    // 3. 输出不透明颜色
    fragColor = vec4(rgb, 1.0);
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
 * @brief ITU-R BT.709 标准 HDTV 色彩空间转换矩阵（按 OpenGL Column-Major 列优先存储）
 * @details
 * 数学公式：
 * R = Y + 1.5748 * V
 * G = Y - 0.1873 * U - 0.4681 * V
 * B = Y + 1.8556 * U
 */
constexpr std::array<float, 9> kMatrixBt709 = {
    1.0f,
    1.0f,
    1.0f,  // 第 1 列：Y 的加权系数
    0.0f,
    -0.1873f,
    1.8556f,  // 第 2 列：U 的加权系数
    1.5748f,
    -0.4681f,
    0.0f  // 第 3 列：V 的加权系数
};

}  // namespace

/**
 * @brief 构造函数：初始化视窗控件并绑定跨线程渲染信号槽
 * @param parent Qt 父对象指针
 * @details
 * 使用 Qt::QueuedConnection 将 FrameArrived 信号连接至自身的 update() 槽；
 * 确保后台工作线程无论何时 emit 该信号，最终的绘制驱动 update() 严格在 Qt GUI 主线程执行。
 */
OpenGLView::OpenGLView(QWidget* parent) : QOpenGLWidget(parent) {
    connect(this, &OpenGLView::FrameArrived, this, QOverload<>::of(&QOpenGLWidget::update), Qt::QueuedConnection);
}

/**
 * @brief 析构函数：释放 OpenGL 显存资源与上下文环境
 * @details
 * 销毁流程：
 * 1. 显式调用 makeCurrent()，将控件关联的 OpenGL 上下文绑定到当前调用线程；
 * 2. 调用 glDeleteTextures 彻底销毁预分配的 Y/U/V 三张显存纹理句柄；
 * 3. VAO/VBO/EBO 与 QOpenGLShaderProgram 作为 Qt RAII 对象，在自身析构时自动完成销毁；
 * 4. 调用 doneCurrent() 解绑上下文。
 */
OpenGLView::~OpenGLView() {
    makeCurrent();

    // 释放由 SetupTextures 创建的 Y/U/V 显存纹理句柄
    glDeleteTextures(static_cast<GLsizei>(texture_ids_.size()), texture_ids_.data());

    doneCurrent();
    LOG_INFO("OpenGLView destroyed, OpenGL resources released");
}

/**
 * @brief 接收上游解码或渲染节点投递的 YUV420P 视频帧（IVideoSink 实现）
 * @param video_frame 共享所有权的有效视频图像帧
 * @details
 * 执行机制：
 * 1. 指针防空检查；
 * 2. 加锁保护信箱（current_frame_），新到达的帧直接覆盖旧帧（旧帧引用计数减 1 触发释放）；
 *    采用“只呈现最新帧、宁丢旧帧不堆积延迟”的实时渲染丢帧策略；
 * 3. 跨线程发射 FrameArrived 信号，非阻塞唤醒 GUI 主线程。
 */
void OpenGLView::OnReceiveVideoFrame(std::shared_ptr<mediacore::VideoFrame> video_frame) {
    if (video_frame == nullptr) {
        LOG_WARN("Video frame ignored: null frame");
        return;
    }

    LOG_TRACE("Video frame received: {}x{}", video_frame->width_, video_frame->height_);

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        current_frame_ = std::move(video_frame);
    }

    // 队列化唤醒 UI 线程重绘
    emit FrameArrived();
}

/**
 * @brief 初始化 OpenGL 运行环境与显存资源
 * @details
 * 初始化步骤：
 * 1. 调用 initializeOpenGLFunctions() 加载当前驱动上下文对应的 3.3 Core 函数指针；
 * 2. 配置默认清屏底色（深灰蓝色）；
 * 3. 编译并链接 GLSL 矩阵色彩转换着色器；
 * 4. 配置全屏四边形几何顶点数据（VAO/VBO/EBO）；
 * 5. 预先生成 Y/U/V 纹理句柄并设置双线性插值采样参数。
 */
void OpenGLView::initializeGL() {
    LOG_INFO("OpenGL initialization started");

    initializeOpenGLFunctions();

    // 设置默认清屏底色（RGBA）
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);

    shader_ready_ = BuildShader();
    SetupGeometry();
    SetupTextures();

    if (shader_ready_) {
        LOG_INFO("OpenGL initialization finished");
    } else {
        LOG_ERROR("OpenGL initialization finished with shader errors");
    }
}

/**
 * @brief 视窗几何尺寸重置响应
 * @param width 控件新的物理像素宽度
 * @param height 控件新的物理像素高度
 * @details 动态调整 glViewport 视口，映射归一化设备坐标至物理窗口区域。
 */
void OpenGLView::resizeGL(int width, int height) {
    glViewport(0, 0, width, height);
    LOG_INFO("OpenGL viewport resized to {}x{}", width, height);
}

/**
 * @brief 核心呈现绘制循环（PaintGL）
 * @details
 * 渲染流程：
 * 1. 调用 glClear 清除颜色缓冲区；
 * 2. 状态门控拦截：若着色器编译失败或信箱无有效数据帧，仅保留清屏底色直接返回；
 * 3. 获取信箱最新帧的共享引用；
 * 4. 调用 UploadFrameTextures 将 Y/U/V 三平面像素上传至显存；
 * 5. 绑定着色器并上传 BT.709 色彩空间转换矩阵；
 * 6. 依次绑定对应纹理单元（Texture Unit 0/1/2）；
 * 7. 绑定 VAO 几何布局，通过 glDrawElements 执行索引图元绘制。
 */
void OpenGLView::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);

    if (!shader_ready_) {
        LOG_TRACE("Paint skipped because shader is not ready");
        return;
    }

    // 1. 从信箱中安全提取当前最新帧
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

    // 2. 上传 Y/U/V 数据至对应 GPU 纹理
    LOG_TRACE("Painting frame: {}x{}", frame->width_, frame->height_);
    UploadFrameTextures(*frame);

    // 3. 激活着色器并传递色彩转换参数
    shader_program_.bind();
    shader_program_.setUniformValue("u_colorMatrix", QMatrix3x3(kMatrixBt709.data()));

    // 4. 分别激活纹理单元并绑定对应的 2D 纹理对象
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[1]);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[2]);

    // 5. 绑定顶点数组并绘制全屏四边形
    vao_.bind();
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    // 6. 解绑恢复状态
    vao_.release();
    shader_program_.release();
}

/**
 * @brief 编译并链接 GLSL 着色器程序
 * @return 编译链接完全成功返回 true，否则记录日志并返回 false
 * @details
 * 编译成功后立即绑定程序并将 texY/texU/texV 三个采样器 uniform
 * 与固定的纹理单元插槽（0, 1, 2）建立绑定映射。
 */
bool OpenGLView::BuildShader() {
    if (!shader_program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSource)) {
        LOG_ERROR("Vertex shader compilation failed: {}", shader_program_.log().toStdString());
        return false;
    }

    if (!shader_program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShaderSource)) {
        LOG_ERROR("Fragment shader compilation failed: {}", shader_program_.log().toStdString());
        return false;
    }

    if (!shader_program_.link()) {
        LOG_ERROR("Shader program link failed: {}", shader_program_.log().toStdString());
        return false;
    }

    // 建立片元着色器 Uniform 采样器与对应纹理单元的静态绑定
    shader_program_.bind();
    shader_program_.setUniformValue("texY", 0);
    shader_program_.setUniformValue("texU", 1);
    shader_program_.setUniformValue("texV", 2);
    shader_program_.release();

    LOG_INFO("Shader program compiled and linked");
    return true;
}

/**
 * @brief 构建全屏四边形几何数据（VAO/VBO/EBO）
 * @details
 * 数据装配规范：
 * - 属性槽 0：顶点位置（2 个 float，偏移 0）；
 * - 属性槽 1：纹理坐标（2 个 float，偏移 2 * sizeof(float)）；
 * - 顶点总步幅（Stride）：4 * sizeof(float)。
 */
void OpenGLView::SetupGeometry() {
    vao_.create();
    vao_.bind();

    // 1. 初始化顶点缓冲对象（VBO）并写入网格属性
    vbo_.create();
    vbo_.bind();
    vbo_.allocate(kVertices.data(), static_cast<int>(sizeof(float) * kVertices.size()));

    // 2. 初始化索引缓冲对象（EBO）并写入拓扑顺序
    ebo_.create();
    ebo_.bind();
    ebo_.allocate(kIndices.data(), static_cast<int>(sizeof(unsigned int) * kIndices.size()));

    const int stride = 4 * static_cast<int>(sizeof(float));

    // 配置属性槽 0（位置）
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // 配置属性槽 1（纹理坐标）
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(2 * sizeof(float)));  // NOLINT
    glEnableVertexAttribArray(1);

    vao_.release();
    LOG_DEBUG("OpenGL geometry prepared");
}

/**
 * @brief 预分配 Y/U/V 三张 2D 纹理对象并配置采样过滤参数
 * @details
 * 采样过滤统一采用 GL_LINEAR（双线性插值），寻址环绕采用 GL_CLAMP_TO_EDGE（边缘钳位），
 * 规避缩放贴图边缘出现黑边采样伪影。
 */
void OpenGLView::SetupTextures() {
    glGenTextures(static_cast<GLsizei>(texture_ids_.size()), texture_ids_.data());
    LOG_DEBUG("OpenGL texture handles generated: {}, {}, {}", texture_ids_[0], texture_ids_[1], texture_ids_[2]);

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
 * @brief 将视频帧的 Y/U/V 三平面数据上传至 GPU 显存纹理
 * @param frame 待上传的有效视频帧对象
 * @details
 * 上传与显存管理策略：
 * 1. 显存复用：分辨率未变时，使用 glTexSubImage2D 覆盖原有显存，零额外开销；
 *    分辨率发生变动时，使用 glTexImage2D 重新分配显存；
 * 2. 内存对齐保护：设置 GL_UNPACK_ALIGNMENT = 1，防止画面宽度非 4 的倍数时产生行倾斜；
 * 3. 行跨距支持：设置 GL_UNPACK_ROW_LENGTH 为 frame.linesize_[i]，完美支持携带对齐填充（Padding）的解码帧；
 *    上传完成后立即将 GL_UNPACK_ROW_LENGTH 还原为 0，防止影响后续全局 OpenGL 绘制。
 */
void OpenGLView::UploadFrameTextures(const mediacore::VideoFrame& frame) {
    const int frame_width = frame.width_;
    const int frame_height = frame.height_;

    // 检查分辨率是否发生变化
    const bool need_realloc =
        !texture_allocated_ || frame_width != last_frame_width_ || frame_height != last_frame_height_;

    if (need_realloc) {
        LOG_INFO("Reallocating video textures for frame size {}x{}", frame_width, frame_height);
    }

    // 1. 设置解包内存对齐为 1 字节（防止非 4 字节对齐的奇数尺寸画面倾斜）
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 2. 计算各平面物理有效尺寸（YUV420P 格式 U/V 宽高向上取整为 Y 的一半）
    const int uv_width = (frame_width + 1) / 2;
    const int uv_height = (frame_height + 1) / 2;

    const std::array<int, 3> plane_widths = {frame_width, uv_width, uv_width};
    const std::array<int, 3> plane_heights = {frame_height, uv_height, uv_height};

    // 3. 循环上传三平面数据（0: Y, 1: U, 2: V）
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texture_ids_[i]);

        // 告知 OpenGL CPU 内存中每行的实际步幅（包含 Padding 字节数）
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize_[i]);

        if (need_realloc) {
            // 分辨率变更：重新分配显存
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
            // 分辨率未变：热更新显存现有内容
            glTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, 0, plane_widths[i], plane_heights[i], GL_RED, GL_UNSIGNED_BYTE, frame.data_[i]);
        }
    }

    // 4. 重置解包行长状态，避免污染其他 UI 绘制逻辑
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    // 5. 更新分配状态与分辨率快照
    if (need_realloc) {
        texture_allocated_ = true;
        last_frame_width_ = frame_width;
        last_frame_height_ = frame_height;
    }
}
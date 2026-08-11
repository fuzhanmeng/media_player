/**
 * @file opengl_view.cpp
 * @brief OpenGLView 实现：YUV420P 三平面纹理上传 + 片段着色器 YUV->RGB + 全屏矩形渲染
 * @details 全部 GL 调用集中在 GUI 线程（initializeGL/resizeGL/paintGL），后台线程只负责把帧投进信箱
 */
#include "opengl_view.h"

#include <QMatrix3x3>  // 用于传递色彩空间转换矩阵给 Shader
#include <array>

#include "mediacore/av_types/video_frame.h"
#include "mediacore/logger.h"

namespace {

// 1. 着色器代码 (Shader Programs)

/**
 * @brief 顶点着色器源码（GLSL 3.30 Core）
 * @details 顶点着色器在 GPU 上对"每个顶点"执行一次
 *          主要职责：
 *          1. 把 CPU 传入的顶点位置输出为裁剪空间坐标 gl_Position
 *          2. 把纹理坐标 aTexCoord 透传给片段着色器，GPU 会在三角形内部自动插值
 */
constexpr auto kVertexShaderSource = R"(#version 330 core

layout(location = 0) in vec2 aPos;      // 接收顶点位置的输入口（插槽 0）
layout(location = 1) in vec2 aTexCoord; // 接收纹理坐标的输入口（插槽 1）

out vec2 vTexCoord;                     // 向外发送数据的出口，传给片段着色器

void main() {
    // 决定该顶点在屏幕上的最终位置（补全 4D 向量：z=0.0，w=1.0）
    // aPos 已经是 NDC 坐标(-1~1)，故直接输出，无需 MVP 矩阵变换
    gl_Position = vec4(aPos, 0.0, 1.0);
    
    // 透传：把接收到的纹理坐标原封不动地塞进输出管道
    vTexCoord = aTexCoord;
}
)";

/**
 * @brief 片段着色器源码（GLSL 3.30 Core）- 高性能矩阵版
 * @details 片段着色器在 GPU 上对"每个像素（片元）"并行执行一次
 *          这里是 YUV420P 到 RGB 转换发生地从 3 张单通道纹理采样，
 *          再使用 3x3 矩阵乘法瞬间算出 RGB，彻底解放 CPU
 */
constexpr auto kFragmentShaderSource = R"(#version 330 core

// ================= 输入与输出 =================
in vec2 vTexCoord;       // 从顶点着色器插值而来的纹理坐标
out vec4 fragColor;      // 输出：该像素最终的 RGBA 颜色

// ================= 统一变量 =================
uniform sampler2D texY;  // Y 平面纹理（存放于纹理单元 0）
uniform sampler2D texU;  // U 平面纹理（存放于纹理单元 1）
uniform sampler2D texV;  // V 平面纹理（存放于纹理单元 2）

// 由 C++ 传入的 3x3 色彩空间转换矩阵 (BT.709 或 BT.2020)
uniform mat3 u_colorMatrix; 

void main() {
    // 1. 采样并构建 YUV 向量
    vec3 yuv;
    // .r 表示读取红色通道（因为我们用 R8 单通道格式存储数据）
    yuv.x = texture(texY, vTexCoord).r;
    
    // U 和 V 以 128（即 0.5）为无色中点，减去 0.5 还原为有符号的色差值
    yuv.y = texture(texU, vTexCoord).r - 0.5;
    yuv.z = texture(texV, vTexCoord).r - 0.5;

    // 2. 硬件级矩阵乘法 (YUV -> RGB)，一行代码完成所有浮点运算
    vec3 rgb = u_colorMatrix * yuv;

    // 3. 输出最终颜色（透明度设为 1.0 完全不透明）
    fragColor = vec4(rgb, 1.0);
}
)";

// 2. 几何体数据与色彩矩阵 (Geometry & Color Math)

/**
 * @brief 全屏矩形顶点数据：4 个顶点 × (2维位置 + 2维纹理坐标)
 * @details
 *   1. 位置 (NDC坐标)：范围 -1.0 ~ 1.0。(-1,-1)为左下角，(1,1)为右上角，撑满整个屏幕。
 *   2. 纹理 (UV坐标)：由于视频内存的第一行(V=0)是人头(画面顶部)，而屏幕Y=1才是顶部。
 *      【核心修复】这里将 V 轴进行了倒置映射，防止画面倒立！
 */
constexpr std::array<float, 16> kVertices = {
    // 顶点位置 NDC (X, Y)  // 纹理挂钩 (U, V) -> V轴已倒置！
    -1.0f,
    -1.0f,
    0.0f,
    1.0f,  // 屏幕左下角 -> 绑定画面最底端 (脚)
    1.0f,
    -1.0f,
    1.0f,
    1.0f,  // 屏幕右下角 -> 绑定画面最底端
    1.0f,
    1.0f,
    1.0f,
    0.0f,  // 屏幕右上角 -> 绑定画面最顶端 (人头)
    -1.0f,
    1.0f,
    0.0f,
    0.0f  // 屏幕左上角 -> 绑定画面最顶端 (人头)
};

/**
 * @brief 索引数组：指示 GPU 如何用三角形拼成矩形
 * @details
 *   GPU 只能绘制三角形。这里复用上面的 4 个顶点，通过索引连线，将其分割为 2 个三角形。
 *   - 逆时针绕序 (CCW)：OpenGL 默认认为逆时针连线的是正面。
 */
constexpr std::array<unsigned int, 6> kIndices = {
    0,
    1,
    2,  // 第 1 个三角形：左下 -> 右下 -> 右上
    2,
    3,
    0  // 第 2 个三角形：右上 -> 左上 -> 左下
};

/**
 * @brief BT.709 色彩转换矩阵 (适用于 1080p 及绝大多数 4K SDR 视频)
 * @note OpenGL 的 mat3 在内存中按列优先 (Column-Major) 存储
 */
constexpr std::array<float, 9> kMatrixBt709 = {
    1.0f,
    1.0f,
    1.0f,  // 第 1 列 (对应 Y 的系数)
    0.0f,
    -0.1873f,
    1.8556f,  // 第 2 列 (对应 U 的系数)
    1.5748f,
    -0.4681f,
    0.0f  // 第 3 列 (对应 V 的系数)
};

}  // namespace

// 3. 类方法实现

/**
 * @brief 构造函数
 * @details 跨线程投递的核心连接：FrameArrived 信号 -> update() 槽 Qt::QueuedConnection：
 *          无论 emit 发生在哪个线程，都是异步投递
 */
OpenGLView::OpenGLView(QWidget* parent) : QOpenGLWidget(parent) {
    connect(this, &OpenGLView::FrameArrived, this, QOverload<>::of(&QOpenGLWidget::update), Qt::QueuedConnection);
}

/**
 * @brief 析构函数
 */
OpenGLView::~OpenGLView() {
    // 释放 GPU 资源之前，必须先 makeCurrent：把本控件的 GL 上下文绑定到当前线程
    // 否则 glDeleteTextures 等调用无上下文可作用，导致显存泄露或者崩溃
    makeCurrent();

    // glDeleteTextures：删除 SetupTextures 创建的 Y/U/V 三平面对应的纹理句柄，归还显存
    glDeleteTextures(static_cast<GLsizei>(texture_ids_.size()), texture_ids_.data());

    // vbo_/ebo_/vao_ 是 Qt 封装对象，析构时会自动 destroy()，无需手动删除
    // QOpenGLShaderProgram 同理，RAII自动释放着色器程序

    // 解绑上下文，与 makeCurrent 配对
    doneCurrent();

    LOG_INFO("OpenGLView destroyed, OpenGL resources released");
}

/**
 * @brief 信箱模式接收来自后台线程传递过来的一帧视频帧 (由后台生产线程调用)
 */
void OpenGLView::OnReceiveVideoFrame(std::shared_ptr<mediacore::VideoFrame> video_frame) {
    if (video_frame == nullptr) {
        LOG_WARN("Video frame ignored: null frame");
        return;
    }

    LOG_TRACE("Video frame received: {}x{}", video_frame->width_, video_frame->height_);

    {
        // 信箱写入：加锁，新帧直接覆盖旧帧（旧 shared_ptr 引用计数减一，可能触发释放）
        // 宁可丢掉来不及渲染的旧帧，也不堆积延迟
        std::lock_guard<std::mutex> lock(frame_mutex_);
        current_frame_ = std::move(video_frame);
    }

    // 通知 GUI 线程，信箱中有新的一帧到来，该重绘了
    // 因为是 QueuedConnection，此处 emit 立即返回，不阻塞后台生产线程
    emit FrameArrived();
}

/**
 * @brief GL 生命周期回调（全部在 GUI 线程、上下文已 makeCurrent）
 */
void OpenGLView::initializeGL() {
    LOG_INFO("OpenGL initialization started");

    // initializeOpenGLFunctions：加载当前上下文对应的 OpenGL 3.3 函数指针
    // 必须在任何 gl* 调用前执行一次，由 OpenGLFunctions_3_3_Core 提供
    initializeOpenGLFunctions();

    // glClearColor：设置清屏颜色（RGBA），此处深灰色后续 glClear 用此颜色填充
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);

    shader_ready_ = BuildShader();  // 编译链接着色器
    SetupGeometry();  // 上传全屏矩形顶点到 VAO/VBO/EBO
    SetupTextures();  // 预分配 Y/U/V 三张纹理句柄

    if (shader_ready_) {
        LOG_INFO("OpenGL initialization finished");
    } else {
        LOG_ERROR("OpenGL initialization finished with shader errors");
    }
}

/**
 * @brief 窗口大小调整：窗口大小变化时更新 glViewport
 */
void OpenGLView::resizeGL(int width, int height) {
    // glViewport：定义 NDC（-1，1）映射到窗口的哪块像素区域
    glViewport(0, 0, width, height);
    LOG_INFO("OpenGL viewport resized to {}x{}", width, height);
}

/**
 * @brief 渲染一帧：从信箱取最新帧，上传三平面纹理，着色器绘制到屏幕
 */
void OpenGLView::paintGL() {
    // 用 initializeGL 设的颜色填充后缓冲
    glClear(GL_COLOR_BUFFER_BIT);

    if (!shader_ready_) {
        LOG_TRACE("Paint skipped because shader is not ready");
        return;
    }

    // 1. 从信箱取最新帧的副本
    std::shared_ptr<mediacore::VideoFrame> frame;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame = current_frame_;
    }

    // 如果信箱空了、或者帧无效（空帧/Flush/EOF信令帧），只清屏，不渲染纹理
    if (frame == nullptr || !frame->IsValid()) {
        LOG_TRACE("Paint skipped because no valid frame is available");
        return;
    }

    // 2. 上传 Y/U/V 三平面数据到显存纹理
    LOG_TRACE("Painting frame: {}x{}", frame->width_, frame->height_);
    UploadFrameTextures(*frame);

    // 3. 激活着色器程序
    shader_program_.bind();

    // 将色彩转换矩阵传给 Shader
    shader_program_.setUniformValue("u_colorMatrix", QMatrix3x3(kMatrixBt709.data()));

    // 激活 3 个纹理单元并绑定对应纹理
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[0]);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[1]);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, texture_ids_[2]);

    // 4. 绑定 VAO：恢复 SetupGeometry 记录的顶点布局
    vao_.bind();

    // 5. glDrawElements：按索引绘制图元（两个三角形）到【后缓冲】
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    // 收尾解绑
    vao_.release();
    shader_program_.release();
}

/**
 * @brief 编译链接 YUV -> RGB 的顶点 + 片段着色器
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

    // 把片段着色器里的 3 个采样器 uniform 绑定到固定纹理单元编号
    shader_program_.bind();
    shader_program_.setUniformValue("texY", 0);
    shader_program_.setUniformValue("texU", 1);
    shader_program_.setUniformValue("texV", 2);
    shader_program_.release();

    LOG_INFO("Shader program compiled and linked");
    return true;
}

/**
 * @brief 上传全屏矩形顶点（位置 + 纹理坐标）到 VAO/VBO/EBO
 */
void OpenGLView::SetupGeometry() {
    vao_.create();
    vao_.bind();

    // VBO：上传顶点数据
    vbo_.create();
    vbo_.bind();

    vbo_.allocate(kVertices.data(), static_cast<int>(sizeof(float) * kVertices.size()));

    // EBO：上传索引数据
    ebo_.create();
    ebo_.bind();
    ebo_.allocate(kIndices.data(), static_cast<int>(sizeof(unsigned int) * kIndices.size()));

    // 每个顶点 4 个 float：前 2 个是位置，后 2 个是纹理坐标
    const int stride = 4 * static_cast<int>(sizeof(float));

    // 属性槽 0 = 位置
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // 属性槽 1 = 纹理坐标 (偏移 2*float)
    // 修复了原写法的 bug：GL_FALSE 应该是 GL_FLOAT
    // VBO 绑定状态下，OpenGL 把最后一个参数解释为“顶点内字节偏移”而不是普通 CPU 指针
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(2 * sizeof(float)));  // NOLINT

    glEnableVertexAttribArray(1);

    vao_.release();
    LOG_DEBUG("OpenGL geometry prepared");
}

/**
 * @brief 创建 3 个纹理句柄并设置采样/环绕参数
 */
void OpenGLView::SetupTextures() {
    glGenTextures(static_cast<GLsizei>(texture_ids_.size()), texture_ids_.data());
    LOG_DEBUG("OpenGL texture handles generated: {}, {}, {}", texture_ids_[0], texture_ids_[1], texture_ids_[2]);

    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texture_ids_[i]);

        // 缩放过滤：线性插值
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // 环绕方式：钳到边缘
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    LOG_INFO("OpenGL textures prepared");
}

/**
 * @brief 将当前帧的 Y/U/V 三平面分别上传到对应纹理显存中
 */
void OpenGLView::UploadFrameTextures(const mediacore::VideoFrame& frame) {
    const int frame_width = frame.width_;
    const int frame_height = frame.height_;

    // 1. 检测分辨率变化：若变了，需重新分配显存；否则只更新池水
    const bool need_realloc =
        !texture_allocated_ || frame_width != last_frame_width_ || frame_height != last_frame_height_;

    if (need_realloc) {
        LOG_INFO("Reallocating video textures for frame size {}x{}", frame_width, frame_height);
    }

    // 2. 内存对齐保护
    // 强制 OpenGL 读取 CPU 内存时按 1 字节逐个紧凑读取
    // 防止宽度不为 4 的倍数时产生画面倾斜或斜切条纹
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 3. 计算三平面尺寸：Y 全分辨率，U/V 宽高减半 (YUV420P 特性)
    // 计算半区宽度必须用当前帧的 kFrameWidth，不能用上一帧的
    const int uv_width = (frame_width + 1) / 2;
    const int uv_height = (frame_height + 1) / 2;

    const std::array<int, 3> plane_widths = {frame_width, uv_width, uv_width};
    const std::array<int, 3> plane_heights = {frame_height, uv_height, uv_height};

    // 4. 执行上传：循环处理 Y(0), U(1), V(2)
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texture_ids_[i]);

        if (need_realloc) {
            // [情况 A] 重新开辟显存并填入第一帧数据
            // GL_R8: GPU内部单通道8位; GL_RED: CPU数据也是单通道
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
            // [情况 B] 仅更新已有显存的内容（省去分配开销，性能极高）
            // 直接从坐标 (0,0) 开始覆盖 WxH 的数据块
            glTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, 0, plane_widths[i], plane_heights[i], GL_RED, GL_UNSIGNED_BYTE, frame.data_[i]);
        }
    }

    // 5. 更新状态与历史记录
    if (need_realloc) {
        texture_allocated_ = true;
        last_frame_width_ = frame_width;
        last_frame_height_ = frame_height;
    }
}

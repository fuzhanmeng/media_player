/**
 * @file opengl_view.cpp
 * @brief OpenGLView 实现：YUV420P 三平面纹理上传 + 片段着色器 YUV->RGB 转换 + 全屏矩形渲染
 * @details
 * 所有 GL 调用集中在 initializeGL/resizeGL/paintGL 中，均在 GUI 线程执行，后台线程只通过
 * OnReceiveVideoFrame 把帧写入信箱，不直接触碰任何 GL 状态
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
 * @details
 * 顶点着色器对每个顶点执行一次，职责有两点：
 * 主要职责：
 * 1. 把 CPU 传入的顶点位置输出为裁剪空间坐标 gl_Position
 * 2. 把纹理坐标 aTexCoord 透传给片段着色器，GPU 会在三角形内部自动插值
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
 * @brief 片段着色器源码（GLSL 3.30 Core）
 * @details
 * 片段着色器在 GPU 上对每个像素（片元）并执行一次，这里从 3 张单通道纹理采样出 YUV，再用 3x3 矩阵乘法
 * 一次性算出 RGB
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
 * 1. 位置（NDC 坐标）：范围 -1.0 ~ 1.0，(-1,-1) 为左下角，(1,1) 为右上角，撑满整个视口。
 * 2. 纹理（UV 坐标）：视频数据里第一行（V=0）对应画面顶部，而纹理坐标 V=0 在 OpenGL 里
 * 对应贴图底部，因此这里对 V 轴做了倒置映射，避免画面上下颠倒
 */
constexpr std::array<float, 16> kVertices = {
    // 顶点位置 NDC (X, Y)  // 纹理挂钩 (U, V) -> V轴已倒置！
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
 * @brief 索引数组：指示 GPU 如何用三角形拼成矩形
 * @details
 * OpenGL 只能绘制三角形，这里复用上面 4 个顶点，按索引连线拆分为 2 个三角形
 * 逆时针绕序（CCW）：OpenGL 默认将逆时针连线的三角形视为正面
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
    1.0f,  // 第 1 列（Y 的系数）
    0.0f,
    -0.1873f,
    1.8556f,  // 第 2 列（U 的系数）
    1.5748f,
    -0.4681f,
    0.0f  // 第 3 列（V 的系数）
};

}  // namespace

// 3. 类方法实现

/**
 * @brief 构造函数
 * @details
 * 把 FrameArrived 信号连接到自身的 update()，连接类型为 Qt::QueueConnection，无论 emit 发生在
 * 哪个线程，update() 调用都会被投递到本对象所在的 GUI 线程执行，这是后台线程通知 GUI 线程重绘的关键
 */
OpenGLView::OpenGLView(QWidget* parent) : QOpenGLWidget(parent) {
    connect(this, &OpenGLView::FrameArrived, this, QOverload<>::of(&QOpenGLWidget::update), Qt::QueuedConnection);
}

/**
 * @brief 析构函数
 */
OpenGLView::~OpenGLView() {
    // 释放 GPU 资源前必须先 makeCurrent，把本控件的 GL 上下文绑定到当前线程
    // 否则 glDeleteTextures 等调用没有上下文可作用，会导致显存泄漏或崩溃
    makeCurrent();

    // 删除 SetupTextures 创建的 Y/U/V 三张纹理句柄，归还显存
    glDeleteTextures(static_cast<GLsizei>(texture_ids_.size()), texture_ids_.data());

    // vbo_/ebo_/vao_ 是 Qt 封装对象，析构时会自动 destroy()，无需手动释放
    // QOpenGLShaderProgram 同理，由 RAII 自动释放着色器程序

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
        // 宁可丢弃来不及渲染的旧帧，也不让未渲染的帧堆积造成延迟
        std::lock_guard<std::mutex> lock(frame_mutex_);
        current_frame_ = std::move(video_frame);
    }

    // 通知 GUI 线程信箱中有新帧、需要重绘
    // 由于是 Qt::QueuedConnection，emit 在这里立即返回，不会阻塞当前的后台生产线程
    emit FrameArrived();
}

/**
 * @brief GL 生命周期回调（均在 GUI 线程执行，上下文已由 Qt 完成 makeCurrent）
 */
void OpenGLView::initializeGL() {
    LOG_INFO("OpenGL initialization started");

    // 加载当前上下文对应的 OpenGL 3.3 函数指针，必须在任何 gl* 调用之前执行一次
    initializeOpenGLFunctions();

    // 设置清屏颜色（RGBA），后续 glClear 会用这个颜色填充缓冲区
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
 * @brief 窗口大小调整：更新 glViewport 以匹配新的物理尺寸
 */
void OpenGLView::resizeGL(int width, int height) {
    // glViewport：定义 NDC（-1，1）映射到窗口的哪块像素区域
    glViewport(0, 0, width, height);
    LOG_INFO("OpenGL viewport resized to {}x{}", width, height);
}

/**
 * @brief 渲染一帧：从信箱取最新帧，上传三平面纹理，用着色器绘制到屏幕
 */
void OpenGLView::paintGL() {
    // 用 initializeGL 设的颜色填充后缓冲
    glClear(GL_COLOR_BUFFER_BIT);

    if (!shader_ready_) {
        LOG_TRACE("Paint skipped because shader is not ready");
        return;
    }

    // 1. 从信箱取出最新帧的一份引用
    std::shared_ptr<mediacore::VideoFrame> frame;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame = current_frame_;
    }

    // 信箱为空，或帧无效（例如空帧、Flush/EOF 控制信令帧），只清屏不渲染纹理
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
 * @brief 编译链接 YUV -> RGB 的顶点着色器与片段着色器
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

    // 每个顶点占 4 个 float：前 2 个是位置，后 2 个是纹理坐标
    const int stride = 4 * static_cast<int>(sizeof(float));

    // 属性槽 0 = 位置
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);

    // 属性槽 1 = 纹理坐标（偏移 2 个 float）
    // 在 VBO 已绑定的状态下，glVertexAttribPointer 的最后一个参数会被解释为顶点内的
    // 字节偏移，而不是普通的 CPU 指针
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

    // 检测分辨率是否变化：变化时需要重新分配显存，否则只更新已有纹理的内容
    const bool need_realloc =
        !texture_allocated_ || frame_width != last_frame_width_ || frame_height != last_frame_height_;

    if (need_realloc) {
        LOG_INFO("Reallocating video textures for frame size {}x{}", frame_width, frame_height);
    }

    // 2. 内存对齐保护
    // 强制 OpenGL 按 1 字节对齐读取 CPU 内存，防止宽度不是 4 的倍数时出现行错位、画面倾斜
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 3. 计算三平面尺寸：Y 平面与视频帧同分辨率；YUV420P 的 U/V 平面宽高分别为 Y 平面的 1/2
    // 使用当前帧的宽高计算，确保分辨率变化后 U/V 尺寸也同步更新
    const int uv_width = (frame_width + 1) / 2;
    const int uv_height = (frame_height + 1) / 2;

    const std::array<int, 3> plane_widths = {frame_width, uv_width, uv_width};
    const std::array<int, 3> plane_heights = {frame_height, uv_height, uv_height};

    // 4. 执行上传：循环处理 Y(0), U(1), V(2)
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texture_ids_[i]);

        if (need_realloc) {
            // 情况 A：分辨率变化，重新分配显存并写入当前帧数据
            // GL_R8: GPU 内部存储格式为单通道 8 位；GL_RED：CPU 侧数据同样是单通道
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
            // 情况 B：分辨率未变，仅更新已有显存内容，省去重新分配的开销
            glTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, 0, plane_widths[i], plane_heights[i], GL_RED, GL_UNSIGNED_BYTE, frame.data_[i]);
        }
    }

    // 5. 更新分配状态和分辨率历史记录，供下一帧判断是否需要重新分配
    if (need_realloc) {
        texture_allocated_ = true;
        last_frame_width_ = frame_width;
        last_frame_height_ = frame_height;
    }
}

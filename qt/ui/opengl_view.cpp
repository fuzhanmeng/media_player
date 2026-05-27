/**
 * @file opengl_view.cpp
 */
#include "opengl_view.h"

#include "logger.h"
#include "video_frame.h" // 确保包含真正的帧结构定义

namespace av {

OpenGLView::OpenGLView(QWidget* parent) : QOpenGLWidget(parent) {}

OpenGLView::~OpenGLView() {
    // 释放 OpenGL 显存资源时，必须确保当前线程持有 OpenGL Context
    makeCurrent();
    if (m_textureId != 0) {
        glDeleteTextures(1, &m_textureId);
    }
    doneCurrent();
}

void OpenGLView::OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    LOG_INFO("Receive Video Frame from VideoDecoder");

    if (!video_frame)
        return;

    {
        // 加锁保护共享资源：将底层传递过来的帧数据转交给视图持有
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_currentFrame = video_frame;
    }

    // 跨线程 UI 刷新通知：
    // update() 是 QWidget 提供的方法，它是绝对线程安全的
    // 它不会立即执行绘制，而是向主线程事件循环投递一个 Paint 事件，由主线程在下一帧调度 paintGL()
    update();
}

void OpenGLView::initializeGL() {
    // 初始化 OpenGL 函数指针，必须在第一步调用
    initializeOpenGLFunctions();

    // 设置清屏底色为纯黑色
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    // 预先向显卡申请一个 2D 纹理对象句柄
    glGenTextures(1, &m_textureId);
    glBindTexture(GL_TEXTURE_2D, m_textureId);

    // 配置纹理的缩放插值算法为线性过滤（边缘平滑）
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // 解绑纹理
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLView::resizeGL(int width, int height) {
    // 映射 OpenGL 的标准化设备坐标 (NDC) 到实际的屏幕像素视口
    glViewport(0, 0, width, height);
}

void OpenGLView::paintGL() {
    // 1. 清空颜色缓冲区
    glClear(GL_COLOR_BUFFER_BIT);

    // 2. 线程安全地获取当前帧的智能指针副本
    std::shared_ptr<VideoFrame> frameToRender;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        frameToRender = m_currentFrame;
    }

    // 3. 有效性校验：若无数据或尺寸异常，维持黑屏状态
    if (!frameToRender || !frameToRender->data_.data() || frameToRender->width_ <= 0) {
        return;
    }

    // ==========================================
    // 预留区域：将 frameToRender 内部的 YUV/RGB 数据
    // 上传到 m_textureId 并执行 Shader 绘制管线
    // 我们将在后续完善此处的具体 OpenGL 绘图代码
    // ==========================================
}

} // namespace av
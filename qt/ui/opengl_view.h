/**
 * @file opengl_view
 * @brief 自定义视频渲染视图,专门用于接管底层解码后推上来的视频帧（YUV 或 RGB），并使用自定义 Shader 进行硬件加速渲染
 */
#pragma once

#include <QOpenGLFunctions_3_3_Core> // 提供了现代 OpenGL 3.3 核心模式的所有函数声明
#include <QOpenGLWidget> // 提供了一个可以直接执行原生 OpenGL API 命令的 Qt 窗口容器
#include <memory>
#include <mutex>

#include "ivideo_sink.h"

namespace av {
/**
 * @brief 自定义视频渲染视图
 * 1. QOpenGLWidget: 提供 Qt 窗口环境
 * 2. QOpenGLFunctions_3_3_Core: 提供底层 OpenGL API 解析
 * 3. IVideoSink: 实现底层核心引擎的数据接收协议
 */
class OpenGLView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core, public IVideoSink {
    Q_OBJECT

public:
    explicit OpenGLView(QWidget* parent = nullptr);

    ~OpenGLView() override;

    /**
     * @brief 接收底层投递上来的视频帧
     * @note 该函数由底层后台解码线程触发，内部实现必须保证线程安全
     */
    void OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) override;

protected:
    // 初始化显卡状态,分配纹理资源
    void initializeGL() override;

    // 视口尺寸调整
    void resizeGL(int width, int high) override;

    // 调用 OpenGL API，核心渲染循环
    void paintGL() override;

private:
    /// 用于跨线程同步视频帧数据的互斥锁
    std::mutex m_frameMutex;

    /// 暂存当前需要渲染的最新视频帧
    std::shared_ptr<VideoFrame> m_currentFrame{nullptr};

    /// OpenGL 纹理句柄标识
    GLuint m_textureId{0};
};
} // namespace av
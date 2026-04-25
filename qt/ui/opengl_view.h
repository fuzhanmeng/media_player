/**
 * @file opengl_view
 * @brief 自定义视频渲染视图,专门用于接管底层解码后推上来的视频帧（YUV 或 RGB），并使用自定义 Shader 进行硬件加速渲染
 */
#pragma once

#include <QOpenGLFunctions_3_3_Core> // 提供了现代 OpenGL 3.3 核心模式的所有函数声明
#include <QOpenGLWidget> // 提供了一个可以直接执行原生 OpenGL API 命令的 Qt 窗口容器

namespace av {
/**
 * @brief 自定义视频渲染视图
 * 继承自 QOpenGLWidget，提供基于 OpenGL 3.3 Core Profile 的自定义渲染环境。
 */
class OpenGLView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit OpenGLView(QWidget* parent = nullptr);

    ~OpenGLView() override;
    /**
     * @brief 获取底层渲染句柄
     * 供 IPlayer::AttachDisplayView 使用，将播放器画面重定向到此画布。
     * @return void* 视图的底层句柄或标识符
     */
    void* GetVideoDisplayView();

protected:
    // 初始化显卡状态
    void initializeGL() override;

    // 当用户拖拽改变窗口大小时，告诉显卡新的绘制区域有多大
    void resizeGL(int width, int high) override;

    // 调用 OpenGL API，把画面-画出来
    void paintGL() override;
};
} // namespace av
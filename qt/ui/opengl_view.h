/**
 * @file opengl_view.h
 * @brief 自定义视频渲染视图
 * @details 专门用于接管底层解码后推上来的视频帧（YUV420P），并使用自定义 Shader 进行 GPU 硬件加速渲染。
 * 融合了 Qt 窗口层、现代 OpenGL 核心 API 以及业务解耦接收器。
 */
#pragma once

#include <QOpenGLFunctions_3_3_Core> // 现代 OpenGL 3.3 核心模式 API
#include <QOpenGLShaderProgram> // Qt 封装的着色器管理工具
#include <QOpenGLWidget> // Qt 原生 OpenGL 渲染容器
#include <memory>
#include <mutex>

#include "ivideo_sink.h"
#include "video_frame.h"

namespace av {

/**
 * @class OpenGLView
 * @brief 视频硬件渲染器
 * 1. QOpenGLWidget: 提供 Qt 窗口环境与双缓冲刷新机制
 * 2. QOpenGLFunctions_3_3_Core: 提供无固定管线的现代底层 OpenGL API
 * 3. IVideoSink: 实现底层核心引擎的数据接收协议，彻底解耦业务与 UI
 */
class OpenGLView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core, public IVideoSink {
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param parent 父窗口指针
     */
    explicit OpenGLView(QWidget* parent = nullptr);

    /**
     * @brief 析构函数
     * @details 负责销毁显存中的纹理、顶点缓冲等物理资源，防止显存泄漏
     */
    ~OpenGLView() override;

    // ================== IVideoSink 接口实现 ==================

    /**
     * @brief 接收底层投递上来的视频帧 (单帧覆盖机制)
     * @param video_frame 包含纯净 YUV420P 像素数据的智能指针
     * @note 该函数由底层后台解码子线程极速触发，采用 O(1) 的指针移动并向主线程派发重绘事件
     */
    void OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) override;

protected:
    // ================== QOpenGLWidget 核心渲染管线 ==================

    /**
     * @brief 初始化 OpenGL 渲染环境 (生命周期内仅调用一次)
     * @details 负责加载 API 指针、编译着色器、搭建几何幕布、预分配纹理句柄
     */
    void initializeGL() override;

    /**
     * @brief 视口尺寸调整 (窗口大小改变时自动触发)
     * @param width 新的物理宽度
     * @param height 新的物理高度
     */
    void resizeGL(int width, int height) override;

    /**
     * @brief 核心渲染循环 (主线程屏幕刷新时自动触发)
     * @details 将最新的 YUV 内存数据推入显存，调用 Shader 矩阵换算并绘制到屏幕
     */
    void paintGL() override;

private:
    // ================== 内部辅助函数 ==================

    /**
     * @brief 构建并编译 GPU 着色器程序 (Shader)
     */
    void BuildShader();

    /**
     * @brief 分配并初始化顶点与纹理坐标数据 (VAO/VBO/EBO)
     */
    void SetupGeometry();

private:
    // ================== 跨线程共享数据区 ==================

    /// 互斥锁：保护跨线程的视频帧指针交接
    std::mutex m_frameMutex;

    /// 暂存区：当前需要被 GPU 渲染的最新的视频帧 (Mailbox 信箱模式)
    std::shared_ptr<VideoFrame> m_currentFrame{nullptr};

    // ================== GPU 物理渲染资源 ==================

    /// 着色器程序：负责管理运行在 GPU 上的微型程序
    QOpenGLShaderProgram m_shaderProgram;

    /// OpenGL 纹理句柄数组：分别对应 Y, U, V 三个颜色物理分量
    GLuint m_textureIds[3]{0, 0, 0};

    /// OpenGL 顶点数组对象 (记录了如何解析顶点缓冲的配置)
    GLuint m_vao{0};
    /// OpenGL 顶点缓冲对象 (存放实际的顶点坐标和纹理坐标内存)
    GLuint m_vbo{0};
    /// OpenGL 索引缓冲对象 (告诉 GPU 怎么用 4 个点画出 2 个三角形来拼成一个矩形)
    GLuint m_ebo{0};
};

} // namespace av
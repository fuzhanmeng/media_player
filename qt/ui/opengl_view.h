/**
 * @file opengl_view.h
 * @brief 自定义视频渲染视图
 * @details 用于接管底层解码后推上来的视频帧（YUV420P），并使用自定义 Shader 进行 GPU 硬件加速渲染
 *   CPU内存(YUV420P) → glTexSubImage2D → GPU显存(3个纹理) → Fragment Shader(YUV→RGB矩阵运算) → 帧缓冲 → 屏幕
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

    /**
     * @brief 接收底层投递上来的视频帧（信箱模式，新帧覆盖旧帧）
     * @param video_frame 包含纯净 YUV420P 像素数据的智能指针
     * @note 该函数由底层后台解码子线程极速触发，内部用 mutex 保护，通过 update() 投递 Paint 事件到主线程
     */
    void OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) override;

protected:
    /**
     * @brief 初始化 OpenGL 渲染环境
     * @details 负责加载 API 指针、编译着色器、搭建几何幕布、预分配纹理句柄
     */
    void initializeGL() override;

    /**
     * @brief 视口尺寸调整 (窗口大小改变时自动触发)
     * @param width 新的物理宽度
     * @param height 新的物理高度
     * @param 更新 OpenGL 视口（Viewport），告知 GPU 绘图大小，实现 NDC 坐标（-1~1）到屏幕像素坐标的正确映射
     */
    void resizeGL(int width, int height) override;

    /**
     * @brief 渲染循环
     * @details 将最新 YUV 帧上传到 GPU 纹理，触发 Shader 绘制到帧缓冲
     */
    void paintGL() override;

private:
    // ================== 内部辅助函数 ==================

    /**
     * @brief 构建并编译 GPU 着色器程序 (Shader)
     */
    void BuildShader();

    /**
     * @brief  上传全屏矩形的顶点数据到 GPU (VAO/VBO/EBO)
     */
    void SetupGeometry();

private:
    // ================== 跨线程共享数据区 ==================

    /// 互斥锁：保护跨线程的视频帧指针交接
    std::mutex m_frameMutex;

    /// 暂存区：当前需要被 GPU 渲染的最新的视频帧 (Mailbox 信箱模式，只保留最新一帧)
    std::shared_ptr<VideoFrame> m_currentFrame{nullptr};

    // ================== GPU 物理渲染资源 ==================

    /// 着色器程序：（Vertex Shader + Fragment Shader 链接后的整体）
    QOpenGLShaderProgram m_shaderProgram;

    /// OpenGL 纹理句柄数组：分别对应 Y, U, V 三个颜色物理分量
    GLuint m_textureIds[3]{0, 0, 0};

    /// OpenGL 顶点数组对象 (记录了如何解析顶点缓冲的配置)
    GLuint m_vao{0};
    /// OpenGL 顶点缓冲对象 (存放实际的顶点坐标和纹理坐标内存)
    GLuint m_vbo{0};
    /// OpenGL 索引缓冲对象 (告诉 GPU 怎么用 4 个点画出 2 个三角形来拼成一个矩形)
    GLuint m_ebo{0};

    /// 纹理显存是否已通过 glTexImage2D 完成初次分配
    /// false = 尚未分配，需要调用 glTexImage2D（分配+上传）
    /// true  = 已分配，后续帧用 glTexSubImage2D（只上传，不重新分配）
    bool m_textureInitialized{false};

    /// 上一帧的视频宽度（像素）
    /// 用于检测分辨率变化，变化时需重新调用 glTexImage2D 重新分配
    int m_lastVideoWidth{0};

    /// 上一帧的视频高度（像素）
    int m_lastVideoHeight{0};
};

} // namespace av
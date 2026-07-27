/**
 * @file opengl_view.h
 * @brief 视频渲染画布：实现 IVideoSink，接收 YUV420P 帧并用 OpenGL 上传三纹理、着色器 YUV→RGB 渲染
 * @details 数据流：后台线程 OnReceiveVideoFrame -> 信箱暂存最新帧 -> 跨线程投递重绘 -> GUI 线程
            paintGL 上传纹理 + 着色器绘制
 * @note 跨线程要点：OnReceiveVideoFrame 运行在生产者后台， 而 OpenGL 上下文 绑定在 UI 线程
 */
#pragma once

/// Qt 对 OpenGL 的封装
#include <QOpenGLBuffer> /// GPU 显存缓冲的封装（VBO/EBO）
#include <QOpenGLFunctions_3_3_Core> /// OpenGL 3.3 核心版的全部函数入口
#include <QOpenGLShaderProgram> /// 着色器程序的 RAII 封装
#include <QOpenGLVertexArrayObject> /// 顶点属性布局的封装，记录"顶点数据怎么解析"
#include <QOpenGLWidget> /// 能渲染 OpenGL 的窗口控件
#include <memory>
#include <mutex>

#include "mediacore/ivideo_sink.h"

namespace mediacore {
struct VideoFrame;
}

/**
 * @class OpenGLView
 * @brief 视频硬件渲染器 YUV420P -> OpenGL纹理 -> 着色器 -> 屏幕
 * @details 多继承三个角色
            1. QOpenGLWidget             Qt 窗口环境与双缓冲刷新
            2. OpenGLFunctions_3_3_Core  现代 OpenGL 3.3 Core API
            3. mediacore::IVideoSink     底层帧的接收协议，彻底解耦 "谁生产帧"
 */
class OpenGLView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core, public mediacore::IVideoSink {
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param parent 父窗口指针,Qt 父子对象树管理生命周期
     */
    explicit OpenGLView(QWidget* parent = nullptr);

    /**
     * @brief 析构函数
     * @note 需在 GL 上下文有效时释放纹理 / VBO 等显存资源，避免显存泄漏
     */
    ~OpenGLView() override;

    /**
     * @brief 接收一帧待显示的视频帧 YUV420P
     * @param VideoFrame 共享所有权的 YUV420P 帧
     * @note 由 后台生产者线程调用：锁内存入信箱，再发 FrameArrived信号 Qt::QueuedConnection
             投递到 GUI 线程触发重绘，本函数自身不会碰任何 GL 调用
     */
    void OnReceiveVideoFrame(std::shared_ptr<mediacore::VideoFrame> video_frame) override;

signals:
    /**
     * @brief 新的视频帧存入信箱， 请求 GUI 线程重绘
     * @note 仅作跨线程投递用：连接到自身的 update(),以 Qt::QueueConnection 保证在 GUI 线程执行
     */
    void FrameArrived();

protected:
    /**
     * @brief 初始化 GL 上下文:加载函数指针、编译着色器、建立全屏矩形几何、预分配纹理句柄
     */
    void initializeGL() override;

    /**
     * @brief 窗口大小调整：窗口大小变化时更新 glViewport
     * @param width 新的物理宽度
     * @param height 新的物理高度
     */
    void resizeGL(int width, int height) override;

    /**
     * @brief 渲染一帧：从信箱取最新帧，上传三平面纹理，着色器绘制到屏幕，宁可丢帧，不要延迟
     */
    void paintGL() override;

private:
    /**
     * @brief 编译链接 YUV -> RGB 的顶点 + 片段着色器
     */
    void BuildShader();

    /**
     * @brief 上传全屏矩形顶点（位置 + 纹理坐标） 到 VAO/VBO/EBO
     */
    void SetupGeometry();

    /**
     * @brief 创建 3 个纹理句柄并设置采样/环绕参数 （Y U V 各一张 R8 单通道纹理）
     */
    void SetupTextures();

    /**
     * @brief 将当前帧的 Y/U/V 三平面分别上传到对应纹理
     * @param frame 待上传的有效数据帧（调用方已确保非空且 IsValid）
     * @note 分辨率变化时用 glTexImage2D 重新分哦，否则使用 glTexSubImage2D 仅更新，省显存操作
     */
    void UploadFrameTextures(const mediacore::VideoFrame& frame);

private:
    // ================= 跨线程共享数据（信箱）=================

    /// 保护 currentFrame_ 的跨线程交接，后台线程写入 / GUI 线程 paintGL 读取
    std::mutex frameMutex_;

    /// 信箱：仅保留最新的一帧，旧帧会被新帧覆盖
    std::shared_ptr<mediacore::VideoFrame> currentFrame_{nullptr};

    // ================= GPU 渲染资源 =================

    /// 着色器程序:顶点 + 片段链接后的整体
    QOpenGLShaderProgram shaderProgram_;

    /// Y/U/V 三平面对应的纹理句柄
    GLuint textureIds_[3]{0, 0, 0};

    /// 顶点数组对象：记录顶点属性布局
    QOpenGLVertexArrayObject vao_;

    /// 顶点缓冲对象：全屏矩形顶点数据
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};

    /// 索引缓冲对象：两个三角形拼成矩形
    QOpenGLBuffer ebo_{QOpenGLBuffer::IndexBuffer};

    /// 纹理显存是否已经首次分配： false 时用 glTexImage2D 分配, true 时使用 glTexSubImage2D 更新
    bool textureAllocated_{false};

    /// 上一帧分辨率，用于检测尺寸变化触发纹理重新分配
    int lastFrameWidth_{0};
    int lastFrameHeight_{0};
};

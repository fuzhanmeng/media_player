/**
 * @file opengl_view.h
 * @brief 视频渲染画布：实现 IVideoSink，接收 YUV420P 帧并用 OpenGL 上传三纹理、着色器 YUV→RGB 渲染
 * @details
 * 数据流：后台线程调用 OnReceiveVideoFrame -> 帧写入信箱 -> 发出跨线程信号请求重绘 -> GUI 线程
 * paintGL 中读取信箱、上传纹理、执行着色器绘制
 * @note
 * OnReceiveVideoFrame 运行在生产者所在的后台线程，而所有 GL 调用只能在持有 OpenGL 上下文的GUI 线程执
 * 行，两者之间靠 Qt::QueuedConnection 信号完成线程切换
 */
#pragma once

/// Qt 对 OpenGL 的封装
#include <QOpenGLBuffer>  /// GPU 显存缓冲的封装（VBO/EBO）
#include <QOpenGLFunctions_3_3_Core>  /// OpenGL 3.3 核心版的全部函数入口
#include <QOpenGLShaderProgram>  /// 着色器程序的 RAII 封装
#include <QOpenGLVertexArrayObject>  /// 顶点属性布局的封装，记录顶点数据如何解析
#include <QOpenGLWidget>  /// 能渲染 OpenGL 的窗口控件
#include <memory>
#include <mutex>

#include "mediacore/ivideo_sink.h"

namespace mediacore {
struct VideoFrame;
}

/**
 * @class OpenGLView
 * @brief 视频硬件渲染器 YUV420P -> OpenGL纹理 -> 着色器 -> 屏幕
 * @details
 * 多继承三个角色：
 * 1. QOpenGLWidget：提供 Qt 窗口环境与双缓冲刷新
 * 2. QOpenGLFunctions_3_3_Core：提供 OpenGL 3.3 Core 版本的函数入口
 * 3. mediacore::IVideoSink：帧接收协议，使本类无需知道帧的生产者是谁
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
     * @note 需在 GL 上下文有效时释放纹理等显存资源，否则调用无上下文可作用，导致显存泄漏
     */
    ~OpenGLView() override;

    /**
     * @brief 接收一帧待显示的 YUV420P 视频帧
     * @param VideoFrame 共享所有权的视频帧
     * @note
     * 由后台生产者线程调用：加锁写入信箱后发出 FrameArrived 信号，信号以 Qt::QueuedConnection
     * 方式投递到 GUI 线程触发重绘，本函数自身不涉及任何 GL 调用
     */
    void OnReceiveVideoFrame(std::shared_ptr<mediacore::VideoFrame> video_frame) override;

signals:
    /**
     * @brief 通知有新的一帧已存入信箱，请求 GUI 线程重绘
     * @note 仅用于跨线程投递：本信号连接到自身的 update()，连接类型为 Qt::QueuedConnection
     * 确保 update() 总是在 GUI 线程执行，而不论 emit 发生在哪个线程
     */
    void FrameArrived();

protected:
    /**
     * @brief 初始化 GL 上下文：加载函数指针、编译着色器、建立全屏矩形几何、预分配纹理句柄
     */
    void initializeGL() override;

    /**
     * @brief 窗口大小调整：窗口大小变化时更新 glViewport
     * @param width 新的物理宽度
     * @param height 新的物理高度
     */
    void resizeGL(int width, int height) override;

    /**
     * @brief 渲染一帧：从信箱取最新帧，上传三平面纹理，用着色器绘制到屏幕
     */
    void paintGL() override;

private:
    /**
     * @brief 编译链接 YUV -> RGB 的顶点着色器与片段着色器
     * @return true 表示着色器编译链接成功，false 表示失败
     */
    bool BuildShader();

    /**
     * @brief 上传全屏矩形顶点（位置 + 纹理坐标） 到 VAO/VBO/EBO
     */
    void SetupGeometry();

    /**
     * @brief 创建 3 个纹理句柄并设置采样、环绕参数（Y/U/V 各一张 R8 单通道纹理）
     */
    void SetupTextures();

    /**
     * @brief 将当前帧的 Y/U/V 三平面分别上传到对应纹理
     * @param frame 待上传的有效数据帧（调用方已确保非空且 IsValid）
     * @note 分辨率发生变化时用 glTexImage2D 重新分配显存，否则用 glTexSubImage2D 仅更新内容
     */
    void UploadFrameTextures(const mediacore::VideoFrame& frame);

private:
    /// 保护 current_frame_ 的跨线程访问：后台线程写入，GUI 线程 paintGL 读取
    std::mutex frame_mutex_;

    /// 信箱：仅保留最新的一帧，旧帧会被新帧覆盖
    std::shared_ptr<mediacore::VideoFrame> current_frame_{nullptr};

    /// 着色器程序：顶点着色器与片段着色器链接后的整体
    QOpenGLShaderProgram shader_program_;

    /// Y/U/V 三平面对应的纹理句柄
    std::array<GLuint, 3> texture_ids_{0, 0, 0};

    /// 顶点数组对象：记录顶点属性布局
    QOpenGLVertexArrayObject vao_;

    /// 顶点缓冲对象：全屏矩形顶点数据
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};

    /// 索引缓冲对象：两个三角形拼成矩形所用的索引数据
    QOpenGLBuffer ebo_{QOpenGLBuffer::IndexBuffer};

    /// 纹理显存是否已首次分配：false 时用 glTexImage2D 分配，true 时用 glTexSubImage2D 更新
    bool texture_allocated_{false};

    /// 着色器程序是否已成功编译链接
    bool shader_ready_{false};

    /// 上一帧的分辨率，用于检测尺寸变化以触发纹理重新分配
    int last_frame_width_{0};
    int last_frame_height_{0};
};

/**
 * @file opengl_view.h
 * @brief 使用 OpenGL 硬件加速渲染 YUV420P 视频帧的 Qt 窗口控件
 * @details
 * 负责打通工作线程与 GUI 线程的图像渲染边界：
 * 1. 架构定位：OpenGLView 实现 mediacore::IVideoSink 抽象接口，作为视频解码管线的最终呈现端；
 * 2. 跨线程信箱机制：后台 VideoSinkNode 工作线程通过 OnReceiveVideoFrame 将新帧写入互斥锁保护的信箱，
 *    并通过 Qt::QueuedConnection 发出 FrameArrived 信号触发 GUI 线程的 update()；
 * 3. 硬件色彩空间转换：GUI 线程在 paintGL 中读取信箱，将 Y/U/V 三个单通道（GL_R8）纹理上传至 GPU，
 *    在片段着色器（Fragment Shader）中完成矩阵级 $YUV \rightarrow RGB$ 色彩转换并最终呈现在屏幕上。
 */

#pragma once

#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <array>
#include <memory>
#include <mutex>

#include "mediacore/ivideo_sink.h"

namespace mediacore {
struct VideoFrame;
}

/**
 * @class OpenGLView
 * @brief 接收并显示解码视频图像的 OpenGL 3.3 视窗控件
 * @details
 * 采用多继承承担三个维度的核心职责：
 * 1. QOpenGLWidget：嵌入 Qt 控件树，提供原生窗口环境、帧缓冲区管理与双缓冲自动交换（SwapBuffers）；
 * 2. QOpenGLFunctions_3_3_Core：提供标准的 OpenGL 3.3 Core Profile 硬件加速绘图 API 入口；
 * 3. mediacore::IVideoSink：抽象消费接口，使渲染组件无需感知视频帧的具体生产者（如软解/硬解）。
 */
class OpenGLView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core, public mediacore::IVideoSink {
    Q_OBJECT

public:
    /**
     * @brief 构造函数：初始化 OpenGL 视窗控件
     * @param parent Qt 父控件指针，由父子对象树统一管理本控件生命周期
     */
    explicit OpenGLView(QWidget* parent = nullptr);

    /**
     * @brief 析构函数：释放 OpenGL 显存资源
     * @note 析构时必须在有效的 OpenGL 上下文中释放纹理、缓冲与着色器句柄。
     */
    ~OpenGLView() override;

    Q_DISABLE_COPY_MOVE(OpenGLView)

    /**
     * @brief 接收一帧已完成解码并经过格式转换的 YUV420P 视频帧
     * @param video_frame 共享所有权的有效视频图像帧
     * @note
     * 1. 此回调由后台 VideoSinkNode 工作线程同步调用；
     * 2. 内部仅执行轻量的互斥锁置换信箱操作，绝不阻塞工作线程，也不直接调用 OpenGL 绘制指令。
     */
    void OnReceiveVideoFrame(std::shared_ptr<mediacore::VideoFrame> video_frame) override;

signals:
    /**
     * @brief 通知有新视频帧已写入信箱，请求 GUI 主线程调度重绘
     * @note 以 Qt::QueuedConnection 队列化方式跨线程投递，确保 update() 严格在 GUI 线程执行。
     */
    void FrameArrived();

protected:
    /**
     * @brief 初始化 OpenGL 渲染上下文
     * @details
     * 在首帧渲染前被 Qt 自动调用：
     * 1. 加载 Core Profile 函数指针；
     * 2. 编译并链接 $YUV \rightarrow RGB$ 矩阵转换着色器程序；
     * 3. 建立全屏四边形几何顶点数据（VAO / VBO / EBO）；
     * 4. 预先生成 Y/U/V 三个纹理对象的句柄并配置线性过滤采样参数。
     */
    void initializeGL() override;

    /**
     * @brief 响应视窗尺寸变更事件
     * @param width 控件新的物理宽度（像素）
     * @param height 控件新的物理高度（像素）
     * @details 根据视窗几何尺寸更新 glViewport，适配自适应画面拉伸。
     */
    void resizeGL(int width, int height) override;

    /**
     * @brief 核心渲染主循环：绘制当前视频画面
     * @details
     * 执行逻辑：
     * 1. 从互斥锁保护的信箱中取出最新持有的视频帧；
     * 2. 校验着色器与帧有效性；
     * 3. 根据分辨率变化使用 glTexImage2D（重分显存）或 glTexSubImage2D（热更新）上传 Y/U/V 三平面纹理；
     * 4. 绑定纹理单元至采样器，运行着色器绘制全屏网格。
     */
    void paintGL() override;

private:
    /**
     * @brief 编译并链接 YUV 至 RGB 色彩空间转换着色器程序
     * @return 编译链接成功返回 true，发生语法或链接错误返回 false
     */
    bool BuildShader();

    /**
     * @brief 初始化全屏四边形几何图元数据
     * @details 将标准化设备坐标（NDC）空间下的位置及纹理 UV 坐标上传至 GPU（VBO / EBO / VAO）。
     */
    void SetupGeometry();

    /**
     * @brief 生成 3 个纹理句柄并配置采样过滤与寻址环绕参数
     * @details 创建 Y/U/V 各自对应的 GL_TEXTURE_2D
     * 纹理对象，配置双线性插值（GL_LINEAR）与边缘截断（GL_CLAMP_TO_EDGE）。
     */
    void SetupTextures();

    /**
     * @brief 将当前帧的 Y/U/V 三平面像素数据上传至 GPU 显存
     * @param frame 待上传的有效图像帧（已校验 IsValid 为 true）
     * @note 仅在视频宽高发生改变时使用 glTexImage2D 重新分配显存；尺寸未变时使用 glTexSubImage2D 复用显存直接覆盖。
     */
    void UploadFrameTextures(const mediacore::VideoFrame& frame);

private:
    std::mutex frame_mutex_;  ///< 互斥锁：保护 current_frame_ 信箱的跨线程读写访问
    std::shared_ptr<mediacore::VideoFrame> current_frame_{nullptr};  ///< 视频帧信箱：始终仅保留最新到达的一帧画面
    QOpenGLShaderProgram shader_program_;  ///< 着色器程序对象（承载 YUV 转 RGB 片段着色器）
    std::array<GLuint, 3> texture_ids_{0, 0, 0};  ///< Y/U/V 三个平面的 OpenGL 2D 纹理句柄（0: Y, 1: U, 2: V）
    QOpenGLVertexArrayObject vao_;  ///< 顶点数组对象（VAO），保存顶点属性解析格式
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};  ///< 顶点缓冲对象（VBO），存储网格顶点与纹理坐标
    QOpenGLBuffer ebo_{QOpenGLBuffer::IndexBuffer};  ///< 索引缓冲对象（EBO），存储三角形顶点绘制索引
    bool texture_allocated_{false};  ///< 标记纹理显存是否已初次分配
    bool shader_ready_{false};  ///< 标记着色器程序是否已成功通过编译与链接
    int last_frame_width_{0};  ///< 记录上一帧宽度，用于检测视频分辨率动态重协商
    int last_frame_height_{0};  ///< 记录上一帧高度，用于检测视频分辨率动态重协商
};
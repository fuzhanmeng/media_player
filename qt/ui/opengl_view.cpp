/**
 * @file opengl_view.cpp
 * @brief 自定义视频渲染视图实现
 */
#include "opengl_view.h"

#include "logger.h"

namespace av {

// ================== Shader 源码区 ==================

/**
 * @brief 顶点着色器 (Vertex Shader)
 * @details 负责计算图像在屏幕上的位置。
 * 由于图像解码后的内存排列通常是 Left-Top (左上角为基准)，而 OpenGL 的纹理坐标系是 Left-Bottom (左下角为基准)，
 * 此处必须对 Y 轴的纹理坐标进行 1.0 - y 的垂直翻转，否则画面会上下颠倒
 */
static const char* kVertexShaderSource = R"(
    #version 330 core
    layout (location = 0) in vec3 aPos;      // 顶点坐标
    layout (location = 1) in vec2 aTexCoord; // 纹理坐标

    out vec2 TexCoord; // 传递给片段着色器的变量

    void main() {
        gl_Position = vec4(aPos, 1.0);
        // 执行 Y 轴翻转
        TexCoord = vec2(aTexCoord.x, 1.0 - aTexCoord.y); 
    }
)";

/**
 * @brief 片段着色器 (Fragment Shader)
 * @details 负责极其密集的并行色彩计算,利用 GPU 的浮点矩阵运算能力,
 * 将 YUV420P 三通道纹理采样后，依据 BT.601 标准转换矩阵，瞬间输出 RGB 像素给显示器
 */
static const char* kFragmentShaderSource = R"(
    #version 330 core
    in vec2 TexCoord;
    out vec4 FragColor;
    
    // Y, U, V 三个独立的纹理采样器
    uniform sampler2D texY;
    uniform sampler2D texU;
    uniform sampler2D texV;
    
    void main() {
        vec3 yuv;
        vec3 rgb;
        
        // 1. 采样纹理并进行偏移量校准 (OpenGL 采样出来的值归一化在 0.0~1.0)
        // Y 分量原本是 16~235,U/V 是 16~240 (中心点在 128)
        yuv.x = texture(texY, TexCoord).r - 0.0625; // 减去 16/256
        yuv.y = texture(texU, TexCoord).r - 0.5;    // 减去 128/256
        yuv.z = texture(texV, TexCoord).r - 0.5;
        
        // 2. 使用 BT.601 标准转换矩阵进行 YUV 到 RGB 的转换
        rgb = mat3(
            1.164,   1.164,   1.164,
            0.0,    -0.392,   2.017,
            1.596,  -0.813,   0.0
            ) * yuv;
            
            // 3. 输出最终像素颜色
            FragColor = vec4(rgb, 1.0);
            }
            )";

/**
 * @brief 构造函数
 * @param parent 父窗口指针
 */

OpenGLView::OpenGLView(QWidget* parent) : QOpenGLWidget(parent) {
    // 强制此 Widget 接收并处理底层的 Paint 事件，无视任何父级背景透明度
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
}

/**
 * @brief 析构函数
 * @details 负责销毁显存中的纹理、顶点缓冲等物理资源，防止显存泄漏
 */
OpenGLView::~OpenGLView() {
    // 释放 OpenGL 显存资源时，必须确保当前线程持有 OpenGL Context
    makeCurrent();

    // 释放 3 个 YUV 纹理资源
    glDeleteTextures(3, m_textureIds);

    // 释放 几何画板资源 (防止显存泄漏)
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
    glDeleteBuffers(1, &m_ebo);

    doneCurrent();
}

/**
 * @brief 接收底层投递上来的视频帧 (单帧覆盖机制)
 * @param video_frame 包含纯净 YUV420P 像素数据的智能指针
 * @note 该函数由底层后台解码子线程极速触发，采用 O(1) 的指针移动并向主线程派发重绘事件
 */
void OpenGLView::OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    if (!video_frame || !video_frame->buffer_)
        return;

    {
        // 加锁保护共享资源：将底层传递过来的帧数据转交给视图持有
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_currentFrame = std::move(video_frame);
    }

    // 跨线程 UI 刷新通知：
    // update() 是 QWidget 提供的方法，它是绝对线程安全的
    // 它不会立即执行绘制，而是向主线程事件循环投递一个 Paint 事件，由主线程在下一帧调度 paintGL()
    update();
}

/**
 * @brief 初始化 OpenGL 渲染环境 (生命周期内仅调用一次)
 * @details 负责加载 API 指针、编译着色器、搭建几何幕布、预分配纹理句柄
 */
void OpenGLView::initializeGL() {
    // 1. 挂载显卡驱动提供的核心函数指针表
    initializeOpenGLFunctions();

    // 2. 编译并连接 Shader
    BuildShader();

    // 3. 构建渲染幕布(顶点坐标系设置)
    SetupGeometry();

    // 4. 向显卡申请 3 个纹理标识符
    glGenTextures(3, m_textureIds);

    // 5. 绑定纹理的索引
    m_shaderProgram.bind();
    m_shaderProgram.setUniformValue("texY", 0);
    m_shaderProgram.setUniformValue("texU", 1);
    m_shaderProgram.setUniformValue("texV", 2);
    m_shaderProgram.release();

    // 6. 设置清屏默认黑色背景
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    LOG_INFO("OpenGLView GPU pipeline initialized successfully...");
}

/**
 * @brief 视口尺寸调整 (窗口大小改变时自动触发)
 * @param width 新的物理宽度
 * @param height 新的物理高度
 */
void OpenGLView::resizeGL(int width, int height) {
    // 告诉显卡：你的绘图区域铺满整个 Widget
    glViewport(0, 0, width, height);
}

/**
 * @brief 渲染循环
 * @details 将最新的 YUV 内存数据推入显存，调用 Shader 矩阵换算并绘制到屏幕
 */
void OpenGLView::paintGL() {
    // 1. 安全提取当前需要渲染的帧数据
    std::shared_ptr<VideoFrame> frame;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (!m_currentFrame) {
            // 如果还没收到视频帧，用黑色清屏后直接返回
            glClear(GL_COLOR_BUFFER_BIT);
            return;
        }
        frame = m_currentFrame;
    }

    // 2. 清除上一帧的颜色缓冲
    glClear(GL_COLOR_BUFFER_BIT);

    // 3. 激活渲染器和几何幕布
    m_shaderProgram.bind();
    glBindVertexArray(m_vao);

    // 告诉显卡：在从 CPU 内存读取 YUV 字节流到显存时，取消 4 字节的默认对齐！
    // 因为很多视频分辨率的宽度不是 4 的倍数，不加这句画面会严重花屏倾斜。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 4. 显存上传阶段 (将 YUV 数据分别塞进 3 个纹理中)

    // 上传 Y 分量 (全尺寸)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureIds[0]);

    // 配置纹理拉伸/缩小算法为线性插值 (GL_LINEAR)，边缘限制为贴边 (GL_CLAMP_TO_EDGE)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 执行拷贝：内存 -> 显存
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame->width_, frame->height_, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data_[0]);

    // 上传 U 分量 (宽高为 Y 的二分之一)
    int uv_width = frame->width_ / 2;
    int uv_height = frame->height_ / 2;

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureIds[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data_[1]);

    // 上传 V 分量 (宽高为 Y 的二分之一)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureIds[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data_[2]);

    // 5. 下达渲染命令：用 6 个索引顶点，画出 2 个三角形，拼成一个视频画布！
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

    // 6. 激活并使用指定的配置存档(解除绑定)
    glBindVertexArray(0);
    m_shaderProgram.release();
}

/**
 * @brief 构建并编译 GPU 着色器程序 (Shader)
 */
void OpenGLView::BuildShader() {
    // 编译顶点着色器
    if (!m_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShaderSource)) {
        LOG_ERROR("Vertex Shader compilation failed: {}", m_shaderProgram.log().toStdString());
    }

    // 编译片段着色器
    if (!m_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShaderSource)) {
        LOG_ERROR("Fragment Shader compilation failed: {}", m_shaderProgram.log().toStdString());
    }

    // 链接着色器
    if (!m_shaderProgram.link()) {
        LOG_ERROR("Shader program linking failed: {}", m_shaderProgram.log().toStdString());
    }
}

/**
 * @brief 分配并初始化顶点与纹理坐标数据 (VAO/VBO/EBO)
 */
void OpenGLView::SetupGeometry() {
    // 1. 顶点属性数组：铺满整个标准化屏幕
    // 每行包含 5 个 float：前面 3 个是 X,Y,Z 位置，后面 2 个是 U,V 纹理坐标
    float vertices[] = {
        // 顶点位置 (X, Y, Z)       // 纹理贴图坐标 (U, V)
        1.0f,  1.0f,  0.0f, 1.0f, 1.0f, // 右上角
        1.0f,  -1.0f, 0.0f, 1.0f, 0.0f, // 右下角
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, // 左下角
        -1.0f, 1.0f,  0.0f, 0.0f, 1.0f // 左上角
    };

    // 2. 索引数组：复用上面 4 个顶点，画出 2 个直角三角形，组成一个长方形
    unsigned int indices[] = {
        0, 1, 3, // 第一个三角形 (右上, 右下, 左上)
        1, 2, 3 // 第二个三角形 (右下, 左下, 左上)
    };

    // 3. 向 GPU 申请内存缓冲对象
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    // 4. 开始录制 VAO 的配置状态
    glBindVertexArray(m_vao);

    // 把顶点数组搬运到 GPU 显存
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 把索引数组搬运到 GPU 显存
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // 5. 告诉显卡：如何解析刚刚传过去的这堆 Float 数字
    // 配置位置属性 (Location = 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // 配置纹理坐标属性 (Location = 1)
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // 6. 结束录制，解绑状态
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

} // namespace av
/**
 * @file opengl_view.cpp
 * @brief 自定义视频渲染视图实现

 * YUV420P -> 屏幕的完整流程
 * 1. OnReceiveVideoFrame：音画同步后的视频帧传递过来后，使用此函数进行接收并将视频帧存入m_currentFrame，调用 update()
 * 2. update()： 向主线程事件循环投递Paint事件
 * 3. paintGL： 主线程执行，把YUV数据上传到 3 个 纹理
 * 4. Fragment Shader： GPU 并行对每个像素采样 YUV，做BT.601 矩阵运算输出 RGB
 * 5. Qt 双缓冲：paintGL 返回后自动交换前后缓冲， 画面出现在屏幕上
 */
#include "opengl_view.h"

#include "logger.h"

namespace av {

// ================== Shader 源码区 ==================

/**
 * @brief 顶点着色器 (Vertex Shader)
 * @details 决定"把视频画在屏幕哪里"
 * 由于图像解码后的内存排列通常是 Left-Top (左上角为基准)，而 OpenGL 的纹理坐标系是 Left-Bottom (左下角为基准)，
 * 此处必须对 Y 轴的纹理坐标进行 1.0 - y 的垂直翻转，否则画面会上下颠倒
 */
static const char* kVertexShaderSource = R"(
    #version 330 core
    layout (location = 0) in vec3 aPos;      // 顶点位置坐标 (x, y, z)
    layout (location = 1) in vec2 aTexCoord; // 纹理坐标 (u, v)，范围 0.0~1.0

    out vec2 TexCoord; // 传递给片段着色器的变量

    void main() {
        gl_Position = vec4(aPos, 1.0);
        // 执行 Y 轴翻转：把纹理坐标的 v 分量从"左下角为0"改为"左上角为0"
        TexCoord = vec2(aTexCoord.x, 1.0 - aTexCoord.y); 
    }
)";

/**
 * @brief 片段着色器 (Fragment Shader)，决定"每个像素是什么颜色"
 * @details 负责极其密集的并行色彩计算,利用 GPU 的浮点矩阵运算能力,
 * 将 YUV420P 三通道纹理采样后，依据 BT.601 标准转换矩阵，瞬间输出 RGB 像素给显示器
 *   原始采样值范围：Y: 16~235，U/V: 16~240（中心128）
 *   归一化后（OpenGL 自动除以255）：Y: 0.063~0.922，U/V: 0.063~0.941
 *
 *   偏移校正：
 *     y' = Y - 16/256 = Y - 0.0625
 *     u' = U - 128/256 = U - 0.5
 *     v' = V - 128/256 = V - 0.5
 *
 *   矩阵运算（BT.601）：
 *     R = 1.164*y'            + 1.596*v'
 *     G = 1.164*y' - 0.392*u' - 0.813*v'
 *     B = 1.164*y' + 2.017*u'

 */
static const char* kFragmentShaderSource = R"(
    #version 330 core
    in vec2 TexCoord;
    out vec4 FragColor;
    
    // Y, U, V 三个独立的纹理采样器
    uniform sampler2D texY;  // 纹理单元 0：Y 亮度分量
    uniform sampler2D texU;  // 纹理单元 1：U 色差分量（Cb）
    uniform sampler2D texV;  // 纹理单元 2：V 色差分量（Cr）
    
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
            1.164,  1.164,  1.164,   // Y 列：R/G/B 都从 Y 取亮度基底
            0.0,   -0.392,  2.017,   // U 列：影响 G 和 B
            1.596, -0.813,  0.0      // V 列：影响 R 和 G
            ) * yuv;
            
            // 3. 输出 RGBA，透明度固定为 1.0（不透明）
            FragColor = vec4(rgb, 1.0);
    }
)";

/**
 * @brief 构造函数
 * @param parent 父窗口指针
 */

OpenGLView::OpenGLView(QWidget* parent) : QOpenGLWidget(parent) {
    // 强制此 Widget 自己处理绘制，不透传给父级背景

    // WA_OpaquePaintEvent：通知 Qt 这个控件会自己绘制全部像素，不需要父级先画背景
    setAttribute(Qt::WA_OpaquePaintEvent);

    // WA_NoSystemBackground：禁止系统在绘制前自动填充背景色（避免闪烁）
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

    //  释放顶点相关的 GPU 资源
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
    glDeleteBuffers(1, &m_ebo);

    // 解绑 Context，归还给 Qt 的 Context 管理系统
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
    // 1. 加载当前平台 OpenGL 3.3 Core Profile 的函数指针，不调用这个，glGenTextures 等函数全是空指针
    initializeOpenGLFunctions();

    // 2. 编译并链接着色器程序
    BuildShader();

    // 3.把全屏矩形的顶点数据上传到 GPU
    SetupGeometry();

    // 4. 向显卡申请 3 个纹理标识符
    glGenTextures(3, m_textureIds);

    // 5. 纹理参数只需设置一次
    for (int i = 0; i < 3; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_textureIds[i]);

        // 缩小过滤：当纹理比屏幕小时（视频分辨率 < 窗口尺寸）用双线性插值
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        // 放大过滤：当纹理比屏幕大时（视频分辨率 > 窗口尺寸）同样用双线性插值
        // GL_LINEAR = 平滑；GL_NEAREST = 锐利但有锯齿（像素风格）
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // 水平方向（S轴）纹理坐标超出 [0,1] 时的处理方式
        // GL_CLAMP_TO_EDGE：超出边界就取边缘像素值，不重复平铺
        // 视频渲染用这个，防止边缘出现重复或黑边
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        // 垂直方向（T轴）同上
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    // 取消纹理绑定，恢复干净状态
    glBindTexture(GL_TEXTURE_2D, 0);

    // 6. 告诉 Shader 程序：texY/texU/texV 分别从哪个纹理单元取数据
    //    数字 0/1/2 对应 GL_TEXTURE0 / GL_TEXTURE1 / GL_TEXTURE2
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
 * @brief 窗口大小变化时 Qt 自动调用
 *
 * Viewport 定义了 NDC 坐标（-1.0 ~ 1.0）到屏幕像素坐标的映射关系：
 *   NDC (-1,-1) → 屏幕像素 (0, 0)           左下角
 *   NDC ( 1, 1) → 屏幕像素 (width, height)  右上角
 *
 * 窗口大小变化后必须更新 Viewport，否则视频会只画在旧尺寸的区域里。
 *
 * 如果想保持视频宽高比（letterbox/pillarbox），
 * 可以在这里计算缩放后的居中区域，把 glViewport 的 x/y 设成非零偏移。
 */
void OpenGLView::resizeGL(int width, int height) {
    // 参数：(左下角x, 左下角y, 宽度, 高度)
    // 铺满整个 Widget
    glViewport(0, 0, width, height);
}

/**
 * @brief 渲染一帧（由主线程事件循环驱动，每次 update() 后触发）
 *
 *   首帧 / 分辨率变化 → glTexImage2D  （在GPU分配显存 + 上传数据）
 *   后续帧            → glTexSubImage2D（直接覆盖写入，无显存重新分配）
 *
 * 性能对比（以 1080p 为例）：
 *   glTexImage2D：每次约 2MB 显存 malloc + 数据传输 ≈ 较高开销
 *   glTexSubImage2D：只做数据传输，无 malloc，开销降低约 30~40%
 */
void OpenGLView::paintGL() {
    // 1. 加锁拿到最新帧（O(1) shared_ptr 引用计数操作，极快）
    std::shared_ptr<VideoFrame> frame;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (!m_currentFrame) {
            // 还没收到任何视频帧，清屏为黑色后返回
            glClear(GL_COLOR_BUFFER_BIT);
            return;
        }
        frame = m_currentFrame; // shared_ptr 拷贝，延长生命周期
    }

    // 2. 清除上一帧的颜色缓冲，准备绘制新帧
    glClear(GL_COLOR_BUFFER_BIT);

    // 3. 激活着色器程序 + 绑定 VAO（VAO 里记录了顶点数据的解析配置）
    m_shaderProgram.bind();
    glBindVertexArray(m_vao);

    // 4. 设置像素解包对齐为 1 字节
    //    OpenGL 默认按 4 字节对齐读取像素数据
    //    但 YUV 数据每行字节数 = width，不一定是 4 的倍数
    //    （例如 1280 宽可以，但 1366 宽就不行）
    //    设为 1 告诉 GPU：每行数据紧密排列，无需跳过对齐填充字节
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // 5. 检测是否需要重新分配显存（首帧 or 分辨率变化）
    //    分辨率变化场景：播放列表中不同分辨率的视频交替播放
    bool need_reinit =
        (!m_textureInitialized || frame->width_ != m_lastVideoWidth || frame->height_ != m_lastVideoHeight);

    if (need_reinit) {
        // 记录新尺寸，下一帧用来判断是否再次变化
        m_lastVideoWidth = frame->width_;
        m_lastVideoHeight = frame->height_;
        // 标记为未初始化，触发下面三个纹理都走 glTexImage2D 分支
        m_textureInitialized = false;
    }

    // UV 分量是 YUV420P 格式，宽高各为 Y 的一半
    const int uv_width = frame->width_ / 2;
    const int uv_height = frame->height_ / 2;

    // ==================== 上传 Y 分量 ====================
    // 激活 0 号纹理单元，对应 Shader 里的 texY
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureIds[0]);

    if (!m_textureInitialized) {
        // glTexImage2D：在 GPU 显存分配空间并上传数据
        // 参数说明：
        //   GL_TEXTURE_2D     — 2D 纹理类型
        //   0                 — mipmap 级别，0 = 原始尺寸（视频不用 mipmap）
        //   GL_RED            — GPU 内部存储格式：单通道，每像素 1 字节
        //   frame->width_     — 纹理宽度（像素数）
        //   frame->height_    — 纹理高度（像素数）
        //   0                 — border，OpenGL 规范要求必须为 0
        //   GL_RED            — CPU 数据格式：单通道
        //   GL_UNSIGNED_BYTE  — CPU 数据类型：每字节 0~255 无符号整数
        //   frame->data_[0]   — CPU 内存数据指针
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, frame->width_, frame->height_, 0, GL_RED, GL_UNSIGNED_BYTE,
                     frame->data_[0]);
    } else {
        // glTexSubImage2D：显存已分配，直接覆盖写入新数据
        // 比 glTexImage2D 少了"内部存储格式"和"border"两个参数
        // 因为显存格式在 glTexImage2D 时已经确定，这里只是更新内容
        // 参数说明：
        //   0, 0              — 更新起始偏移 (x, y)，从左上角开始所以是 0,0
        //                       如果只想更新纹理的某个子区域，这里填偏移量
        //   frame->width_     — 更新区域的宽度
        //   frame->height_    — 更新区域的高度
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width_, frame->height_, GL_RED, GL_UNSIGNED_BYTE,
                        frame->data_[0]);
    }

    // ==================== 上传 U 分量 ====================
    // 激活 1 号纹理单元，对应 Shader 里的 texU
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureIds[1]);

    if (!m_textureInitialized) {
        // U 分量尺寸是 Y 的四分之一（宽/2 × 高/2）
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data_[1]);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, frame->data_[1]);
    }

    // ==================== 上传 V 分量 ====================
    // 激活 2 号纹理单元，对应 Shader 里的 texV
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureIds[2]);

    if (!m_textureInitialized) {
        // V 分量尺寸同 U，也是 Y 的四分之一
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, uv_width, uv_height, 0, GL_RED, GL_UNSIGNED_BYTE, frame->data_[2]);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RED, GL_UNSIGNED_BYTE, frame->data_[2]);
    }

    // 6. 三个纹理全部上传完毕，标记显存已分配
    //    之后的帧走 glTexSubImage2D 快速路径
    if (!m_textureInitialized) {
        m_textureInitialized = true;
    }

    // 7. 下达绘制命令
    //    GPU 根据 EBO 里的 6 个索引，从 VBO 取 4 个顶点，拼成 2 个三角形
    //    覆盖整个屏幕，Fragment Shader 对每个像素采样纹理并输出 RGB
    glDrawElements(GL_TRIANGLES, // 图元类型：三角形
                   6, // 索引数量：2个三角形 × 3个顶点 = 6
                   GL_UNSIGNED_INT, // 索引数据类型
                   nullptr); // 偏移量为 0（从 EBO 起点开始）

    // 8. 解绑，恢复干净状态（良好习惯，防止后续 OpenGL 调用误操作这些对象）
    glBindVertexArray(0);
    m_shaderProgram.release();

    // Qt 会在 paintGL 返回后自动执行双缓冲交换，画面出现在屏幕上
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
 * @brief 创建全屏矩形并上传顶点数据到 GPU (VAO/VBO/EBO)
 * 数据流向： vertices 数组 -> VBO（GPU）显存 -> VAO 记录解析规则
 *
 * 顶点布局（每行 5 个 float）：
 *   [位置 x, 位置 y, 位置 z, 纹理 u, 纹理 v]
 *
 * NDC 坐标范围 -1~1，四个角点铺满整个屏幕：
 *
 *   (-1, 1)────────(1, 1)      ← 屏幕顶部
 *      │    视频画面    │
 *   (-1,-1)────────(1,-1)      ← 屏幕底部
 *
 * EBO 把 4 个顶点分成 2 个三角形：
 *   三角形1：右上(0) + 右下(1) + 左上(3)
 *   三角形2：右下(1) + 左下(2) + 左上(3)
 *   拼在一起 = 一个完整矩形

 */
void OpenGLView::SetupGeometry() {
    float vertices[] = {
        // 位置坐标 (X, Y, Z)      纹理坐标 (U, V)
        1.0f,  1.0f,  0.0f, 1.0f, 1.0f, // 顶点0：右上角
        1.0f,  -1.0f, 0.0f, 1.0f, 0.0f, // 顶点1：右下角
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, // 顶点2：左下角
        -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, // 顶点3：左上角
    };

    unsigned int indices[] = {
        0, 1, 3, // 三角形1：右上 + 右下 + 左上
        1, 2, 3, // 三角形2：右下 + 左下 + 左上
    };

    // 向 GPU 申请对象句柄
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    // 绑定 VAO，开始录制配置
    // 之后的 VBO 绑定、属性指针设置都会被 VAO 记住
    glBindVertexArray(m_vao);

    // 上传顶点数据到 VBO
    // GL_STATIC_DRAW：数据只上传一次，之后只读（顶点坐标不变）
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 上传索引数据到 EBO
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // 告诉 GPU 如何从 VBO 的字节流里解析出"位置属性"（Location=0）
    // 参数：属性索引, 分量数, 数据类型, 是否归一化, 步长(字节), 起始偏移
    // 步长 = 5个float × 4字节 = 20字节（每个顶点占 20 字节）
    // 偏移 = 0（位置数据从每个顶点的第 0 字节开始）
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // 解析"纹理坐标属性"（Location=1）
    // 偏移 = 3个float × 4字节 = 12字节（跳过前面的 xyz 位置数据）
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // 解绑 VBO（VAO 已经记住了配置，VBO 解绑不影响）
    // 注意：EBO 不能在 VAO 绑定期间解绑，否则 VAO 会失去对 EBO 的引用
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}
} // namespace av
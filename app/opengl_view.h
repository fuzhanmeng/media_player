/**
 * @file opengl_view.h
 * @brief 视频渲染
 * @details
 * 负责打通工作线程与 GUI 线程的图像渲染边界：
 * 1. 架构定位：OpenGLView 实现 mediacore::IVideoSink 抽象接口，作为视频解码管线的最终呈现端；
 * 2. 跨线程信箱机制：后台 VideoSinkNode 工作线程通过 OnReceiveVideoFrame 将新帧写入互斥锁保护的信箱，
 *    并通过 Qt::QueuedConnection 发出 FrameArrived 信号触发 GUI 线程的 update()；
 * 3. 两条渲染路径：由 VideoFrame::hardware_ 是否为空决定
 * - 软件帧：GUI 线程在 paintGL 中把 Y/U/V 三个平面（GL_R8）上传为三张纹理，片段着色器完成 YUV -> RGB
 * - 硬件帧：像素始终存储在 GPU 显存中，不经过 CPU，VA-API 解码出的显存 DMA-BUF 通过 EGL 包装成 EGLImage
 * 再绑定成 OpenGL 纹理，由双平面 (NV12/P010) 着色器采样
 * 硬件帧导入或者绘制失败时，把该帧下载到 CPU 再走软件解码路径
 */

#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <QMetaObject>
#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "deferred_resource_queue.h"
#include "mediacore/ivideo_sink.h"

namespace mediacore {
struct VideoFrame;
}

// DMA-BUF 纹理所需的 GL 扩展函数指针类型定义
// QOpenGLFunctions_3_3_Core 不包含这些（它们不属于 OpenGL 3.3 核心）
// 所以需要 glext.h 里的 PFN* 类型声明（例如 glEGLImageTargetTexture2DOES）

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GL/glext.h>

/**
 * @class OpenGLView
 * @brief 接收并显示解码视频图像的 OpenGL 3.3 视窗控件
 * @details
 * 采用多继承承担三个维度的核心职责：
 * 1. QOpenGLWidget：嵌入 Qt 控件树，提供原生窗口环境、帧缓冲区管理与双缓冲自动交换（SwapBuffers）；
 * 2. QOpenGLFunctions_3_3_Core：提供标准的 OpenGL 3.3 Core Profile 硬件加速绘图 API 入口；
 * 3. mediacore::IVideoSink：抽象消费接口，使渲染组件无需感知视频帧的具体生产者（如软解/硬解）
 * 线程约定：
 * · OnReceiveVideoFrame 由 VideoSinkNode 工作线程调用，只操作信箱，不碰任何 GL 状态；
 * · 其余所有 OpenGL / EGL 调用都在 GUI 线程的 initializeGL / paintGL / 析构中完成
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
     * @brief 接收一帧已解码的视频帧（软件帧或硬件帧）
     * @param video_frame 共享所有权的有效视频图像帧
     * @note
     * 1. 此回调由后台 VideoSinkNode 工作线程同步调用；
     * 2. 内部仅执行轻量的互斥锁置换信箱操作，绝不阻塞工作线程，也不直接调用 OpenGL 绘制指令
     * 3. 硬件帧通过 shared_ptr 持有 DMA-BUF fd 与 AVFrame 引用，被新帧覆盖的旧帧如果已经被渲染过，其最后
     * 一份引用会在待释放列表中，要等 GPU 的 fence 完成才会真的释放
     */
    void OnReceiveVideoFrame(std::shared_ptr<mediacore::VideoFrame> video_frame) override;

    /**
     * @brief 接收 Player 注入的硬件渲染标志能力
     * @param capability 与解码器共享的原子布尔标志，标志是否具备硬解码的能力
     * @details
     * 渲染端是唯一的写入源
     * · initializeGL 结束时写入 EGL DMA-BUF 导入是否可用，GL 资源释放时写回 false
     * · 连续多帧硬件渲染失败时写回 false，让解码器不再导出 DMA-BUF
     * 解码器每帧读取该标志，false 时直接把硬件帧下载到 CPU，避免白做导出
     */
    void SetHardwareRenderCapability(const std::shared_ptr<std::atomic<bool>>& capability) override;

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
     * 1. 加载 Core Profile 函数指针，注册上下文销毁通知；
     * 2. 创建与本控件共享资源的离屏上下文（retirement context，用于销毁阶段安全等待 GPU）；
     * 3. 初始化 EGL DMA-BUF 导入能力，并编译硬件帧（双平面）着色器；
     * 4. 编译软件帧（三平面）着色器，建立全屏四边形几何数据，创建 Y/U/V 三张软件帧纹理；
     * 5. 最后发布硬件渲染能力标志（放在最后，保证标志为 true 时所有渲染资源都已就绪）
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
     * 1. 回收已完成 GPU 读取的硬件帧资源；
     * 2. 从互斥锁保护的信箱中取出最新帧，校验有效性，按色彩空间选择转换矩阵；
     * 3. 硬件帧：尝试 EGLImage 零拷贝渲染；失败则下载到 CPU，转为软件帧继续；
     * 4. 软件帧：上传 Y/U/V 三平面纹理，运行三平面着色器绘制全屏四边形
     */
    void paintGL() override;

private:
    // ==================== 软件帧渲染（CPU 上传）====================

    /**
     * @brief 编译并链接软件帧（Y/U/V 三平面）YUV → RGB 着色器程序
     * @return 编译链接成功返回 true，发生语法或链接错误返回 false
     */
    bool BuildSoftwareFrameShader();

    /**
     * @brief 初始化全屏四边形几何图元数据
     * @details
     * 将标准化设备坐标（NDC）空间下的位置及纹理 UV 坐标上传至 GPU（VBO / EBO / VAO）
     * 软件帧与硬件帧共用这一份几何数据
     */
    void CreateFullscreenQuadGeometry();

    /**
     * @brief 生成 3 个纹理句柄并配置采样过滤与寻址环绕参数
     * @details 创建 Y/U/V 各自对应的 GL_TEXTURE_2D
     * 纹理对象，配置双线性插值（GL_LINEAR）与边缘截断（GL_CLAMP_TO_EDGE）
     */
    void CreateSoftwareFrameTextures();

    /**
     * @brief 将软件帧的 Y/U/V 三平面像素数据上传至 GPU 显存
     * @param software_video_frame 待上传的有效软件帧（已校验 IsValid 为 true）
     * @note 仅在视频宽高发生改变时使用 glTexImage2D 重新分配显存；尺寸未变时使用 glTexSubImage2D 复用显存直接覆盖
     */
    void UploadSoftwareFrameTextures(const mediacore::VideoFrame& frame);

    // ==================== 零拷贝硬件渲染（DMA-BUF → EGLImage → 纹理）====================

    /**
     * @brief DMA-BUF -> EGLImage -> OpenGL Texture 所需的 EGL 函数与能力查询结果
     * @details
     * 该结构体相当于 Renderer 的 EGL DMA-BUF 导入工具箱，它不保存视频像素数据，也不持有某一帧的 DMA-BUF fd
     * 主要保存：
     * 1. 当前 EGLDisplay
     * 2. 创建和销毁 EGLImage 的函数入口
     * 3. 查询 DRM FourCC 和 Modifier 支持情况的函数入口
     * 4. 将 EGLImage 关联到 OpenGL Texture 的函数入口
     * 5. 驱动支持导入的格式列表
     * 6. 格式与 Modifier 的查询缓存
     * 7. DMA-BUF 导入功能是否初始化成功的状态
     * @note
     * 该结构体保存的是 EGL 导入能力，不是 EGLImage 本身
     */
    struct EglDmaBufImportFunctions {
        // 当前 Qt OpenGL Context 所属的 EGLDisplay
        // 创建和销毁 EGLImage 时使用对应的 EGLDisplay
        EGLDisplay display_{EGL_NO_DISPLAY};

        // 创建 EGLImage 的函数入口，用于将 DMA-BUF fd 和图像布局信息导入 EGL
        PFNEGLCREATEIMAGEKHRPROC create_image_{nullptr};

        // 销毁 EGLImage 的函数入口，只销毁 EGL 层图像对象，不直接销毁 VA Surface
        PFNEGLDESTROYIMAGEKHRPROC destroy_image_{nullptr};

        // 查询当前 EGLDisplay 支持导入哪些 DRM FourCC 格式
        PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats_{nullptr};

        // 查询某种 DRM FourCC 对应支持哪些 Modifier
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers_{nullptr};

        // OpenGL 扩展函数：
        // glEGLImageTargetTexture2DOES()，将 EGLImage 关联到当前绑定的 OpenGL Texture
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture_{nullptr};

        // 当前 EGLDisplay 声明支持导入的 DRM FourCC 格式列表。
        // 例如 NV12、P010 等。
        //
        // 注意：支持某种 FourCC，不代表它的所有 Modifier 都支持。
        std::vector<EGLint> importable_formats_;

        // 缓存 DRM FourCC 与 Modifier 的组合是否可用。
        //
        // Key:
        //   first  = DRM FourCC。
        //   second = DRM Modifier。
        //
        // Value:
        //   true  = 已确认该组合可以按照项目要求导入并绑定为 GL_TEXTURE_2D。
        //   false = 该组合不支持。
        //
        // 通过缓存避免每次渲染都向驱动重复查询。
        std::map<std::pair<std::uint32_t, std::uint64_t>, bool> format_modifier_support_;

        // 标记 DMA-BUF 导入所需的初始化工作是否已经完成。
        //
        // true 不代表所有 FourCC 和 Modifier 都支持；
        // 每种具体组合仍然需要通过 QueryDmaBufPlaneImportSupport() 检查。
        bool is_available_{false};
    };

    /**
     * @brief 保存一帧正在等待 GPU 完成读取的硬件帧资源
     * @details
     * OpenGL 命令通常是异步执行的，glDrawElements() 返回时，GPU 可能还没有读取完当前的视频图像
     * 因此，不能在提交绘制命令后立即释放当前帧
     * 资源的释放顺序：
     * Fence -> Texture -> EGLImage -> VideoFrame
     * @note
     * GLsync 只表示 OpenGL 命令执行进度，不代表 VA-API 解码进度，VA-API 解码完成需要由 Decoder 侧同步负责
     */
    struct PendingFrameResources {
        // 当前帧对应的 GPU 完成标记
        GLsync fence_{nullptr};

        // 当前帧创建的 EGLImage，NV12/P010 双平面路径分别保存 Y 和 UV 平面的 EGLImage
        std::array<EGLImageKHR, 2> egl_images_{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};

        // 当前帧创建的 OpenGL Texture 对象
        std::array<GLuint, 2> textures_{0, 0};

        // 保留当前视频帧，避免 GPU 读取期间底层资源被提前释放
        std::shared_ptr<mediacore::VideoFrame> video_frame_;

        // Set only after a GL Fence reports completion; a failed wait never sets this flag.
        bool gpu_use_is_complete_{false};
    };

    class DeferredGlFrameBatch;

    /**
     * @brief 初始化 EGL DMA-BUF 导入能力
     * @return 初始化成功返回 true，否则返回 false
     * @details
     * 主要检查
     * 1. 当前的 Qt OpenGL Context 是否使用 EGL 后端
     * 2. 是否支持 EGL DMA-BUF 导入扩展
     * 3. 是否支持需要的 DRM DMA modifier 查询扩展
     * 4. 是否能够获取所需要的 EGL 扩展函数入口
     * 5. 是否能够获取 OpenGL EGLImage 纹理关联函数入口
     * 6. 是否能够查询当前 EGLDisplay 支持的 DRM FourCC 格式
     * 初始化成功后，将函数入口、EGLDisplay 和格式查询结果保存到 egl_import_
     */
    bool InitializeEglDmaBufImport();

    /**
     * @brief 创建与 QOpenGLWidget Context 共享 GL 资源的离屏 Context
     * @param share_context QOpenGLWidget 当前使用的 OpenGL Context
     * @return 创建成功并确认资源共享可用时返回 true
     * @details
     * QOpenGLWidget 的 Context 可能因为控件重新挂载到其他窗口、窗口环境变化等原因被销毁并重新创建
     *
     * 如果原 Context 不再可用，但仍有硬件帧资源需要回收，就需要一个额外的 Context 来执行 Fence 检查和共享 GL 资源清理
     * @note
     * 创建成功还需要确认共享关系确实生效
     */
    bool CreateRetirementContext(QOpenGLContext* share_context);

    /**
     * @brief 查询驱动是否支持某种 DRM FourCC 与 Modifier 组合
     * @param drm_format DRM FourCC 例如：R8 GR88 R16 GR1616
     * @param drm_modifier DMA-BUF 对应的存储布局 Modifier
     * @return 当前 EGLDisplay 支持该组合，并且能够用于 GL_TEXTURE_2D 时返回 true
     */
    bool QueryDmaBufPlaneImportSupport(std::uint32_t drm_format, std::uint64_t drm_modifier);

    /**
     * @brief 编译并链接硬件帧专用 Shader
     * @return 编译和链接成功返回 true，否则返回 false
     * @details
     * 硬件帧使用 NV12/P010 双平面格式，Shader 使用两个 sampler
     * 1. 一个读取 Y 平面纹理
     * 2. 一个读取 UV 平面纹理
     * 随后根据像素位深和色彩转换参数，将 YUV 转换为 RGB
     */
    bool BuildHardwareFrameShader();

    /**
     * @brief 使用 DMA-BUF -> EGLImage -> OpenGL texture 路径渲染一帧硬件视频
     * @param video_frame 包含 HardwareFrame 的视频帧
     * @param color_matrix YUV 转 RGB 使用的 3×3 转换矩阵，按列优先排列
     * @return 渲染成功返回 true，渲染失败返回 false
     */
    bool RenderHardwareFrameWithEglImages(const std::shared_ptr<mediacore::VideoFrame>& video_frame,
                                          const std::array<float, 9>& color_matrix);

    /**
     * @brief 将硬件帧下载到 CPU，转换为软件帧
     * @param hardware_video_frame 输入的硬件视频帧
     * @param software_video_frame 输出的软件视频帧
     * @return 下载成功返回 true，下载失败返回 false
     * @details
     * 当 DMA-BUF 导入或硬件渲染失败时，使用此函数作为回退，将硬件图像下载到 CPU 内存，并转换成项目支持的软件像素格式
     * YUV420P
     */
    bool DownloadHardwareFrameToSoftwareFrame(const mediacore::VideoFrame& hardware_video_frame,
                                             mediacore::VideoFrame& software_video_frame);
    void RecordHardwareFrameDownload(double elapsed_ms, bool succeeded);

    /**
     * @brief 检查并回收已经完成 GPU 读取的硬件帧资源
     * @details
     * 遍历 pending_frame_resources_，检查每一帧对应的 GL Fence
     * Fence 已触发：
     * 说明该 Fence 之前提交的相关 OpenGL 命令已经完成，可以调用 ReleasePendingFrameResources() 释放这一帧的资源
     *
     * Fence 尚未触发：说明 GPU 可能仍在读取当前图像，继续保留这帧及其资源
     *
     * Fence 查询失败：不能直接认为 GPU 已经完成，必须采用安全的等待或资源保留策略
     * @note
     * 该函数不直接销毁 VA Surface
     * 它只负责释放 Renderer 当前持有的 EGL/GL 资源和 VideoFrame 引用
     * VA Surface 是否能够重新使用，最终由 FFmpeg 硬件帧池根据资源引用情况决定
     */
    void ReclaimCompletedFrameResources();

    /**
     * @brief 释放一帧硬件图像占用的资源
     * @param pending_resources 要释放的待回收资源记录
     * @param wait_for_gpu 是否需要先等待 GPU 完成相关命令
     * @details
     * wait_for_gpu == false -> 调用方已经确认 Fence 触发，可以直接释放资源
     * wait_for_gpu == true -> 无法确认 GPU 是否已经完成访问，需要先等待之前提交的 OpenGL 命令
     * @return 只有在确认 GPU 已完成且 EGLImage 已销毁后返回 true
     */
    bool ReleasePendingFrameResources(PendingFrameResources& pending_resources, bool wait_for_gpu);

    /**
     * @brief Transfers unconfirmed frame resources to the process-lifetime GL reaper.
     * @note The reaper keeps the shared Context alive and retries Fence waits on the GUI thread.
     */
    void DeferPendingFrameResources();

    /**
     * @brief 释放当前 OpenGLView 持有的全部 GL/EGL 资源
     * @note
     * GL/EGL 资源操作必须在有效的 Context 和 EGLDisplay 条件下执行。
     */
    void ReleaseOpenGlResources();

private:
    // ================ 软件帧渲染资源 ================

    std::mutex frame_mutex_;  ///< 互斥锁：保护 current_frame_ 信箱的跨线程读写访问
    std::shared_ptr<mediacore::VideoFrame> current_frame_{nullptr};  ///< 视频帧信箱：始终仅保留最新到达的一帧画面
    std::unique_ptr<QOpenGLShaderProgram> shader_program_;  ///< 着色器程序对象（承载 YUV 转 RGB 片段着色器）
    std::array<GLuint, 3> texture_ids_{0, 0, 0};  ///< Y/U/V 三个平面的 OpenGL 2D 纹理句柄（0: Y, 1: U, 2: V）
    QOpenGLVertexArrayObject vao_;  ///< 顶点数组对象（VAO），保存顶点属性解析格式
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};  ///< 顶点缓冲对象（VBO），存储网格顶点与纹理坐标
    QOpenGLBuffer ebo_{QOpenGLBuffer::IndexBuffer};  ///< 索引缓冲对象（EBO），存储三角形顶点绘制索引
    bool texture_allocated_{false};  ///< 标记纹理显存是否已初次分配
    bool shader_ready_{false};  ///< 标记着色器程序是否已成功通过编译与链接
    int last_frame_width_{0};  ///< 记录上一帧宽度，用于检测视频分辨率动态重协商
    int last_frame_height_{0};  ///< 记录上一帧高度，用于检测视频分辨率动态重协商

    // ================ 硬件帧 DMA-BUF / EGLImage / OpenGL 渲染资源 ================

    EglDmaBufImportFunctions egl_import_{};  ///< EGL DMA-BUF 导入能力和函数入口

    std::unique_ptr<QOpenGLContext> retirement_context_;  ///< 与 QOpenGLWidget Context 共享可共享 GL 资源的离屏 Context

    std::unique_ptr<QOffscreenSurface> retirement_surface_;  ///< retirement_context_ 绑定用的离屏 Surface

    QMetaObject::Connection context_destroyed_connection_{};  // 保存 aboutToBeDestroyed 信号的连接句柄

    bool hardware_shader_ready_{false};  ///< 标记硬件帧 Shader 是否已经成功编译并链接

    bool hardware_retirement_suspended_{false};  ///< Fence 状态不可信或回收积压时，改走软件下载路径

    std::unique_ptr<QOpenGLShaderProgram> hardware_shader_program_;  ///< 硬件帧使用的双平面 Shader

    std::vector<std::unique_ptr<PendingFrameResources>> pending_frame_resources_;
    // 保存已经提交 GPU 绘制、但可能尚未完成读取的硬件帧资源

    std::uint64_t renderer_download_count_{0};
    std::uint64_t renderer_download_failures_{0};
    std::uint64_t renderer_download_interval_count_{0};
    std::uint64_t renderer_download_interval_failures_{0};
    double renderer_download_total_ms_{0.0};
    double renderer_download_interval_ms_{0.0};
    double renderer_download_max_ms_{0.0};
    double renderer_download_interval_max_ms_{0.0};
    std::chrono::steady_clock::time_point renderer_statistics_started_at_{};
    std::chrono::steady_clock::time_point renderer_interval_started_at_{};

    // ================ 硬件渲染失败控制 ================

    std::chrono::steady_clock::time_point last_hardware_frame_failure_log_{};  // 最近一次硬件渲染失败日志的时间

    int consecutive_hardware_render_failures_{0};  ///< 连续发生硬件渲染失败的帧数

    // ================ Decoder 与 Renderer 之间的硬件渲染能力协商 ================

    // 与 Decoder 共享的硬件渲染能力标志。
    // 使用 shared_ptr：
    //   Decoder 和 Renderer 可以共同持有同一个状态对象。
    // 使用 atomic<bool>：
    //   允许 Decoder 工作线程和 Renderer GUI 线程安全地读写该状态。
    // Renderer 负责写入：
    //   true  = 当前具备硬件 DMA-BUF 导入和渲染能力。
    //   false = 当前不使用硬件 DMA-BUF 导入路径。
    // Decoder 负责读取：根据该标志决定是否执行 DMA-BUF 导出
    std::shared_ptr<std::atomic<bool>> hardware_render_capability_{};
};

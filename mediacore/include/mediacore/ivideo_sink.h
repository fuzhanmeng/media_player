/**
 * @file ivideo_sink.h
 * @brief 视频帧渲染显示端抽象接口定义
 * @details
 * 作为视频解码管线与具体显示渲染端之间的解耦契约（Observer / Sink 模式）：
 * 1. 生产者解耦：管线内部的 VideoSinkNode 与 VideoSinkDispatcher 仅依赖此纯虚接口，
 *    不感知外部具体的 GUI 框架（Qt、SDL、Metal、D3D 等）或离线录制组件；
 * 2. 帧所有权共享：通过 std::shared_ptr<VideoFrame> 传递视频帧，实现零拷贝跨线程交付；
 * 3. 线程安全边界：回调在渲染工作线程中同步触发，实现端需负责线程切换或无锁队列分发。
 */

#pragma once

#include <atomic>
#include <memory>

namespace mediacore {

class VideoFrame;

/**
 * @class IVideoSink
 * @brief 接收解码后视频帧的显示输出终端接口
 * @note 派生类负责具体的帧数据上传（如 GPU 纹理更新）或界面绘制。
 */
class IVideoSink {
public:
    /**
     * @brief 默认构造函数
     */
    IVideoSink() = default;

    /**
     * @brief 虚析构函数，保证多态派生类的安全释放
     */
    virtual ~IVideoSink() = default;

    // 严禁拷贝与移动，防止抽象接口产生对象切片（Object Slicing）
    IVideoSink(const IVideoSink&) = delete;
    IVideoSink& operator=(const IVideoSink&) = delete;
    IVideoSink(IVideoSink&&) = delete;
    IVideoSink& operator=(IVideoSink&&) = delete;

    /**
     * @brief 接收一帧已完成解码并经过格式转换的视频图像帧
     * @param video_frame 封装有图像尺寸、像素格式（如 YUV420P）与呈现时间戳的共享帧对象（保证非空）
     * @note
     * 1. 执行上下文：此回调在 VideoSinkNode 后台工作线程中被同步调用；
     * 2. 非阻塞契约：实现端应尽快返回（例如将帧推入 UI 队列或触发异步重绘请求），避免阻塞管线消费；
     * 3. 严禁重入：禁止在此回调执行栈中直接同步调用 VideoSinkDispatcher::Attach() 或 Detach()，
     *    否则会触发运行期重入拦截异常
     */
    virtual void OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) = 0;

    /**
     * @brief 注入硬解渲染能力标志，供解码器决定是否导出 DMA-BUF
     * @param capability 布尔原子标志的共享所有权
     *   · true  — 渲染端已确认 EGL DMA-BUF 导入可用，解码器应导出硬件帧
     *   · false — 渲染端尚未初始化或 EGL 不可用，解码器应直接走下载路径
     *   · nullptr — 调用方不关心能力协商，解码器保持自身默认行为不变
     * @details
     * 为什么需要这个机制：
     *
     * 解码器在 BuildPipeline 时就开始工作，而渲染端要到 initializeGL（首帧到来前）才知道 EGL 是否可用
     * 没有这个标志时，解码器默认导出硬件帧，渲染端 EGL 不可用
     * 的情况下每帧都要在 GUI 线程执行 av_hwframe_transfer_data + sws_scale，拖慢界面
     * 正确流程：
     * 1. Player::AttachDisplayView 调用本函数，把 shared_ptr 注入渲染端（OpenGLView）
     * 2. 同一个 shared_ptr 也由 Player::BuildPipeline 注入解码器（FfmpegVideoDecoder）
     * 3. OpenGLView::initializeGL 完成后，根据 InitializeEglDmaBufImport 的结果写入标志
     * 4. 解码器在每帧导出前读取标志；false 时跳过导出，直接走下载转换
     *
     * 默认实现为空，不关心能力协商的实现端无需覆盖
     */
    virtual void SetHardwareRenderCapability(const std::shared_ptr<std::atomic<bool>>& /*capability*/) {}
};

}  // namespace mediacore
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
     *    否则会触发运行期重入拦截异常。
     */
    virtual void OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) = 0;
};

}  // namespace mediacore
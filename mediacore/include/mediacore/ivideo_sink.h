/**
 * @file ivideo_sink.h
 * @brief 视频渲染注入点接口：解耦 "谁生产帧" 与 "谁消费帧"
 * @details UI 层（OpenGLView）与底层（StubPlayer / 真实解码管线）之间的唯一契约
 */

#pragma once

#include <memory>

namespace mediacore {

class VideoFrame;
/**
 * @brief 视频帧的消费接口
 * @details
 * 任何想要接收视频帧并显示/处理的对象都实现此接口，生产者只需要把视频帧推送过来，不关心接收方是谁
 */
class IVideoSink {
public:
    /**
     * @brief 析构函数
     */
    virtual ~IVideoSink() = default;

    /**
     * @brief 接收一帧待显示/处理的视频帧
     * @param video_frame 共享所有权的视频帧，接收方可安全持有以跨调用使用
     */
    virtual void OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) = 0;
};
}  // namespace mediacore
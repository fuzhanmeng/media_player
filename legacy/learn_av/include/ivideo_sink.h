/**
 * @file ivideo_sink.h
 * @brief 视频渲染输出端标准抽象接口
 * @details 通过本接口，核心业务逻辑模块 (Player) 与 UI 渲染实现实现了彻底的解耦
 * 业务模块无需感知具体的渲染实现细节，仅需将 VideoFrame 投递至 Sink
 */

#pragma once
#include <memory>

namespace av {

// 前置声明，减小编译依赖
struct VideoFrame;

/**
 * @class IVideoSink
 * @brief 视频数据接收端接口
 */
class IVideoSink {
public:
    IVideoSink() = default;
    virtual ~IVideoSink() = default;

    /**
     * @brief 响应底层管线投递的视频帧
     * @param video_frame 包含 YUV/RGBA 裸数据及 PTS 时间戳的智能指针
     */
    virtual void OnReceiveVideoFrame(std::shared_ptr<VideoFrame> video_frame) = 0;
};

} // namespace av
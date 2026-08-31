/**
 * @file iplayer.h
 * @brief 播放器的接口，UI 层和 mediacore 内核之间唯一的边界契约
 * @details UI（IPlayerWidget / ControllerWidget）只依赖此接口
 */

#pragma once

#include <cstdint>
#include <string>

#include "playback_state.h"

namespace mediacore {

class IVideoSink;

/**
 * @brief 播放器统一操作契约，定义打开、播放、暂停、停止、跳转、状态查询等 UI 可见的全部能力
 */
class IPlayer {
public:
    /**
     * @brief 虚析构函数
     */
    virtual ~IPlayer() = default;

    /**
     * @brief 绑定视频渲染槽，将底层管线输出的视频帧推送到指定的接收端
     * @param sink 实现了 IVideoSink 接口的视频帧接收器（通常是 UI 层的 OpenGLView）
     */
    virtual void AttachDisplayView(IVideoSink* sink) = 0;

    /**
     * @brief 解绑视频渲染槽，停止向该接收端推送视频帧
     * @param sink 之前通过 AttachDisplayView 绑定的接收器
     */
    virtual void DetachDisplayView(IVideoSink* sink) = 0;

    /**
     * @brief 打开媒体源，异步进入 kOpening 状态
     * @param url 本地文件路径或网络流地址
     */
    virtual void Open(const std::string& url) = 0;

    /**
     * @brief 开始/恢复播放，驱动状态跃迁到 kPlaying
     */
    virtual void Play() = 0;

    /**
     * @brief 暂停播放，驱动状态跃迁到 kPaused
     */
    virtual void Pause() = 0;

    /**
     * @brief 停止播放并拆除底层管线，驱动状态跃迁到 kStopped
     */
    virtual void Stop() = 0;

    /**
     * @brief 跳转到指定时间点
     * @param timestamp_us 跳转的目标时间戳，单位微秒
     */
    virtual void Seek(int64_t timestamp_us) = 0;

    /**
     * @brief 查询当前播放状态
     */
    virtual PlaybackState GetState() const = 0;

private:
    // member variables
};
}  // namespace mediacore

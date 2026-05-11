/**
 * @file idemuxer_listener.h
 * @brief 解封装器事件监听器接口
 */
#pragma once

#include <memory>

extern "C" {
#include <libavformat/avformat.h>
}

namespace av {

class Packet;

/**
 * @brief 解封装器事件监听器接口
 * @details 所有需要接收解封装器事件的类都需要实现这个接口
 */
class IDemuxerListener {
public:
    virtual ~IDemuxerListener() = default;

    /**
     * @brief 通知发现视频流
     * @param stream FFmpeg视频流对象
     */
    virtual void OnNotifyVideoStream(AVStream* stream) = 0;

    /**
     * @brief 通知发现音频流
     * @param stream FFmpeg音频流对象
     */
    virtual void OnNotifyAudioStream(AVStream* stream) = 0;

    /**
     * @brief 通知收到视频数据包
     * @param packet 视频数据包
     */
    virtual void OnNotifyVideoPacket(std::shared_ptr<Packet> packet) = 0;

    /**
     * @brief 通知收到音频数据包
     * @param packet 音频数据包
     */
    virtual void OnNotifyAudioPacket(std::shared_ptr<Packet> packet) = 0;

    /**
     * @brief 通知到达文件末尾
     * @details 当解封装器读完所有数据包时调用此方法
     */
    virtual void OnNotifyEOF() = 0;
};

} // namespace av
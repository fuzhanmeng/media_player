/**
 * @file idemuxer_listener
 * @brief 解复用器数据输出监听接口 (观察者模式)
 */
#pragma once

#include <memory>

struct AVStream;

namespace av {
struct IAVPacket;

class IDemuxerListener {
public:
    virtual ~IDemuxerListener() = default;

    // --- 阶段一：流信息初始化通知 ---
    // 解复用器打开文件后，一旦找到音视频流，立刻通过这两个接口把流信息(包含编解码参数)发给下游
    virtual void OnNotifyVideoStream(AVStream* stream) = 0;
    virtual void OnNotifyAudioStream(AVStream* stream) = 0;

    // --- 阶段二：实时数据包推送 ---
    // 解复用器在后台线程读到压缩数据后，封装成 IAVPacket，通过这两个接口源源不断地推给下游
    virtual void OnNotifyVideoPacket(std::shared_ptr<IAVPacket> packet) = 0;
    virtual void OnNotifyAudioPacket(std::shared_ptr<IAVPacket> packet) = 0;

private:
};
} // namespace av
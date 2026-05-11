/**
 * @file iaudio_decoder.h
 * @brief 音频解码器纯虚接口定义
 */

#pragma once

#include <memory>

struct AVStream;

namespace av {

class Packet;
class Frame;

class IAudioDecoder {
public:
    /**
     * @brief 解码器数据输出的回调监听器 (内部类接口)
     */
    class Listener {
    public:
        virtual ~Listener() = default;
        // 传递解码后的音频帧
        virtual void OnNotifyAudioFrame(std::shared_ptr<Frame> frame) = 0;

        // 通知音频解码完成
        virtual void OnNotifyAudioEOF() = 0; 
    };

    virtual ~IAudioDecoder() = default;

    /**
     * @brief 注册监听器，用于接收解压后的原始音频数据
     */
    virtual void SetListener(Listener* listener) = 0;

    /**
     * @brief 初始化解码器，传入从 Demuxer 获取的音频流信息
     * @return 初始化是否成功
     */
    virtual bool Init(AVStream* stream) = 0;

    /**
     * @brief 接收Demuxer发送过来的压缩数据包
     * @param packet 包含 AAC 等压缩数据的智能指针
     */
    virtual void PushPacket(std::shared_ptr<Packet> packet) = 0;

    /**
     * @brief 通知解码器输入结束，输出缓冲区中剩余的所有帧
     * @details 当解封装器到达文件末尾时调用此方法,调用后解码器会继续输出所有剩余帧，直到返回AVERROR_EOF
     */
    virtual void SendEOF() = 0; 

    /**
     * @brief 启动解码线程
     */
    virtual void Start() = 0;

    /**
     * @brief 暂停解码
     */
    virtual void Pause() = 0;

    /**
     * @brief 停止解码线程并释放相关资源
     */
    virtual void Stop() = 0;

    /**
     * @brief 清洗解码器内部的缓存 (通常在 Seek 跳转时调用)
     */
    virtual void Flush() = 0;

    /**
     * @brief 工厂方法创建一个IAudioDecoder
     */
    static std::unique_ptr<IAudioDecoder> Create();
};

} // namespace av
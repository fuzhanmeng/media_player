/**
 * @file iaudio_encoder.h
 * @brief 音频编码
 */

#pragma once

#include "define/av_frame.h"
#include "define/av_packet.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace av {

class IAudioEncoder {
public:
    /**
     * @brief 监听回调类
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * @brief 将编码后的packet传给FileWriter
         */
        virtual void OnAudioEncoderNotifyPacket(std::shared_ptr<Packet> audio_packet) = 0;

        /**
         * @brief 通知音频编码结束
         */
        virtual void OnAudioEncoderNotifyFinished() = 0;
    };

    /**
     * @brief 创建音频编码器实例
     */
    static std::unique_ptr<IAudioEncoder> Create();

    /**
     * @file iaudio_encoder.h
     * @brief 构造函数
     */
    IAudioEncoder() = default;

    /**
     * @brief 析构函数
     */

    virtual ~IAudioEncoder() = default;

    /**
     * @brief 设置监听回调实例
     */
    virtual void SetListener(Listener* listener) = 0;

    /**
     * @brief 初始化音频编码器，配置编码格式
     */
    virtual bool Init(int sample_rate, int channels, int bit_rate) = 0;

    /**
     * @brief 接收PCM，内部编码，回调listener
     */
    virtual void Encoder(std::shared_ptr<Frame> av_frame) = 0;

    /**
     * @brief 冲洗编码缓冲区
     */
    virtual void Flush() = 0;

    /**
     * @brief 释放资源
     */
    virtual void Close() = 0;

    /**
     * @brief 获取视频编码器上下文
     */
    virtual AVCodecContext* GetCodecContext() const = 0;
};
} // namespace av
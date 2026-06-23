/**
 * @file ivideo_encoder.h
 * @brief 视频编码接口
 */

#pragma once

#include "define/av_frame.h"
#include "define/av_packet.h"
#include "video_frame.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace av {
class IVideoEncoder {
public:
    /**
     * @brief 监听回调类
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * @file ivideo_encoder.h
         * @brief 将编码后的packet传给FileWriter
         */
        virtual void OnVideoEncoderNotifyPacket(std::shared_ptr<Packet> video_packet) = 0;

        /**
         * @brief 通知视频编码结束
         */
        virtual void OnVideoEncoderNotifyFinished() = 0;
    };

    /**
     * @brief 创建视频编码实例
     */
    static std::unique_ptr<IVideoEncoder> Create();

    /**
     * @brief 设置监听回调
     */
    virtual void SetListener(Listener* listener) = 0;

    /**
     * @brief 构造函数
     */
    IVideoEncoder() = default;

    /**
     * @brief 析构函数
     */
    virtual ~IVideoEncoder() = default;

    /**
     * @file ivideo_encoder.h
     * @brief 初始化视频编码器，配置编码格式
     */
    virtual bool Init(int width, int height, int fps, int bit_rate) = 0;

    /**
     * @brief 接收YUV420P，内部编码，回调Listener
     */
    virtual void Encoder(std::shared_ptr<VideoFrame> video_frame) = 0;

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
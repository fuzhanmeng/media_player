/**
 * @file video_encoder.h
 * @brief 视频编码实现
 */
#pragma once

#include <mutex>

#include "encode/interface/ivideo_encoder.h"
#include "video_frame.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace av {

class VideoEncoder : public IVideoEncoder {
public:
    /**
     * @brief 构造函数
     */
    VideoEncoder();

    /**
     * @brief 析构函数
     */
    ~VideoEncoder();

    /**
     * @brief 设置监听回调
     */
    void SetListener(Listener* listener) override;

    /**
     * @brief 初始化视频编码器，配置H.264编码参数
     */
    bool Init(int width, int height, int fps, int bit_rate) override;

    /**
     * @brief 接收YUV420P，编码，回调
     */
    void Encoder(std::shared_ptr<VideoFrame> video_frame) override;

    /**
     * @brief 冲洗编码内部缓冲区
     */
    void Flush() override;

    /**
     * @brief 释放资源
     */
    void Close() override;

    /**
     * @brief 返回视频编码器上下文，供Muxer通过avcodec_parameters_from_context写入MP4文件头
     */
    AVCodecContext* GetCodecContext() const override;

private:
    Listener* listener_{nullptr};

    std::mutex encoder_muxter_;

    /// 编码器上下文
    AVCodecContext* av_encodec_ctx_{nullptr};

    /// 编码PTS计数器，每帧自增1，seek不影响录制时间轴连续性
    int64_t pts_{0};
};
} // namespace av
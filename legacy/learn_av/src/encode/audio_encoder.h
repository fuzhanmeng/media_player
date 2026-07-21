/**
 * @file audio_encoder.h
 * @brief 音频编码实现
 */

#pragma once

#include <mutex>

#include "define/av_frame.h"
#include "encode/interface/iaudio_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
}

namespace av {
class AudioEncoder : public av::IAudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    /**
     * @brief 设置监听回调
     */
    void SetListener(Listener* listener) override;

    /**
     * @brief 初始化音频编码器，配置AAC编码参数
     */
    bool Init(int sample_rate, int channels, int bit_rate) override;

    /**
     * @brief 接收PCM，内部编码，回调listener
     */
    void Encoder(std::shared_ptr<Frame> av_frame) override;

    /**
     * @brief 冲洗编码缓冲区
     */
    void Flush() override;

    /**
     * @brief 释放资源
     */
    void Close() override;

    /**
     * @brief 返回编码器上下文，供Muxer写入文件头时读取编码参数
     */
    AVCodecContext* GetCodecContext() const override;

private:
    Listener* listener_{nullptr};

    std::mutex encoder_muxter_;

    /// 编码器上下文
    AVCodecContext* av_encodec_ctx_{nullptr};

    /// 音频格式转换器
    SwrContext* swr_ctx_{nullptr};

    /// 复用帧容器：Init()分配，每次Encoder()调用覆写数据，Close()释放
    AVFrame* fltp_frame_{nullptr};

    /// 编码PTS计数器，每次Encoder()后累加nb_samples，seek不影响录制时间轴连续性
    int64_t pts_{0};
};
} // namespace av

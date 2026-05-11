/**
 * @file audio_decoder.h
 * @brief 音频解码与重采样模块的实现
 */
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "interface/iaudio_decoder.h"
#include "utils/thread_safe_queue.h"

namespace av {
class Packet;
class Frame;
} // namespace av

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h> // 重采样
}

namespace av {

class AudioDecoder : public IAudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder() override;

    /**
     * @brief 注册监听器，用于接收解压后的原始音频数据
     */
    void SetListener(Listener* listener) override;

    /**
     * @brief 初始化解码器，传入从 Demuxer 获取的音频流信息
     * @return 初始化是否成功
     */
    bool Init(AVStream* stream) override;

    /**
     * @brief 接收Demuxer发送过来的压缩数据包
     * @param packet 包含 AAC 等压缩数据的智能指针
     */
    void PushPacket(std::shared_ptr<Packet> packet) override;

    /**
     * @brief 通知解码器输入结束，输出缓冲区中剩余的所有帧
     * @details 当解封装器到达文件末尾时调用此方法,调用后解码器会继续输出所有剩余帧，直到返回AVERROR_EOF
     */
    void SendEOF() override;

    /**
     * @brief 启动解码线程
     */
    void Start() override;

    /**
     * @brief 暂停解码
     */
    void Pause() override;

    /**
     * @brief 停止解码线程并释放相关资源
     */
    void Stop() override;

    /**
     * @brief 清洗解码器内部的缓存 (通常在 Seek 跳转时调用)
     */
    void Flush() override;

private:
    /**
     * @brief 解码与重采样的后台死循环，运行在 decode_thread_ 中
     */
    void DecodeLoop();

    /**
     * @brief 处理解码后的原始音频帧，执行重采样并分发给下游
     * @param decoded_frame 从解码器上下文中提取出的原始音频帧
     */
    void ProcessAndResample(AVFrame* decoded_frame);

private:
    // 1. ffmpeg
    std::mutex codec_mutex_;
    AVCodecContext* av_codec_ctx_{nullptr};
    SwrContext* swr_ctx_{nullptr};

    // 2. 目标重采样参数
    int out_sample_rate_{44100}; // 目标采样率：44.1kHz
    AVSampleFormat out_sample_fmt_{AV_SAMPLE_FMT_S16}; // 目标采样格式：16位交错 (S16)
    int out_channels_{2}; // 目标声道数：双声道 (立体声)
    AVRational time_base_{0, 1}; // 时间基，记录该音频流的滴答频率

    // 3. 任务队列： 接收来自Demuxer的压缩包 (Packet)
    ThreadSafeQueue<std::shared_ptr<Packet>> packet_queue_;

    // 4. 下游监听器：解好的 Frame 通过这个发给播放/渲染模块
    Listener* audio_decoder_listener_{nullptr};

    // 5. 线程控制
    std::thread decode_thread_;
    std::atomic<bool> is_running_{false};
    std::atomic<bool> is_paused_{false};

    // 新增：专门控制暂停与唤醒的并发工具
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
};

} // namespace av
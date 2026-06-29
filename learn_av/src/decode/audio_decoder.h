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
#include "playback_sync.h"
#include "utils/thread_safe_queue.h"

namespace av {
class Packet;
class Frame;
class StreamInfo;
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
    ~AudioDecoder();

    /**
     * @brief 注册监听回调，解码后的数据传递
     */
    void SetListener(Listener* listener) override;

    /**
     * @brief 初始化解码器，传入从 Demuxer 获取的音频流信息
     * @return 初始化是否成功
     */
    bool Init(const StreamInfo& info) override;

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
    void Start(std::shared_ptr<SyncContext> sync_ctx) override;

    /**
     * @brief 阶段一：下发异步终止信令 --非阻塞
     */
    void RequestStop() override;

    /**
     * @brief 阶段二：同步等待回收线程 --阻塞
     */
    void WaitForStop() override;

    /**
     * @brief 清洗解码器内部的缓存
     */
    void Flush() override;

    /**
     * @brief 清空队列，唤醒阻塞
     */
    void AbortAndClearQueue() override;

    /**
     * @brief 启动队列
     */
    void StartQueue() override;

    /**
     * @brief 获取采样率
     */
    int GetSample() const override;

    /**
     * @brief 获取声道数
     */
    int GetChannels() const override;

private:
    /**
     * @brief 释放 FFmpeg 解码器与重采样上下文资源
     */
    void Close();

    /**
     * @brief 异步解码循环，运行在 decode_thread_ 中
     */
    void ThreadLoop();

    /**
     * @brief 处理解码后的原始音频帧，执行重采样并分发给下游
     * @param av_frame 从解码器上下文中提取出的原始音频帧 (AVFrame)
     */
    void ProcessAndResample(AVFrame* av_frame);

private:
    // 1. FFmpeg 核心上下文与同步锁
    std::mutex codec_mutex_;
    AVCodecContext* av_codec_ctx_{nullptr};
    SwrContext* swr_ctx_{nullptr};

    // 2. 目标重采样参数 (Target Resample Parameters)
    int out_sample_rate_{44100}; ///< 目标采样率：44.1kHz
    AVSampleFormat out_sample_fmt_{AV_SAMPLE_FMT_S16}; ///< 目标采样格式：16位交错 (S16)
    int out_channels_{2}; ///< 目标声道数：双声道 (立体声)
    AVRational time_base_{0, 1}; ///< 时间基，用于计算 PTS

    // 3. 并发数据管道 (Pipeline)
    // 限制队列最大深度为 100（实现阻塞式背压，防止 OOM）
    ThreadSafeQueue<std::shared_ptr<Packet>> packet_queue_{100};

    // 4. 数据下行回调
    Listener* listener_{nullptr};

    // 5. 并发生命周期与状态控制原语
    std::thread decode_thread_;
    std::atomic<bool> is_abort_requested_{true}; ///< 物理线程终止信令，true=线程应退出

    // 5.1 Seek 期间丢帧门闩：置位后丢弃所有帧，直到处理完 kFlush 包才复位
    // 解决"is_seeking_ 已清零但 Flush 包尚未处理"这一时间窗内旧帧漏帧问题
    std::atomic<bool> awaiting_flush_{false};

    // 6. 全局状态同步上下文 (The Source of Truth)
    std::shared_ptr<SyncContext> sync_ctx_{nullptr};
};

} // namespace av
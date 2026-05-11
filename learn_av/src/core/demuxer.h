/**
 * @file demuxer
 * @brief 文件解封装模块，负责将媒体文件拆解为音视频 Packet 流
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "interface/idemuxer.h"

struct AVFormatContext;

namespace av {

class Demuxer : public IDemuxer {
public:
    Demuxer();
    ~Demuxer() override;

    void SetListener(IDemuxerListener* listener) override;
    bool Open(const std::string& url) override;
    void Start() override;
    void Pause() override;
    void Stop() override;
    void SeekTo(float progress) override;
    float GetDuration() override;

private:
    /**
     * @brief 解复用后台线程主循环
     * 只要 Stop() 没被调用，它就会一直在后台跑
     */
    void ThreadLoop();

    /**
     * @brief 内部跳转处理函数
     * 在 ThreadLoop 中被安全调用，防止与读包操作发生资源抢占
     */
    void ProcessSeek();

    /**
     * @brief 读取单个包并推给下游
     * @return true 继续读取；false 需要休眠或退出
     */
    bool ReadAndSendPacket();

    struct StreamInfo {
        int stream_index_{-1};
        std::atomic<int> pipeline_resource_count_{0}; // 当前线程内累积了多少包
        std::shared_ptr<std::function<void()>> pipeline_release_callback_; // 该条流的回调

        /**
         * @brief 重置流信息到初始状态
         */
        void Reset() {
            stream_index_ = -1;
            pipeline_resource_count_.store(0, std::memory_order_release);
            pipeline_release_callback_.reset();
        }
    };

private:
    IDemuxerListener* demuxer_listener_{nullptr};

    // FFmpeg格式上下文
    AVFormatContext* avformat_ctx_{nullptr};
    std::mutex avformat_mutex_;

    // 音视频流信息
    StreamInfo audio_stream_info_;
    StreamInfo video_stream_info_;

    // 工作线程
    std::thread demuxer_thread_;
    std::atomic<bool> is_paused_{true}; // 是否处于暂停读取状态
    std::atomic<bool> is_abort_requested_{false}; // 是否彻底终止销毁线程

    // 条件变量
    std::mutex condition_mutex_;
    std::condition_variable demuxer_cv_;
    const int kMaxPipelineCapacity = 100;

    // 跳转相关
    std::atomic<bool> is_seek_requested_{false};
    std::atomic<float> target_seek_progress_{-1.0f};
};
} // namespace av
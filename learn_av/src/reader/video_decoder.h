/**
 * @file video_decoder.h
 * @brief 视频解码类的实现
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "interface/ivideo_decoder.h"
#include "playback_sync.h"
#include "utils/thread_safe_queue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace av {

class VideoDecoder : public IVideoDecoder {
public:
    /**
     * @brief 构造函数
     */
    VideoDecoder();

    /**
     * @brief 析构函数
     */
    ~VideoDecoder();

    /**
     * @brief 设置监听器
     */
    void SetListener(Listener* listener) override;

    /**
     * @brief 初始化解码器上下文
     * @param info 纯净视频流元数据 DTO，包含编码格式、分辨率、帧率等核心参数
     * @return true 初始化成功；false 初始化失败
     */
    bool Init(const StreamInfo& info) override;

    /**
     * @brief 接收来自 Demuxer 的视频压缩数据包
     * @param packet 包含 H264 等压缩数据的智能指针
     * @note 内部应实现隐式背压机制，队列满时会阻塞调用线程
     */
    void PushPacket(std::shared_ptr<Packet> packet) override;

    /**
     * @brief 通知解码器输入已结束
     * @details 投递 EOF 标记，触发解码器排空内部残余的 B 帧/P 帧
     */
    void SendEOF() override;

    /**
     * @brief 启动异步解码工作线程
     * @param sync_ctx 注入全局状态同步上下文
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
     * @brief 清洗解码器内部参考帧缓存
     * @details 在 Seek 操作时调用，防止解码出花屏或马赛克 (因 I/P/B 帧时序依赖断裂)
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

private:
    /**
     * @brief 释放 FFmpeg 解码器
     */
    void Close();

    /**
     * @brief 视频解码死循环，运行在 decode_thread_ 中
     */
    void ThreadLoop();

    /**
     * @brief 处理解码出的裸帧，进行 YUV->RGBA 转换，并分发给监听器
     */
    void ProcessAndPack(AVFrame* frame);

private:
    // 1. FFmpeg 核心引擎与底层资源
    std::mutex codec_mutex_; ///< 保护解码器上下文的多线程并发访问
    AVCodecContext* av_codec_ctx_{nullptr}; ///< FFmpeg 视频解码器物理上下文

    // 2. 线程与并发状态控制
    std::thread decode_thread_; ///< 独立的视频解码后台 OS 线程
    std::atomic<bool> is_abort_requested_{false}; ///< 物理线程终止信令

    // 3. 数据流转通道
    ThreadSafeQueue<std::shared_ptr<Packet>> packet_queue_{60}; ///< 阻塞式背压队列，缓存约 1~2 秒视频包

    // 4. 视频元数据
    AVRational time_base_{0, 1}; ///< 视频流的时间基，用于计算绝对时钟

    // 5. 下游回调通道
    Listener* listener_{nullptr}; ///< 下游数据路由监听器指针

    // 6. 全局状态同步上下文 (The Source of Truth)
    std::shared_ptr<SyncContext> sync_ctx_{nullptr};
};
} // namespace av
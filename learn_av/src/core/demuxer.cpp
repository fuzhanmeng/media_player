/**
 * @file demuxer.cpp
 * @brief Demuxer module responsible for demuxing mp4.
 */

#include "demuxer.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "data/av_packet.h"
#include "interface/idemuxer_listener.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace av {

Demuxer::Demuxer() {}

Demuxer::~Demuxer() {
    Stop();

    if (avformat_ctx_) {
        avformat_close_input(&avformat_ctx_);
    }

    std::cout << "[Demuxer] Destroyed." << std::endl;
}

void Demuxer::SetListener(IDemuxerListener* listener) { listener_ = listener; }

bool Demuxer::Open(const std::string& url) {
    // 1. 加锁
    std::lock_guard<std::mutex> lock(avformat_mutex_);

    // 2. 打开文件
    if (avformat_ctx_) {
        avformat_close_input(&avformat_ctx_);
    }

    if (avformat_open_input(&avformat_ctx_, url.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "[Demuxer] Error: Failed to open input file, url: " << url << std::endl;
        return false;
    }

    // 3. 探测流信息
    if (avformat_find_stream_info(avformat_ctx_, nullptr) < 0) {
        std::cerr << "[Demuxer] Error: Failed to find stream information." << std::endl;
        return false;
    }

    // 4. 遍历流，找到视频和音频，并通知下游
    for (int i = 0; i < avformat_ctx_->nb_streams; ++i) {
        AVStream* stream = avformat_ctx_->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_.stream_index_ = i;
            if (listener_) {
                listener_->OnNotifyVideoStream(stream);
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_.stream_index_ = i;
            if (listener_) {
                listener_->OnNotifyAudioStream(stream);
            }
        }
    }
    return true;
}

void Demuxer::Start() {
    // 如果线程被销毁了，直接返回
    if (is_abort_) {
        return;
    }

    // 如果线程还没有创建,就创建它
    if (!thread_.joinable()) {
        thread_ = std::thread(&Demuxer::ThreadLoop, this);
    }

    // 如果原来是暂停的,那么设置为false
    is_paused_ = false;

    cv_.notify_all();
    std::cout << "[Demuxer] Engine started." << std::endl;
}

void Demuxer::Pause() {
    is_paused_ = true;
    std::cout << "[Demuxer] Engine paused." << std::endl;
}

void Demuxer::Stop() {
    // 线程是否彻底终止
    if (is_abort_) {
        return;
    }

    is_abort_ = true;
    is_paused_ = false; // 防止线程死锁在 paused 状态
    cv_.notify_all(); // 暴力唤醒所有正在沉睡的线程，让它们执行退出逻辑

    if (thread_.joinable()) {
        thread_.join();
    }

    std::cout << "[Demuxer] Engine stopping..." << std::endl;
}

void Demuxer::SeekTo(float progress) {
    if (is_seek_requested_) {
        return;
    }
    // 快速记录状态，立刻返回，绝不卡 UI 线程
    is_seek_requested_ = true;
    seek_progress_ = progress;

    cv_.notify_all();
}

float Demuxer::GetDuration() {
    if (!avformat_ctx_) {
        return 0.0f;
    }

    // duration 单位是微秒 (AV_TIME_BASE)，转换为秒返回
    float duration = static_cast<float>(avformat_ctx_->duration) / AV_TIME_BASE;
    return duration;
}

void Demuxer::ThreadLoop() {
    // 只要线程没有停止
    while (!is_abort_) {
        // 第一优先级：处理跳转
        if (is_seek_requested_) {
            ProcessSeek();
            // Seek 完不要往下走，重新开启新一轮循环，防止读到 Seek 前的脏包
            continue;
        }

        // 第二优先级：背压流量控制与暂停控制
        {
            std::unique_lock<std::mutex> lock(cv_mutex_);

            cv_.wait(lock, [this]() {
                // 什么时候允许线程醒来干活？
                // 1. 系统要求强行退出 (is_abort_ == true)
                // 2. 有人点击了进度条 (is_seek_requested_ == true)
                // 3. 没有按暂停，并且 视频和音频的包数量都没有超过仓库上限
                return is_abort_ || is_seek_requested_ ||
                    !is_paused_ && video_stream_.pipeline_resource_count_ < kMaxPipelineCapacity &&
                    audio_stream_.pipeline_resource_count_ < kMaxPipelineCapacity;
            });

            // 如果是被强行终止唤醒的，立刻退出
            if (is_abort_) {
                break;
            }

            // 如果是被 Seek 唤醒的，回到循环开头去处理 Seek
            if (is_seek_requested_) {
                continue;
            }
        }

        // 第三优先级：读取包并发送
        if (!ReadAndSendPacket()) {
            // 如果返回 false (通常是读到了文件末尾 EOF)
            // 不要退出线程，而是休眠一小会儿，等待用户重新 Seek 拖动进度条
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void Demuxer::ProcessSeek() {
    // 在这里要操作 FFmpeg，必须加锁

    // 1. add mutex
    std::unique_lock<std::mutex> lock(avformat_mutex_);
    if (!avformat_ctx_) {
        return;
    }

    // 1. 换算目标时间戳
    int64_t target_timestamp = static_cast<int64_t>(seek_progress_ * static_cast<float>(avformat_ctx_->duration));

    // 2. 调用 FFmpeg 底层 API 进行物理寻址 (寻找目标时间点之前的最近一个 I 帧)
    if (av_seek_frame(avformat_ctx_, -1, target_timestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
        std::cout << "[Demuxer] Seek executed successfully." << std::endl;

        // 3. 构建 Flush 空包，清洗下游的脏数据
        if (listener_) {
            // 给视频管线发送 Flush
            if (video_stream_.stream_index_ >= 0) {
                auto flush_video_pkt = std::make_shared<IAVPacket>(nullptr);
                flush_video_pkt->flags_ |= AVFrameFlag::kFlush;
                listener_->OnNotifyVideoPacket(flush_video_pkt);
            }

            // 给音频管线发送 Flush
            if (audio_stream_.stream_index_ >= 0) {
                auto flush_audio_pkt = std::make_shared<IAVPacket>(nullptr);
                flush_audio_pkt->flags_ |= AVFrameFlag::kFlush;
                listener_->OnNotifyAudioPacket(flush_audio_pkt);
            }
        }
    }
    // 4. 重置请求标志
    is_seek_requested_ = false;
}

bool Demuxer::ReadAndSendPacket() {
    // 分配一个干净的空箱子
    AVPacket* packet = av_packet_alloc();
    int ret = 0;

    if (!avformat_ctx_) {
        return false;
    }
    // 锁住文件上下文，防止读取一半时 UI 线程触发了其它对文件的操作
    {
        std::unique_lock<std::mutex> lock(avformat_mutex_);
        ret = av_read_frame(avformat_ctx_, packet);
    }

    if (ret >= 0) {
        auto safe_packet = std::make_shared<IAVPacket>(packet);

        // 如果是视频流
        if (packet->stream_index == video_stream_.stream_index_) {
            // 视频流包+1
            video_stream_.pipeline_resource_count_++;

            // 创建一个智能指针指向回调函数
            safe_packet->release_callback_ = std::make_shared<std::function<void()>>([this]() {
                video_stream_.pipeline_resource_count_--;
                cv_.notify_all(); // 通知被堵住的 Demuxer 可以继续发货了
            });

            // 将packet传走
            listener_->OnNotifyVideoPacket(safe_packet);
        } else if (packet->stream_index == audio_stream_.stream_index_) {
            audio_stream_.pipeline_resource_count_++;

            safe_packet->release_callback_ = std::make_shared<std::function<void()>>([this]() {
                audio_stream_.pipeline_resource_count_--;
                cv_.notify_all();
            });

            // 将packet发送走
            listener_->OnNotifyAudioPacket(safe_packet);
        } else {
            return true;
        }
        // 读到了文件尾部 (End of File)
    } else if (ret == AVERROR_EOF) {
        av_packet_free(&packet);
        return false;
    }
    // 发生严重读取错误else
    else {
        av_packet_free(&packet);
        std::cerr << "[Demuxer] Error: Failed to read frame." << std::endl;
        return false;
    }
    return true;
}

} // namespace av
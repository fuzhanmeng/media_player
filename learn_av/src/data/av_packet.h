/**
 * @file av_packet.h
 * @brief 跨线程安全传递的音频/视频数据包封装
 */

#pragma once
#include <functional>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace av {

// 定义帧标志位，用于通知下游做特殊处理
enum AVFrameFlag {
    kNone = 0,
    kFlush = 1 << 0, // 遇到这个标志，表示发生了 Seek 跳转，下游需要清空历史缓存
};

struct IAVPacket {
    AVPacket* packet_{nullptr};
    int flags_{kNone};

    // 释放回调：当这个 IAVPacket 被下游消费完并析构时，通知解复用器 "我处理完了"
    std::shared_ptr<std::function<void()>> release_callback_;

    explicit IAVPacket(AVPacket* pkt) : packet_(pkt) {}

    ~IAVPacket() {
        // 1. 释放内存
        if (packet_) {
            av_packet_free(&packet_);
        }

        // 2. 触发回调，通知管线资源已释放，解除 Demuxer 的阻塞
        if (release_callback_ && (*release_callback_)) {
            (*release_callback_)();
        }
    }

    // 禁用拷贝构造和赋值运算符
    IAVPacket(const IAVPacket&) = delete;
    IAVPacket& operator=(const IAVPacket&) = delete;

    // 移动构造函数
    IAVPacket(IAVPacket&& other) noexcept
        : packet_(other.packet_), flags_(other.flags_), release_callback_(std::move(other.release_callback_)) {
        other.packet_ = nullptr;
    }

    // 移动赋值运算符
    IAVPacket& operator=(IAVPacket&& other) noexcept {
        if (this != &other) {
            if (packet_) {
                av_packet_free(&packet_);
                packet_ = other.packet_;
                flags_ = other.flags_;
                release_callback_ = std::move(other.release_callback_);

                other.packet_ = nullptr;
            }
        }
        return *this;
    }
};

} // namespace av
/**
 * @file av_packet.h
 * @brief 跨线程安全传递的音频/视频数据包封装
 */
#pragma once

#include <functional>

#include "av_core_def.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace av {

class Packet {
public:
    // 1. 构造/析构
    Packet() { packet_ = av_packet_alloc(); }

    explicit Packet(AVPacket* packet) : packet_(packet) {}

    ~Packet() {
        // A. 释放 FFmpeg 底层 C 内存
        if (packet_) {
            av_packet_free(&packet_);
        }

        // B. 内存释放完后，执行回调，通知上游（Demuxer）减计数
        if (release_callback_) {
            release_callback_();
        }
    }

    // 2. 禁止拷贝构造/复制运算符 (防误操作)
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;

    // 3. 移动构造/移动赋值运算符
    Packet(Packet&& other) noexcept
        : packet_(other.packet_), flags_(other.flags_), release_callback_(std::move(other.release_callback_)) {
        other.packet_ = nullptr;
        other.flags_ = AVFrameFlag::kNone;
        other.release_callback_ = nullptr; // 移交后清空对方的回调
    }

    Packet& operator=(Packet&& other) noexcept {
        if (this != &other) {
            if (packet_) {
                av_packet_free(&packet_);
            }

            packet_ = other.packet_;
            flags_ = other.flags_;
            release_callback_ = std::move(other.release_callback_);

            other.packet_ = nullptr;
            other.flags_ = AVFrameFlag::kNone;
            other.release_callback_ = nullptr;
        }
        return *this;
    }

    // 4. 供外部调用的 API
    AVPacket* GetRawPacket() const { return packet_; }
    AVPacket* operator->() const { return packet_; }

    void AddFlag(AVFrameFlag flag) { flags_ |= flag; }
    bool HasFlag(AVFrameFlag flag) const { return (flags_ & flag) != AVFrameFlag::kNone; }

    void SetReleaseCallback(std::function<void()> cb) { release_callback_ = std::move(cb); }

private:
    AVPacket* packet_{nullptr};

    AVFrameFlag flags_{AVFrameFlag::kNone};

    std::function<void()> release_callback_{nullptr};
};

} // namespace av
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

// 修复点1：kNone 必须为 0
enum AVFrameFlag {
    kNone = 0,
    kFlush = 1 << 0, // 遇到这个标志，表示发生了 Seek 跳转，下游需要清空历史缓存
};

class Packet {
public:
    // 1. 构造/析构
    Packet() { packet_ = av_packet_alloc(); }

    explicit Packet(AVPacket* packet) : packet_(packet) {}

    ~Packet() {
        if (packet_) {
            av_packet_free(&packet_);
        }

        if (release_callback_ && (*release_callback_)) {
            (*release_callback_)();
        }
    }

    // 2. 禁止拷贝构造/复制运算符
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;

    // 3. 移动构造/移动赋值运算符
    Packet(Packet&& other) noexcept
        : packet_(other.packet_), flags_(other.flags_), release_callback_(std::move(other.release_callback_)) {
        other.packet_ = nullptr;
        other.flags_ = 0;
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
            other.flags_ = 0;
        }
        return *this;
    }

    // 4. 供外部调用的 API

    // 获取底层 FFmpeg 指针
    AVPacket* GetRawPacket() const { return packet_; }
    AVPacket* operator->() const { return packet_; }

    // 标志位管理
    void AddFlag(int flag) { flags_ |= flag; }
    bool HasFlag(int flag) const { return (flags_ & flag) != 0; }

    // 绑定释放回调
    void SetReleaseCallback(std::shared_ptr<std::function<void()>> cb) { release_callback_ = std::move(cb); }

private:
    AVPacket* packet_{nullptr};
    int flags_{0};
    std::shared_ptr<std::function<void()>> release_callback_{nullptr};
};

} // namespace av
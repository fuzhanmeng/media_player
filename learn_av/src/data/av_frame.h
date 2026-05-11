/**
 * @file av_frame.h
 * @brief H264/AAC的Frame封装
 */
#pragma once

extern "C" {
#include <libavutil/frame.h>
}

namespace av {
class Frame {
public:
    // 1. 构造/析构
    Frame() { frame_ = av_frame_alloc(); }
    explicit Frame(AVFrame* frame) : frame_(frame) {}

    ~Frame() {
        if (frame_) {
            av_frame_free(&frame_);
        }
    }

    // 2. 禁止拷贝构造/赋值运算符
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    // 3. 移动构造/移动赋值运算符
    Frame(Frame&& other) noexcept : frame_(other.frame_) { other.frame_ = nullptr; }

    Frame& operator=(Frame&& other) noexcept {
        // 检查是否为自赋值
        if (this != &other) {
            if (frame_) {
                av_frame_free(&frame_);
            }

            frame_ = other.frame_;
            other.frame_ = nullptr;
        }

        return *this;
    }

    // 4. 获取frame实例
    AVFrame* GetRawFrame() const { return frame_; }

    // 5. 重载->
    AVFrame* operator->() const { return frame_; }

private:
    AVFrame* frame_{nullptr};
};
} // namespace av
/**
 * @file av_frame.h
 * @brief 音视频原始帧封装类
 * @details 基于RAII机制封装FFmpeg的AVFrame结构体，提供安全的内存管理和便捷的属性访问
 *          支持移动语义，实现零拷贝的帧数据传递
 */
#pragma once

#include <cstdint>
#include <memory>
#include <utility>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace av {

/**
 * @class Frame
 * @brief 音视频原始帧基类
 * @note 禁止拷贝，支持移动语义
 */
class Frame {
public:
    // ================= 构造/析构 =================
    /**
     * @brief 默认构造函数，分配空的AVFrame
     */
    Frame() { frame_ = av_frame_alloc(); }

    /**
     * @brief 从原生AVFrame指针构造
     * @param frame 原生AVFrame指针，Frame将接管其所有权
     */
    explicit Frame(AVFrame* frame) : frame_(frame) {}

    /**
     * @brief 析构函数，自动释放AVFrame
     */
    ~Frame() {
        if (frame_) {
            av_frame_free(&frame_);
        }
    }

    // ================= 拷贝控制 =================
    /// 禁止拷贝构造
    Frame(const Frame&) = delete;
    /// 禁止拷贝赋值
    Frame& operator=(const Frame&) = delete;

    /// 移动构造
    Frame(Frame&& other) noexcept : frame_(std::exchange(other.frame_, nullptr)) {}

    /// 移动赋值
    Frame& operator=(Frame&& other) noexcept {
        if (this != &other) {
            // 释放当前对象的资源
            if (frame_) {
                av_frame_free(&frame_);
            }
            // 转移所有权
            frame_ = std::exchange(other.frame_, nullptr);
        }
        return *this;
    }

    // ================= 底层访问接口 =================
    /**
     * @brief 获取原生AVFrame指针
     * @return 原生AVFrame指针，所有权仍归Frame所有
     */
    AVFrame* GetRawFrame() const { return frame_; }

    /**
     * @brief 重载->运算符，直接访问AVFrame成员
     * @return 原生AVFrame指针
     */
    AVFrame* operator->() const { return frame_; }

    // ================= 状态判断 =================
    /**
     * @brief 判断帧是否为空
     * @return true 帧为空；false 帧包含有效数据
     */
    bool IsEmpty() const { return frame_ == nullptr || frame_->data[0] == nullptr; }

    // ================= 常用属性访问 =================
    /**
     * @brief 获取帧的显示时间戳
     * @return PTS时间戳，单位为流的时间基
     */
    int64_t GetPts() const { return frame_ ? frame_->pts : AV_NOPTS_VALUE; }

    /**
     * @brief 获取帧的解码时间戳
     * @return DTS时间戳，单位为流的时间基
     */
    int64_t GetDts() const { return frame_ ? frame_->pkt_dts : AV_NOPTS_VALUE; }

    /**
     * @brief 获取帧的持续时间
     * @return 持续时间，单位为流的时间基
     */
    int64_t GetDuration() const { return frame_ ? frame_->pkt_duration : 0; }

    /**
     * @brief 判断是否为关键帧
     * @return true 关键帧；false 非关键帧
     */
    bool IsKeyFrame() const { return frame_ && frame_->key_frame; }

    // ================= 工具方法 =================
    /**
     * @brief 深拷贝帧数据
     * @return 新的Frame对象，包含完全独立的帧数据
     * @note 这是一个高开销操作，仅在必要时使用
     */
    std::unique_ptr<Frame> Clone() const {
        if (!frame_) {
            return nullptr;
        }
        AVFrame* new_frame = av_frame_clone(frame_);
        return new_frame ? std::make_unique<Frame>(new_frame) : nullptr;
    }

    /**
     * @brief 重置帧，释放所有数据但保留AVFrame结构体
     */
    void Reset() {
        if (frame_) {
            av_frame_unref(frame_);
        }
    }

private:
    /// 原生FFmpeg帧指针
    AVFrame* frame_{nullptr};
};

} // namespace av
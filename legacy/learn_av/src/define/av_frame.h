/**
 * @file av_frame.h
 * @brief 音视频原始帧封装类
 * @details 基于RAII机制封装FFmpeg的AVFrame结构体，提供安全的内存管理和便捷的属性访问
 * 支持移动语义，实现零拷贝的帧数据传递
 */
#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "av_core_def.h"
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
    Frame(Frame&& other) noexcept
        : frame_(std::exchange(other.frame_, nullptr)),
          time_us_(std::exchange(other.time_us_, 0)),
          flags_(std::exchange(other.flags_, AVFrameFlag::kNone)) {}

    /// 移动赋值
    Frame& operator=(Frame&& other) noexcept {
        if (this != &other) {
            // 释放当前对象的资源
            if (frame_) {
                av_frame_free(&frame_);
            }
            // 转移所有权和具体时间戳
            frame_ = std::exchange(other.frame_, nullptr);
            time_us_ = std::exchange(other.time_us_, 0);
            flags_ = std::exchange(other.flags_, AVFrameFlag::kNone);
        }
        return *this;
    }

    // ================= 底层访问接口 =================
    /**
     * @brief 获取原生AVFrame指针
     * @return 原生AVFrame指针
     */
    AVFrame* GetRawFrame() const { return frame_; }

    /**
     * @brief 重载->运算符，直接访问AVFrame成员
     * @return 原生AVFrame指针
     */
    AVFrame* operator->() const { return frame_; }

    // ================= 标志位操作接口 =================
    /**
     * @brief 添加标志位
     * @param flag 要添加的标志
     */
    void AddFlag(AVFrameFlag flag) { flags_ |= flag; }

    /**
     * @brief 检查是否包含指定标志
     * @param flag 要检查的标志
     * @return true 包含该标志；false 不包含
     */
    bool HasFlag(AVFrameFlag flag) { return (flags_ & flag) != AVFrameFlag::kNone; }

    // ================= 状态判断 =================
    /**
     * @brief 判断帧是否为空
     * @return true 帧为空；false 帧包含有效数据
     */
    bool IsEmpty() const { return frame_ == nullptr || frame_->data[0] == nullptr; }

    /**
     * @brief 判断此帧是否合法且可用于渲染/播放
     * @details 综合校验底层结构、数据指针以及音视频核心参数
     * @return true 数据合法；false 数据损坏或未初始化
     */
    bool IsValid() const {
        // 1. 物理内存拦截
        // 如果底层结构体为空，或者没有分配数据载荷（data[0]为空），直接判非
        if (!frame_ || !frame_->data[0]) {
            return false;
        }

        // 2. 判断是否为合法的-视频帧
        // 视频帧必须具备：宽度 > 0，高度 > 0，且像素格式已被正确赋值
        if (frame_->width > 0 && frame_->height > 0 && frame_->format != AV_PIX_FMT_NONE) {
            // 进一步的严谨校验：视频帧的 linesize[0] (跨距) 也不能为 0
            return frame_->linesize[0] > 0;
        }

        // 3. 判断是否为合法的-音频帧
        // 音频帧必须具备：采样点数 > 0，采样率 > 0，且音频格式已被正确赋值
        if (frame_->nb_samples > 0 && frame_->sample_rate > 0 && frame_->format != AV_SAMPLE_FMT_NONE) {
            // 进一步的严谨校验：声道数或通道布局必须有效
            // 注意：FFmpeg 5.1+ 推荐使用 ch_layout，如果是老版本则是 channels > 0
            return frame_->channels > 0;
        }

        // 4. 既不符合视频规范，也不符合音频规范，视为非法畸形帧
        return false;
    }

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

    /**
     * @brief 获取当前帧的具体显示时间（秒）
     * @return 具体时间戳，用于音画同步
     */
    int64_t GetTimestampUs() const { return time_us_; }

    /**
     * @brief 设置当前帧的具体显示时间（秒）
     * @param pts_sec 结合时间基计算后的绝对时间
     */
    void SetTimestamp(int64_t pts_sec) { time_us_ = pts_sec; }

    /**
     * @brief 获取 秒级 时间戳，仅供 UI 进度条展示或日志打印使用
     * @return 返回双精度浮点型的秒数 (Seconds)
     */
    double GetTimestampSec() const { return static_cast<double>(time_us_) / 1000000.0; }

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
        if (!new_frame) {
            return nullptr;
        }

        auto cloned_frame = std::make_unique<Frame>(new_frame);
        // 确保深拷贝时，自定义的属性也被一并拷贝
        cloned_frame->SetTimestamp(this->time_us_);
        cloned_frame->AddFlag(this->flags_);
        return cloned_frame;
    }

    /**
     * @brief 重置帧，释放所有数据但保留AVFrame结构体
     */
    void Reset() {
        if (frame_) {
            av_frame_unref(frame_);
        }
        time_us_ = 0;
        flags_ = AVFrameFlag::kNone;
    }

private:
    /// 原生FFmpeg帧指针
    AVFrame* frame_{nullptr};

    /// 音画同步时间-微妙
    int64_t time_us_{0};

    /// 音视频帧状态标志
    AVFrameFlag flags_{AVFrameFlag::kNone};
};
} // namespace av
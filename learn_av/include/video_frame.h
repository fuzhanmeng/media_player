/**
 * @file video_frame.h
 * @brief 视频帧数据传输对象 (DTO)
 * @details 负责管理 YUV420P 格式的裸视频数据采用 RAII 机制（基于 std::shared_ptr）自动管理底层内存的生命周期
 */
#pragma once
#include <array>
#include <cstdint>
#include <memory>

#include "av_core_def.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace av {

struct VideoFrame {
    // 1. 图像属性
    int width_{0}; // 画面宽度（像素）
    int height_{0}; // 画面高度（像素）

    // 2. 数据与内存布局 (YUV420P)
    // data_[0] -> Y 分量指针 / data_[1] -> U 分量指针 / data_[2] -> V 分量指针
    std::array<uint8_t*, 3> data_{nullptr, nullptr, nullptr};

    // 3. 各个分量的内存跨距 (Bytes per line)
    std::array<int, 3> linesize_{0, 0, 0};

    // 4. 底层连续内存块，使用 shared_ptr + 自定义删除器管理
    std::shared_ptr<uint8_t> buffer_{nullptr};

    // 5. 绝对显示时间戳，统一转换为微秒 (Microseconds)，用于底层严谨的音画同步计算
    int64_t time_us_{0};

    // 6. 标志位
    AVFrameFlag flags_{AVFrameFlag::kNone};

    // 7. 方法实现
    /**
     * @brief 构造函数：按照 YUV420P 的结构分配一整块连续内存
     * @param width 视频宽度
     * @param height 视频高度
     * @throws std::bad_alloc 内存分配失败时抛出异常
     */
    VideoFrame(int width, int height) : width_(width), height_(height) {
        // 正确计算 UV 分量尺寸（向上取整，兼容 1080x1920 或奇数分辨率，防止内存越界）
        const int kYSize = width * height;
        const int kUvWidth = (width + 1) / 2; // 奇数宽度向上取整
        const int kUvHeight = (height + 1) / 2; // 奇数高度向上取整
        const int kUvSize = kUvWidth * kUvHeight;

        // Y 分量大小 + U 分量大小 + V 分量大小
        const int kTotalSize = kYSize + kUvSize * 2;

        // 使用 FFmpeg 提供的对齐内存分配函数（有利于后续可能的 SIMD 加速操作）
        uint8_t* raw_buffer = static_cast<uint8_t*>(av_malloc(kTotalSize));
        if (!raw_buffer) {
            throw std::bad_alloc();
        }

        // 将裸指针交给 shared_ptr 托管，并绑定 FFmpeg 的专属释放函数 av_free
        buffer_ = std::shared_ptr<uint8_t>(raw_buffer, [](uint8_t* ptr) { av_free(ptr); });

        // 切割这块大连续内存，把首地址分别指给 Y, U, V 平面
        data_[0] = raw_buffer; // Y plane 起始点
        data_[1] = raw_buffer + kYSize; // U plane 起始点
        data_[2] = raw_buffer + kYSize + kUvSize; // V plane 起始点

        // 正确设置 linesize（对于紧凑的 YUV420P，Y 的步长就是宽，UV 的步长就是宽的一半向上取整）
        linesize_[0] = width;
        linesize_[1] = kUvWidth;
        linesize_[2] = kUvWidth;
    }

    /**
     * @brief 设置时间戳
     * @param time_us 外部 (Decoder) 传入的绝对时间戳（单位：微秒）
     */
    void SetTimestamp(int64_t time_us) { time_us_ = time_us; }

    /**
     * @brief 获取用于底层音画同步计算的精准时间戳
     * @return 返回微秒 (Microseconds) 级别的绝对时间戳
     */
    int64_t GetTimestampUs() const { return time_us_; }

    /**
     * @brief 获取 秒级 时间戳, UI 进度条展示或日志打印使用
     * @return 返回双精度浮点型的秒数 (Seconds)
     */
    double GetTimestampSec() const { return static_cast<double>(time_us_) / 1000000.0; }

    /**
     * @brief 校验当前视频帧是否合法且物理数据完整
     * @return true 表示结构完整可以安全传入 OpenGL 渲染，false 表示损坏或空帧
     */
    bool IsValid() const {
        // 1. 检查基本尺寸：宽/高必须大于 0
        if (width_ <= 0 || height_ <= 0) {
            return false;
        }

        // 2. 检查底层内存：智能指针是否持有有效的内存块 (防止被 move 后野指针)
        if (!buffer_) {
            return false;
        }

        // 3. 检查分量指针：Y, U, V 三个平面的指针都不能悬空
        if (!data_[0] || !data_[1] || !data_[2]) {
            return false;
        }

        // 4. 检查步长 (linesize)：防止 OpenGL 纹理上传时发生内存越界
        // Y 分量的步长不能小于画面宽度
        // U/V 分量的步长不能小于画面宽度的一半（向上取整）
        const int kUvWidth = (width_ + 1) / 2;
        if (linesize_[0] < width_ || linesize_[1] < kUvWidth || linesize_[2] < kUvWidth) {
            return false;
        }

        return true;
    }

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
    bool HasFlag(AVFrameFlag flag) const { return (flags_ & flag) != AVFrameFlag::kNone; }
};

} // namespace av
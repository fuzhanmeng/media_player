/**
 * @file video_frame.h
 * @brief 视频帧数据传输对象 (DTO)
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace av {

struct VideoFrame {
    int width_{0}; // 画面宽度
    int height_{0}; // 画面高度

    std::array<uint8_t*, 3> data_{nullptr, nullptr, nullptr};
    std::array<int, 3> linesize_{0, 0, 0};

    std::shared_ptr<uint8_t> buffer_{nullptr};

    int64_t pts_{0}; // 显示时间戳 (微秒)

    /**
     * @brief 构造函数：按照 YUV420P 的结构分配连续内存
     * @param width 视频宽度
     * @param height 视频高度
     * @throws std::bad_alloc 内存分配失败时抛出
     */
    VideoFrame(int width, int height) : width_(width), height_(height) {
        // 正确计算UV分量尺寸（向上取整，兼容所有奇数分辨率）
        const int kYSize = width * height;
        const int kUvWidth = (width + 1) / 2; // 奇数宽度向上取整
        const int kUvHeight = (height + 1) / 2; // 奇数高度向上取整
        const int kUvSize = kUvWidth * kUvHeight;
        const int kTotalSize = kYSize + kUvSize * 2;

        // 添加内存分配失败检查
        uint8_t* raw_buffer = static_cast<uint8_t*>(av_malloc(kTotalSize));
        if (!raw_buffer) {
            throw std::bad_alloc();
        }

        // 修正av_free的lambda（av_free返回void，不能return）
        buffer_ = std::shared_ptr<uint8_t>(raw_buffer, [](uint8_t* ptr) { av_free(ptr); });

        // 切割这块大内存，把指针分别指给 Y, U, V 平面
        data_[0] = raw_buffer; // Y plane
        data_[1] = raw_buffer + kYSize; // U plane
        data_[2] = raw_buffer + kYSize + kUvSize; // V plane

        // 正确设置linesize（与实际UV宽度一致）
        linesize_[0] = width;
        linesize_[1] = kUvWidth;
        linesize_[2] = kUvWidth;
    }

    /**
     * @brief 设置时间戳
     * @param pts_sec 时间戳（秒）
     */
    void SetTimestamp(double pts_sec) { pts_ = static_cast<int64_t>(pts_sec * 1000000); }

    /**
     * @brief 获取总字节数
     * @return YUV420P帧的总字节数
     */
    size_t GetTotalBytes() const {
        const int kUvSize = linesize_[1] * ((height_ + 1) / 2);
        return (width_ * height_ + kUvSize * 2);
    }
};

} // namespace av
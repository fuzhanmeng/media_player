/**
 * @file audio_frame.h
 * @brief 音频帧数据对象，保存一段 packed（多声道音频数据的 交错排列（Interleaved）方式 ）
 * PCM 数据和随帧传递的时间戳、世代号、MediaDataFlag
 *
 * @details
 * AudioFrame 是 Decoder 的输出，后续会流向音画同步和音频输出设备和录制分支
 *
 * 数据流方向
 * Demuxer 读取 AVPacket-> Decoder / Resampler -> AudioFrame -> AudioSink / Sync /Recorder
 *
 * 生命周期
 * 构造时按 packed PCM 布局分配一块连续的内存
 * data_ 指向 buffer_ 的起始地址，不单独拥有内存
 * buffer_ 用 std::shared_ptr 托管，最后一个副本释放时自动 delete[]
 * 无论是音频 PCM 还是视频 YUV，其实都是一堆纯粹的二进制字节流，最标准的做法就是使用 uint8_t 的数组来存储
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "audio_format.h"
#include "mediacore/av_types/media_data_flag.h"
#include "mediacore/logger.h"

namespace mediacore {
/**
 * @struct AudioFrame
 * @brief 解码后的单 packed PCM 音频帧
 */
struct AudioFrame {
    AudioFormat format_{};  ///< 当前音频帧的数据布局
    std::size_t sample_count_{0};  ///< 每个声道的样本数，如 1024
    std::size_t byte_size_{0};  ///< 整帧 PCM 数据总字节数

    /**
     * @brief 原始 PCM 内存的唯一生命周期入口
     * @details
     * data_ 只是保存指向这块 buffer_ 内存的起始地址，不拥有这块内存也不负责释放内存
     * buffer_ 通过自定义删除器调用 delete[] 管理数组生命周期
     */
    std::shared_ptr<uint8_t> buffer_{nullptr};

    /// PCM 数据起始地址，等价于 buffer_.get()
    uint8_t* data_{nullptr};

    /// 该帧的显示时间戳 - 微秒，int64_t 有符号的 64 位整数
    int64_t pts_us_{0};

    /// Seek 世代号，用于判断 Seek 后数据是否更新，uint64_t，只能存 0 和正数，无符号的 64 位整数
    uint64_t epoch_{0};

    /// 随帧传递的控制标志 kFlush / kEOF/ kKeyFrame
    MediaDataFlag flags_{MediaDataFlag::kNone};

    /**
     * @brief 构造一个空的音频帧
     * @details 空帧不持有 PCM 内存，IsValid() 返回false
     */
    AudioFrame() = default;

    /**
     * @brief 按 packed PCM 分配一段连续音频内存
     * @param format 音频帧的布局
     * @param sample_count 每个声道的样本数量
     * @details
     * packed PCM 的总字节数 byte_size_ = sample_count * channel_count_ * bytes_per_sample
     * 每个声道的样本数量 * 声道数量 * 单个采样点占用的字节数量
     */
    explicit AudioFrame(AudioFormat format, std::size_t sample_count) : format_(format), sample_count_(sample_count) {
        if (!format_.IsValid()) {
            LOG_ERROR("AudioFrame create failed: Invalid format");
            return;
        }

        // 如果声道样本数为空
        if (sample_count_ == 0) {
            LOG_ERROR("AudioFrame create failed: sample_count is 0");
            return;
        }

        // 计算 一帧 packed PCM 的总字节数
        byte_size_ = sample_count_ * static_cast<std::size_t>(format_.channel_count_) *
            static_cast<std::size_t>(format_.bytes_per_sample_);

        // 申请 byte_size_ 大小的空间
        uint8_t* raw_buffer = new uint8_t[byte_size_];
        buffer_ = std::shared_ptr<uint8_t>(raw_buffer, [](uint8_t* pointer) {
            LOG_TRACE("AudioFrame buffer released: {}", static_cast<const void*>(pointer));
            delete[] pointer;
        });

        data_ = raw_buffer;
    }

    /**
     * @brief 追加一个带内标志
     * @param flag 要追加的带内标志
     */
    void AddFlag(MediaDataFlag flag) { flags_ |= flag; }

    /**
     * @brief 检查当前帧是否携带指定的带内标志
     * @param flag 要检查的标志
     * @return true 表示含有指定的带内标志，false 表示不携带指定的标志
     */
    bool HasFlag(MediaDataFlag flag) const { return (flags_ & flag) != MediaDataFlag::kNone; }

    /**
     * @brief 检查本帧是不是一个有效的 packed PCM 音频帧
     * @return true 音频帧有效，false 表示本帧无效
     */
    bool IsValid() const {
        // 1. 判读音频格式是否有问题
        if (!format_.IsValid()) {
            return false;
        }

        // 2. 判断音频帧是否有数据，Flush / EOF 这类控制帧通常不携带 PCM 数据
        if (sample_count_ == 0 || byte_size_ == 0) {
            return false;
        }

        // 3. 判断内存是否有效,buffer_（智能指针）和 data_（裸指针）是指向实际 PCM 数据的
        if (!buffer_ || !data_) {
            return false;
        }

        // 4. 检查数据完整性
        const std::size_t expected_size = sample_count_ * static_cast<std::size_t>(format_.channel_count_) *
            static_cast<std::size_t>(format_.bytes_per_sample_);

        if (expected_size != byte_size_) {
            return false;
        }

        return true;
    }
};
}  // namespace mediacore
/**
 * @file audio_frame.h
 * @brief 解码后的单帧 Packed PCM 音频数据值类型定义
 * @details
 * 负责作为音频解码输出与下游渲染/处理模块之间的自包含数据载荷：
 *
 * 1. 数据流向拓扑：
 *    Demuxer 读取 AVPacket -> AudioDecoder / Resampler -> AudioFrame -> AudioSink / AVSync / Recorder
 *
 * 2. 内存生命周期与安全性：
 *    - 构造时根据 Packed PCM 布局（声道交错）一次性在堆上分配连续内存；
 *    - data_ 指向 buffer_ 内存的首地址，不单独拥有内存；
 *    - buffer_ 由 std::shared_ptr 托管，绑定自定义删除器执行 delete[]，最后一个副本释放时自动归还内存；
 *    - 解除对 FFmpeg AVFrame 的生命周期耦合，可安全跨线程队列流转或跨模块持有。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>

#include "audio_format.h"
#include "mediacore/av_types/media_data_flag.h"
#include "mediacore/logger.h"

namespace mediacore {

/**
 * @struct AudioFrame
 * @brief 承载单帧交错排布（Packed）PCM 音频数据的自包含结构体
 */
struct AudioFrame final {
    AudioFormat format_{};  ///< 音频采样格式与声道排布信息
    std::size_t sample_count_{0};  ///< 单个声道的采样点总数（例如 1024）
    std::size_t byte_size_{0};  ///< 整帧 PCM 数据总字节大小

    /**
     * @brief 托管 PCM 内存生命周期的智能指针
     * @details 实际分配的连续数组内存由其独占管理，通过自定义删除器释放。
     */
    std::shared_ptr<std::uint8_t> buffer_{nullptr};

    /// PCM 数据有效起始地址，等价于 buffer_.get()
    std::uint8_t* data_{nullptr};

    /**
     * @brief 该帧音频的呈现时间戳（PTS，单位：微秒）
     * @details 使用 std::optional 表达时间戳存在性，未解析出或缺失时间戳时为空，防止误当作 0 微秒
     */
    std::optional<std::int64_t> pts_us_{};

    /// Seek 世代版本号，用于下游模块过滤与丢弃过期的历史音频帧
    std::uint64_t epoch_{0};

    /// 随帧传递的流控与元数据标志位组合（如 kKeyFrame / kFlush）
    MediaDataFlag flags_{MediaDataFlag::kNone};

    /**
     * @brief 默认构造函数：构造一个空音频帧
     * @details 空帧不持有任何内存，IsValid() 返回 false，常用于控制信令帧或初始占位。
     */
    AudioFrame() = default;

    /**
     * @brief 含参构造函数：按 Packed PCM 规则分配连续内存
     * @param format 音频格式（包含采样率、声道数、量化字节大小）
     * @param sample_count 单声道采样点数
     * @throws std::invalid_argument 当音频格式无效或 sample_count 为 0 时抛出
     * @throws std::bad_alloc 当堆内存分配失败时抛出
     */
    explicit AudioFrame(AudioFormat format, std::size_t sample_count) : format_(format), sample_count_(sample_count) {
        if (!format_.IsValid()) {
            LOG_ERROR("AudioFrame allocation failed: Invalid format");
            throw std::invalid_argument("AudioFrame requires a valid AudioFormat");
        }

        if (sample_count_ == 0) {
            LOG_ERROR("AudioFrame allocation failed: sample_count is 0");
            throw std::invalid_argument("AudioFrame sample_count must be greater than zero");
        }

        // 计算整帧总字节数：单声道采样点数 * 声道数 * 单个采样点字节大小
        byte_size_ = sample_count_ * static_cast<std::size_t>(format_.channel_count_) *
            static_cast<std::size_t>(format_.bytes_per_sample_);

        // 分配连续字节数组
        auto* raw_buffer = new std::uint8_t[byte_size_];
        buffer_ = std::shared_ptr<std::uint8_t>(raw_buffer, [](std::uint8_t* pointer) {
            LOG_TRACE("AudioFrame buffer released: {}", static_cast<const void*>(pointer));
            delete[] pointer;
        });

        data_ = raw_buffer;
    }

    /**
     * @brief 向当前帧追加一个带内标志位
     * @param flag 待追加的控制标志
     */
    void AddFlag(MediaDataFlag flag) noexcept { flags_ |= flag; }

    /**
     * @brief 查询当前帧是否携带指定的带内标志
     * @param flag 待检测的目标标志
     * @return 携带该标志返回 true，否则返回 false
     */
    bool HasFlag(MediaDataFlag flag) const noexcept { return mediacore::HasFlag(flags_, flag); }

    /**
     * @brief 校验本帧是否为结构完整且可安全交付声卡/重采样器的 Packed PCM 帧
     * @return 内存和排布完备有效返回 true，否则返回 false
     */
    bool IsValid() const noexcept {
        // 1. 校验基础格式自洽性
        if (!format_.IsValid()) {
            return false;
        }

        // 2. 校验样本量与字节体量（Flush 等控制帧不具备实际 PCM 载荷）
        if (sample_count_ == 0 || byte_size_ == 0) {
            return false;
        }

        // 3. 校验内存指针有效性
        if (!buffer_ || data_ == nullptr) {
            return false;
        }

        // 4. 校验字节长度数学一致性，防止下溢或溢出
        const std::size_t expected_size = sample_count_ * static_cast<std::size_t>(format_.channel_count_) *
            static_cast<std::size_t>(format_.bytes_per_sample_);
        if (expected_size != byte_size_) {
            return false;
        }

        return true;
    }
};

}  // namespace mediacore
/**
 * @file packet.cpp
 * @brief Packet 的 AVPacket 所有权实现
 */

#include "av_types/packet.h"

#include "mediacore/logger.h"

extern "C" {
#include <libavcodec/packet.h>
}

namespace mediacore {

/**
 * @brief 构造 Packet 并分配一个空的 AVPacket
 * @throw std::bad_alloc 分配失败时抛出
 * @details
 * av_packet_alloc() 只分配结构体，不分配 data 缓冲,data 缓冲由 Demuxer 调用 av_read_frame 时分
 * 配，失败时返回 nullptr，调用时需要检查 IsEmpty()
 */
Packet::Packet() : packet_(av_packet_alloc()) {
    if (packet_ == nullptr) {
        LOG_ERROR("Packet allocation failed: av_packet_alloc returned null");
        throw std::bad_alloc();
    }

    LOG_TRACE("AVPacket allocated: {}", static_cast<const void*>(packet_));
}

/**
 * @brief 释放 Packet 持有的 AVPacket
 * @details av_packet_free() 做三件事
 * 1. av_packet_unref(packet_)：释放 data 缓冲（引用计数减 1，为 0 时真正释放）
 * 2. av_freep(&packet_)：释放 AVPacket 结构体内存
 * 3. 将 packet_ 置为 nullptr（避免悬空指针）
 */
Packet::~Packet() {
    LOG_TRACE("AVPacket released: {}", static_cast<const void*>(packet_));
    av_packet_free(&packet_);
}

/**
 * @brief 移动构造 Packet
 * @param other 被移动的 Packet
 * @details
 * 移动构造只转移指针和随包信息，不复制压缩数据,移动完成后，other 不再拥有原来的 AVPacket，
 * 这样 other 析构时不会再次释放这块内存
 */
Packet::Packet(Packet&& other) noexcept
    : packet_(std::exchange(other.packet_, nullptr)),
      stream_index_(std::exchange(other.stream_index_, -1)),
      size_bytes_(std::exchange(other.size_bytes_, 0)),
      key_frame_(std::exchange(other.key_frame_, false)),
      pts_us_(std::exchange(other.pts_us_, std::nullopt)),
      dts_us_(std::exchange(other.dts_us_, std::nullopt)),
      duration_us_(std::exchange(other.duration_us_, std::nullopt)),
      epoch_(std::exchange(other.epoch_, 0)) {}

/**
 * @brief 移动赋值运算函数
 * @details
 * 先释放当前对象已持有的 AVPacket（否则会在接管 other 的指针后造成内存泄漏）
 * 再转移 other 的所有权，最后将 other 清空以防止其析构时重复释放
 */
Packet& Packet::operator=(Packet&& other) noexcept {
    // 1. 先判断是不是自赋值
    if (this != &other) {
        // 释放当前对象持有的资源
        if (packet_) {
            LOG_TRACE("AVPacket released before move assignment: {}", static_cast<const void*>(packet_));
            av_packet_free(&packet_);
        }

        // 2. 转移所有权
        packet_ = std::exchange(other.packet_, nullptr);
        stream_index_ = std::exchange(other.stream_index_, -1);
        size_bytes_ = std::exchange(other.size_bytes_, 0);
        key_frame_ = std::exchange(other.key_frame_, false);
        pts_us_ = std::exchange(other.pts_us_, std::nullopt);
        dts_us_ = std::exchange(other.dts_us_, std::nullopt);
        duration_us_ = std::exchange(other.duration_us_, std::nullopt);
        epoch_ = std::exchange(other.epoch_, 0);

        LOG_TRACE("Packet move-assigned: {}", static_cast<const void*>(packet_));

        return *this;
    }

    return *this;
}

/**
 * @brief 清空解复用由元数据
 */
void Packet::ResetDemuxMetadata() noexcept {
    stream_index_ = -1;
    size_bytes_ = 0;
    key_frame_ = false;
    pts_us_.reset();
    dts_us_.reset();
    duration_us_.reset();
    epoch_ = 0;
}

/**
 * @brief 写入解复用元数据
 */
void Packet::SetDemuxMetadata(DemuxMetadata metadata) noexcept {
    stream_index_ = metadata.stream_index_;
    size_bytes_ = metadata.size_bytes_;
    key_frame_ = metadata.key_frame_;
    pts_us_ = metadata.pts_us_;
    dts_us_ = metadata.dts_us_;
    duration_us_ = metadata.duration_us_;
}
}  // namespace mediacore
/**
 * @file packet.cpp
 * @brief Packet 的 RAII 实现
 */

#include "av_types/packet.h"

#include "mediacore/logger.h"

extern "C" {
#include <libavcodec/packet.h>
}

namespace mediacore {

/**
 * @brief 构造 Packet 并分配一个空的 AVPacket
 * @details
 * av_packet_alloc() 只分配结构体，不分配 data 缓冲,data 缓冲由 Demuxer 调用 av_read_frame 时分
 * 配，失败时返回 nullptr，调用时需要检查 IsEmpty()
 */
Packet::Packet() : pkt_(av_packet_alloc()) {
    if (pkt_ == nullptr) {
        LOG_ERROR("AVPacket allocation failed");
    }

    LOG_TRACE("AVPacket allocated: {}", static_cast<const void*>(pkt_));
}

/**
 * @brief 释放 Packet 持有的 AVPacket
 * @details av_packet_free() 做三件事
 * 1. av_packet_unref(pkt_)：释放 data 缓冲（引用计数减 1，为 0 时真正释放）
 * 2. av_freep(&pkt_)：释放 AVPacket 结构体内存
 * 3. 将 pkt_ 置为 nullptr（避免悬空指针）
 */
Packet::~Packet() {
    if (pkt_) {
        LOG_TRACE("AVPacket released: {}", static_cast<const void*>(pkt_));
        av_packet_free(&pkt_);
    } else {
        LOG_TRACE("Packet destructor called with empty AVPacket");
    }
}

/**
 * @brief 移动构造 Packet
 * @param other 被移动的 Packet
 * @details
 * 移动构造只转移指针和随包信息，不复制压缩数据,移动完成后，other 不再拥有原来的 AVPacket，
 * 这样 other 析构时不会再次释放这块内存
 */
Packet::Packet(Packet&& other) noexcept
    : pkt_(other.pkt_), pts_us_(other.pts_us_), dts_us_(other.dts_us_), epoch_(other.epoch_), flags_(other.flags_) {
    LOG_TRACE(
        "AVPacket move-constructed: {} -> {}", static_cast<const void*>(other.pkt_), static_cast<const void*>(pkt_));

    other.pkt_ = nullptr;
    other.pts_us_.reset();
    other.dts_us_.reset();
    other.epoch_ = 0;
    other.flags_ = MediaDataFlag::kNone;
}

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
        if (pkt_) {
            LOG_TRACE("AVPacket released before move assignment: {}", static_cast<const void*>(pkt_));
            av_packet_free(&pkt_);
        }

        // 2. 转移所有权
        pkt_ = other.pkt_;
        pts_us_ = other.pts_us_;
        dts_us_ = other.dts_us_;
        epoch_ = other.epoch_;
        flags_ = other.flags_;

        // 3. 清空源对象
        other.pkt_ = nullptr;
        other.pts_us_.reset();
        other.dts_us_.reset();
        other.epoch_ = 0;
        other.flags_ = MediaDataFlag::kNone;

        LOG_TRACE("AVPacket move-assigned: {}", static_cast<const void*>(pkt_));
    } else {
        LOG_TRACE("Packet self move-assignment ignored");
    }

    return *this;
}
}  // namespace mediacore
/**
 * @file packet.cpp
 * @brief Packet DTO 实现
 */

#include "av_types/packet.h"

extern "C" {
#include <libavcodec/packet.h>
}

namespace mediacore {

/**
 * @brief 构造函数
 * @details av_packet_alloc() 只分配结构体，不分配 data 缓冲
 *  data 缓冲由 Demuxer 调用 av_read_frame 时分配，失败时返回 nullptr，调用时需要检查 IsEmpty()
 */
Packet::Packet() : pkt_(av_packet_alloc()), pts_us_(0), dts_us_(0), epoch_(0), flags_(AVFrameFlag::kNone) {}

/**
 * @brief 析构函数
 * @details av_packet_free() 做三件事
 * 1. av_packet_unref(pkt_)：释放 data 缓冲（引用计数减 1，为 0 时真正释放）
 * 2. av_freep(&pkt_)：释放 AVPacket 结构体内存
 * 3. 将 pkt_ 置为 nullptr（避免悬空指针）
 */
Packet::~Packet() {
    if (pkt_) {
        av_packet_free(&pkt_);
    }
}

/**
 * @brief 移动构造：转移所有权，不释放源对象的 pkt_
 * @details 关键点：源对象的 pkt_ 指针被"窃取"到新对象，源对象不再拥有这块内存。
 *          源对象只需置空（放弃所有权），绝不能调用 av_packet_free，否则：
 *          - 新对象的 pkt_ 变成野指针（指向已释放的内存）
 *          - 访问新对象时崩溃（Use After Free）
 *
 *          为什么清空源对象的其他字段？
 *          - 防止误用：移动后的对象应处于"有效但未指定"状态
 *          - 语义清晰：0/nullptr 表示"已被掏空"
 */
Packet::Packet(Packet&& other) noexcept
    : pkt_(other.pkt_), pts_us_(other.pts_us_), dts_us_(other.dts_us_), epoch_(other.epoch_), flags_(other.flags_) {
    other.pkt_ = nullptr;
    other.pts_us_ = 0;
    other.dts_us_ = 0;
    other.epoch_ = 0;
    other.flags_ = AVFrameFlag::kNone;
}

/**
 * @brief 移动赋值运算函数
 * @details 移动赋值分三步：
 *
 * 1. 释放当前持有的资源（避免内存泄漏）
 *    - 当前对象可能已持有一个有效的 AVPacket*
 *    - 必须先释放，否则转移源对象的指针后，旧指针丢失，内存泄漏
 *
 * 2. 转移源对象的所有权
 *    - 窃取源对象的 pkt_ 和元信息
 *    - 此时当前对象接管了源对象的内存
 *
 * 3. 清空源对象
 *    - 源对象不再拥有 pkt_，置空防止析构时 double free
 *    - 其他字段置 0，语义清晰
 */
Packet& Packet::operator=(Packet&& other) noexcept {
    // 1. 先判断是不是自赋值
    if (this != &other) {
        // 释放当前对象持有的资源
        if (pkt_) {
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
        other.pts_us_ = 0;
        other.dts_us_ = 0;
        other.epoch_ = 0;
        other.flags_ = AVFrameFlag::kNone;
    }

    return *this;
}
} // namespace mediacore
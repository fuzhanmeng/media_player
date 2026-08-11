/**
 * @file packet.h
 * @brief Packrt DTO --- RAII 封装的 AVPacket，携带时间戳/世代号/标志位
 * @details Demuxer 从容器中读出压缩数据包，通过 Link<Packet> 传递给 Decoder 消费
 *  时间戳统一换算为微秒（下游无须关系 time_base），epoch_ 用于 Seek 时抛弃旧世代包，flags 扩展带内
 *  信令
 *
 * @par 职责边界
 *  - Packet 只负责"数据容器"：持有 AVPacket* + 元信息（pts/dts/epoch/flags）
 *  - 不负责解封装逻辑（由 DemuxerNode 完成）
 *  - 不负责解码逻辑（由 DecoderNode 完成）
 *
 * @par 生命周期
 *  创建：DemuxerNode 调用 av_read_frame 后构造 Packet
 *  传递：通过 Link<Packet> 在 DAG 节点间移动（移动语义，零拷贝）
 *  销毁：Decoder 消费后自动析构，av_packet_free 释放 FFmpeg 内存
 *
 * @par 线程安全性
 *  Packet 本身不保证线程安全（单一所有者模型）。跨线程传递通过 Link<Packet>
 *  的 ThreadSafeQueue 保证安全（生产者-消费者模式）
 *
 * @par 时间戳为什么用微秒？
 *  FFmpeg 的 AVPacket.pts/dts 以 time_base 为单位（各流不同，如 1/90000）
 *  统一换算为微秒后：
 *  - 下游无需关心 time_base 复杂度
 *  - AVSyncNode 可直接比较音视频时间戳
 *  - 换算公式：pts_us_ = pts * time_base.num * 1000000 / time_base.den
 *  换算时机：Demuxer 读取时完成，一次换算，全链路受益
 */

#pragma once

#include <cstdint>

#include "mediacore/av_types/av_frame_flag.h"

struct AVPacket;

namespace mediacore {

/**
 * @file packet.h
 * @brief 压缩数据包 DTO， RAII 封装的 AVPacket*
 *
 * @details
 * - 独占所有权：禁止拷贝（AVPacket 深拷贝代价高），支持移动（零拷贝传递）
 * - 时间戳统一微秒：下游无需关心 time_base
 * - epoch 字段：Seek 时递增，下游丢弃旧世代包
 * - 带内信令：通过 flags 传递 EOF/Flush，无需额外通道
 *
 * @par 与 AVPacket 的关系
 *  Packet 是 AVPacket 的 RAII 包装 + 元信息扩展，生命周期管理：
 *  - 构造：av_packet_alloc() 分配内存
 *  - 移动：转移指针所有权，源对象置 nullptr
 *  - 析构：av_packet_free() 释放内存和引用的数据
 *
 * @par 输入输出
 *  输入：Demuxer 调用 GetAVPacket() 获取裸指针，填充 av_read_frame 数据
 *  输出：Decoder 读取 GetAVPacket() + pts/dts/epoch/flags，送入 avcodec_send_packet

 */
class Packet {
public:
    /**
     * @brief 默认构造函数：分配空的 AVPacket
     */
    Packet();

    /**
     * @brief 析构函数：释放 AVPacket 内存
     */
    ~Packet();

    // 禁止拷贝 AVPacket
    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;

    /**
     * @brief 移动构造：转移所有权
     */
    Packet(Packet&& other) noexcept;

    /**
     * @brief 移动赋值：转移所有权
     */
    Packet& operator=(Packet&& other) noexcept;

    /**
     * @brief 获取底层 AVPacket 指针 ,只读
     */
    const AVPacket* GetAVPacket() const { return pkt_; }

    /**
     * @brief 获取底层 AVPacket 指针 ,可写
     */
    AVPacket* GetAVPacket() { return pkt_; }

    /**
     * @brief 获取展示时间戳（微秒）
     * @return int64_t 已换算为微秒的 PTS（Presentation Time Stamp）
     */
    int64_t GetPtsUs() const { return pts_us_; }

    /**
     * @brief 设置展示时间戳（微秒）
     * @param pts 微秒级时间戳（由 Demuxer 从 AVPacket.pts 换算）
     */
    void SetPtsUs(int64_t pts) { pts_us_ = pts; }

    /**
     * @brief 获取解码时间戳（微秒）
     * @return int64_t 已经换算为微秒的 DTS
     * @note 对于无 B 帧的流，如Baseline H.264，DTS = PTS
     */
    int64_t GetDtsUs() const { return dts_us_; }

    /**
     * @brief 设置解码时间戳（微秒）
     * @param dts 微秒级时间戳（由 Demuxer 从 AVPacket.dts 换算）
     */
    void SetDtsUs(int64_t dts) { dts_us_ = dts; }

    /**
     * @brief 获取世代号
     * @return uint64_t 当前 Packet 所属的 Seek 世代（从 SyncContext 同步）
     */
    uint64_t GetEpoch() const { return epoch_; }

    /**
     * @brief 设置世代号
     * @param epoch Demuxer 从 SyncContext::GetEpoch() 读取并写入
     */
    void SetEpoch(uint64_t epoch) { epoch_ = epoch; }

    /**
     * @brief 获取标志位
     * @return AVFrameFlag 包含 KeyFrame/EOF/Flush 等标志
     */
    AVFrameFlag GetFlags() const { return flags_; }

    /**
     * @brief 设置标志位（覆盖）
     * @param flag 新的标志位
     */
    void SetFlags(AVFrameFlag flags) { flags_ = flags; }

    /**
     * @brief 检查是否包含标志位
     * @param flag 要检查的标志 如（AVFrameFlag::kKeyFrame）
     * @return true，包含该标志位，false，不含该标志位
     */
    bool HasFlag(AVFrameFlag flag) const { return (flags_ & flag) != AVFrameFlag::kNone; }

    /**
     * @brief 追加标志（不影响已有标志）
     * @param flag 要追加的标志位
     * @details 用法：pkt.AddFlag(AVFrameFlag::kFlush); // 保留已有 kKeyFrame
     */
    void AddFlag(AVFrameFlag flag) { flags_ |= flag; }

    /**
     * @brief 移除标志（不影响其他标志）
     * @param flag 要移除的标志位
     * @details 用法：pkt.RemoveFlag(AVFramFlag::kFlush)
     */
    void RemoveFlag(AVFrameFlag flag) { flags_ &= ~flag; }

    /**
     * @brief 是否为空包（分配失败或已被移动）
     * @return true 表示pkt_ == nullptr
     */
    bool IsEmpty() const { return pkt_ == nullptr; }

private:
    AVPacket* pkt_; ///< FFmpeg 原生包，通过 av_packet_alloc/free 管理
    int64_t pts_us_; ///< 展示时间戳（微秒，已从 time_base 换算）
    int64_t dts_us_; ///< 解码时间戳（微秒，已从 time_base 换算）
    uint64_t epoch_; ///< Seek 世代号（从 SyncContext 同步，用于作废旧包）
    AVFrameFlag flags_; ///< 标志位（KeyFrame/EOF/Flush，支持位运算组合）
};

} // namespace mediacore

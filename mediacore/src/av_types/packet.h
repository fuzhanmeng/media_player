/**
 * @file packet.h
 * @brief RAII 封装的 AVPacket，携带微秒时间戳、Seek 世代号和控制标志
 * @details
 * Demuxer 从容器中读出压缩数据包，通过 Link<Packet> 传递给 Decoder 消费
 * pts/dts 统一换算为微秒，epoch_ 用于 Seek 后丢弃旧世代包，flags_ 承载关键帧/刷新等带内信令
 * 正常播放结束不需要给 Packet 单独塞 EOF 标志，交由 Link::Close() 传播
 */

#pragma once

#include <cstdint>
#include <optional>

#include "mediacore/av_types/media_data_flag.h"

struct AVPacket;

namespace mediacore {

/**
 * @brief 压缩数据包 DTO， RAII 封装的 AVPacket*
 * @details
 * - 独占所有权：禁止拷贝，仅支持移动
 * - 时间戳统一微秒：下游无需关心 time_base
 * - epoch 字段：Seek 时递增，下游丢弃旧世代包
 * - 带内信令：通过 flags 传递 kKeyFrame/Flush，无需额外通道
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
     * @details 分配失败时 pkt_ 保持为 nullptr，调用方需要用 IsEmpty() 判断
     */
    Packet();

    /**
     * @brief 析构函数：释放 AVPacket 内存
     * 这里只释放 FFmpeg 包体，不负责通知下游，也不负责改动队列
     */
    ~Packet();

    /** @brief 禁止拷贝，避免 AVPacket 被重复释放。 */
    Packet(const Packet&) = delete;
    /** @brief 禁止拷贝赋值，避免 AVPacket 被重复释放。 */
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
     * @brief 判断当前 packet 包是否含有 pts
     * @return true 表示含有 pts，false 表示不含 pts
     */
    bool HasPts() const noexcept { return pts_us_.has_value(); }

    /**
     * @brief 判断当前 packet 包是否含有 dts
     * @return true 表示含有 dts，false 表示不含 dts
     */
    bool HasDts() const noexcept { return dts_us_.has_value(); }

    /**
     * @brief 获取展示时间戳
     * @return 有值时返回微秒时间戳（PTS，Presentation Time Stamp），没有时返回空值
     */
    std::optional<std::int64_t> GetPtsUs() const { return pts_us_; }

    /**
     * @brief 获取解码时间戳
     * @return 有值时返回微秒时间戳，没有时返回空值
     */
    std::optional<std::int64_t> GetDtsUs() const noexcept { return dts_us_; }

    /**
     * @brief 设置展示时间戳
     * @param pts 微秒级时间戳；如果没拿到时间戳，就传空值
     */
    void SetPtsUs(std::optional<std::int64_t> pts) noexcept { pts_us_ = pts; }
    /**
     * @brief 设置解码时间戳
     * @param dts 微秒级时间戳；如果没拿到时间戳，就传空值
     */
    void SetDtsUs(std::optional<std::int64_t> dts) noexcept { dts_us_ = dts; }

    /**
     * @brief 清空 PTS，表示这包没有可用展示时间戳
     */
    void ClearPtsUs() noexcept { pts_us_.reset(); }

    /**
     * @brief 清空 DTS，表示这包没有可用解码时间戳
     */
    void ClearDtsUs() noexcept { dts_us_.reset(); }

    /**
     * @brief 获取世代号
     * @return 读取本包所属的 Seek 世代号
     */
    std::uint64_t GetEpoch() const { return epoch_; }

    /**
     * @brief 设置世代号
     * @param epoch Demuxer 从 SyncContext::GetEpoch() 读取并写入
     */
    void SetEpoch(uint64_t epoch) { epoch_ = epoch; }

    /**
     * @brief 获取标志位
     * @return MediaDataFlag 包含 KeyFrame/EOF/Flush 等标志
     */
    MediaDataFlag GetFlags() const { return flags_; }

    /**
     * @brief 设置标志位（覆盖）
     * @param flag 新的标志位
     */
    void SetFlags(MediaDataFlag flags) { flags_ = flags; }

    /**
     * @brief 检查是否包含标志位
     * @param flag 要检查的标志 如（MediaDataFlag::kKeyFrame）
     * @return true，包含该标志位，false，不含该标志位
     */
    bool HasFlag(MediaDataFlag flag) const { return (flags_ & flag) != MediaDataFlag::kNone; }

    /**
     * @brief 追加标志（不影响已有标志）
     * @param flag 要追加的标志位
     * @details 用法：pkt.AddFlag(MediaDataFlag::kFlush); // 保留已有 kKeyFrame
     */
    void AddFlag(MediaDataFlag flag) { flags_ |= flag; }

    /**
     * @brief 移除标志（不影响其他标志）
     * @param flag 要移除的标志位
     * @details 用法：pkt.RemoveFlag(AVFramFlag::kFlush)
     */
    void RemoveFlag(MediaDataFlag flag) { flags_ &= ~flag; }

    /**
     * @brief 是否为空包（分配失败或已被移动）
     * @return true 表示pkt_ == nullptr
     */
    bool IsEmpty() const { return pkt_ == nullptr; }

private:
    AVPacket* pkt_{nullptr};  ///< FFmpeg 原生包，通过 av_packet_alloc/free 管理
    std::optional<std::int64_t> pts_us_{};  ///< 展示时间戳（微秒，已从 time_base 换算）
    std::optional<std::int64_t> dts_us_{};  ///< 解码时间戳（微秒，已从 time_base 换算）
    std::uint64_t epoch_{0};  ///< Seek 世代号（从 SyncContext 同步，用于作废旧包）
    MediaDataFlag flags_{MediaDataFlag::kNone};  ///< 标志位
};

}  // namespace mediacore

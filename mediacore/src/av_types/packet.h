/**
 * @file packet.h
 * @brief RAII 封装的 AVPacket，携带微秒时间戳、Seek 世代号和控制标志
 * @details
 * Packet 是 AVPacket 的独占 RAII 包装，外加从流时间基换算好的微秒级时间戳和媒体世代号
 * 它只允许移动、禁止拷贝，跨线程通过队列传递时用 move 转移所有权
 * Demuxer 填充内容后调用 SetEpoch，Decoder 读 GetAVPacket 送 avcodec_send_packet
 */

#pragma once

#include <cstdint>
#include <optional>

struct AVPacket;

namespace mediacore {

class FfmpegDemuxer;

/**
 * @brief 独占 AVPacket 的 move-only 包装器
 */
class Packet final {
public:
    /**
     * @brief 创建一个空 Packet
     * @throws std::bad_alloc
     * @details av_packet_alloc 只分配结构体，不分配 data 缓冲；data 由 av_read_frame 填充
     */
    Packet();

    /**
     * @brief 释放底层 AVPacket
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
     * @brief 底层 AVPacket 只读指针
     * @note 只能通过 const Packet 调用，avcodec_send_packet 收 const AVPacket*
     * 因此 Decoder 侧通常走这个重载
     */
    const AVPacket* GetAVPacket() const noexcept { return packet_; }

    /**
     * @brief 底层 AVPacket 可写指针
     * @note 通过非 const Packet 调用，av_read_frame 会替换 packet->data 指向的缓冲
     * 因此 Demuxer 填充阶段会获取这个写指针
     */
    AVPacket* GetAVPacket() noexcept { return packet_; }

    /**
     * @brief 判断是否含有有效的 Packet
     * @return 有效返回 true
     */
    bool IsValid() const noexcept { return packet_ != nullptr; }

    /**
     * @brief 返回容器的流索引ID
     * @return stream_index
     */
    int GetStreamIndex() const noexcept { return stream_index_; }

    /**
     * @brief 返回压缩数据大小
     * @return size_bytes
     */
    std::size_t GetSizeBytes() const noexcept { return size_bytes_; }

    /**
     * @brief 判断是否是关键帧
     * @return 是关键帧返回 true，否则返回 false
     */
    bool IsKeyFrame() const noexcept { return key_frame_; }

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
    std::optional<std::int64_t> GetPtsUs() const noexcept { return pts_us_; }

    /**
     * @brief 获取解码时间戳
     * @return 有值时返回微秒时间戳，没有时返回空值
     */
    std::optional<std::int64_t> GetDtsUs() const noexcept { return dts_us_; }

    /**
     * @brief 返回包时长，单位微秒，未知时无值
     */
    std::optional<std::int64_t> GetDurationUs() { return duration_us_; }

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
     * @brief 是否为空包（分配失败或已被移动）
     * @return true 表示packet_ == nullptr
     */
    bool IsEmpty() const { return packet_ == nullptr; }

private:
    friend class FfmpegDemuxer;

    struct DemuxMetadata final {
        int stream_index_{-1};  ///< 本包来自容器中的哪一条流
        std::size_t size_bytes_{0};  ///< 压缩数据的字节数
        bool key_frame_{false};  ///< AV_PKT_FLAG_KEY 是否置位
        std::optional<std::int64_t> pts_us_{};  ///< 展示时间戳（微秒），缺失时为空
        std::optional<std::int64_t> dts_us_{};  ///< 解码时间戳（微秒），缺失时为空
        std::optional<std::int64_t> duration_us_{};  ///< 包时长（微秒），缺失时为空
    };

    /**
     * @brief 清空解复用由元数据
     */
    void ResetDemuxMetadata() noexcept;

    /**
     * @brief 写入解复用元素数据
     */
    void SetDemuxMetadata(DemuxMetadata metadata) noexcept;

    AVPacket* packet_{nullptr};  ///< 底层 AVPacket
    int stream_index_{-1};  ///< 容器流索引
    std::size_t size_bytes_{0};  ///< 压缩数据大小
    bool key_frame_{false};  ///< 是否关键帧
    std::optional<std::int64_t> pts_us_{};  ///< 展示时间戳（微秒）
    std::optional<std::int64_t> dts_us_{};  ///< 解码时间戳（微秒）
    std::optional<std::int64_t> duration_us_{};  ///< 包时长（微秒）
    std::uint64_t epoch_{0};  ///< 媒体世代号
};

}  // namespace mediacore

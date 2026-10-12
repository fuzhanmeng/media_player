/**
 * @file stream_item.h
 * @brief 媒体流水线数据传输载体与带内时序控制信令定义
 * @details
 * 流水线各节点间通过 Link 传递的并非裸数据对象，而是基于 std::variant 封装的三选一变体 StreamItem：
 * 1. 媒体承载数据（Media Payload）：T（如 Packet、VideoFrame、AudioFrame 等）；
 * 2. 流重置信令（StreamReset）：通知下游清空内部历史状态并开始处理 Seek 后的新世代媒体流；
 * 3. 流结束信令（EndOfStream）：通知下游当前世代的所有数据已按序完全送达。
 *
 * 架构设计原理（In-band Signaling）：
 * Seek 翻转与 EOF 都是与数据帧严格保序的时序事件。若仅通过“清空队列”或“关闭通道”等带外手段表达，
 * 会在多线程队列与解码器缓存中引发致命的竞态穿越（Race Condition）。
 * 将控制信令与普通数据置于同一个 FIFO 队列中排队，确保了下游节点接收重置与结束信号时，
 * 前序历史数据已被完整消费，后序新流绝不提前越界。
 */

#pragma once

#include <cstdint>
#include <variant>

namespace mediacore {

/**
 * @struct StreamReset
 * @brief 带内流重置信令：通知下游开始处理一次 Seek 跳转后的新数据流
 * @details
 * 1. 下游节点（如 DecoderNode、VideoSinkNode）收到此信令后，必须重置内部缓存；
 * 2. 携带的 epoch_ 标识新媒体世代，target_us_ 用于时钟重新对齐或首帧过滤。
 */
struct StreamReset final {
    std::uint64_t epoch_{0};  ///< Seek 后启用的新媒体世代号
    std::int64_t target_us_{0};  ///< Seek 目标呈现时间戳（微秒）
};

/**
 * @struct EndOfStream
 * @brief 带内流结束信令：指示指定世代的媒体流已全部按序传输完毕
 * @details
 * 1. 只有上游在完成全流读取与排空（Drain）后才会下发；
 * 2. 下游节点在依次消费完队列中位于其前面的所有数据后，方才感知流结束。
 */
struct EndOfStream final {
    std::uint64_t epoch_{0};  ///< 已有序处理完毕的媒体世代号
};

/**
 * @brief Link 上传递的统一样式数据流载体（Variant 变体类型）
 * @tparam T 具体承载的媒体载荷类型（如 Packet 或 VideoFrame）
 */
template <typename T>
using StreamItem = std::variant<T, StreamReset, EndOfStream>;

}  // namespace mediacore
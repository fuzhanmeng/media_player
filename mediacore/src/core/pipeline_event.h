/**
 * @file pipeline_event.h
 * @brief Node 向 Pipeline 汇报异步控制结果的事件类型与回调集合定义
 * @details
 * 负责打通节点后台工作线程与上层控制平面的异步通信路径：
 * 1. 触发时序与上下文：各事件均在各 Node 的独立后台工作线程（Worker Thread）中同步触发；
 * 2. 非阻塞执行契约：所有回调实现必须保持极致轻量（如仅封装为 Message 投递到 Player 的 Mailbox 队列），
 *    严禁在回调内部执行耗时计算、跨线程加锁等待或阻塞式 I/O，以防挂死数据流水线；
 * 3. 世代隔离（Epoch Validation）：MediaEnded 与 SeekCompleted 携带产生该事件的具体媒体流世代号，
 *    供 Pipeline 或 Player 过滤已被新 Seek 取代的历史作废信令。
 */

#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string>

#include "mediacore/media_error.h"

namespace mediacore {

/**
 * @struct MediaEnded
 * @brief 媒体流按序完全呈现并播放结束的事件
 * @details 由终端节点（如 VideoSinkNode）在消费完当前世代的 EndOfStream 且完成最后一帧渲染后向外触发。
 */
struct MediaEnded final {
    std::uint64_t epoch_{0};  ///< 已完成播放的媒体世代版本号
    std::string node_name_;  ///< 上报本事件的源节点名称（通常为终端渲染节点）
};

/**
 * @struct NodeError
 * @brief 节点工作线程遭遇未恢复致命异常的错误事件
 * @details
 * 与普通的带有作废语义的 Packet / EOS / SeekCompleted 不同，NodeError 通常意味着
 * 某个节点的后台工作线程无法继续正常工作（如硬件解码崩溃、严重内存耗尽）。
 * epoch_ 仅记录出错时所处的数据版本，该错误不可被随意忽略，Pipeline 需根据错误严重性决定是否终止管线。
 */
struct NodeError final {
    std::string node_name_;  ///< 发生异常报错的节点名称
    std::exception_ptr exception_;  ///< 可选捕获的底层原始 C++ 异常指针
    std::string message_;  ///< 人类可读的详细错误描述
    std::uint64_t epoch_{0};  ///< 发生错误时数据流所绑定的世代版本号
    MediaError media_error_{};  ///< 可选的结构化领域错误对象（通用框架错误时保持默认未初始化状态）
};

/**
 * @struct SeekCompleted
 * @brief 时间跳转（Seek）完成、目标帧或兜底帧已成功交付显示端的通知事件
 */
struct SeekCompleted final {
    std::uint64_t epoch_{0};  ///< 完成定位的媒体世代版本号
    std::int64_t best_effort_position_us_{0};  ///< 实际交付上屏呈现的定位帧 PTS 时间戳（微秒）
    bool reached_eof_{false};  ///< 是否因跳转越界或文件末尾而直接触底（使用末尾兜底帧完成定位）
};

/**
 * @struct PipelineEventCallbacks
 * @brief 注入给拓扑中各 Node 的统一事件回调出口集合
 * @note 回调函数在 Node 的工作线程内部被同步调用，必须实现为非阻塞的无挂起操作。
 */
struct PipelineEventCallbacks final {
    using MediaEndedCallback = std::function<void(MediaEnded)>;
    using NodeErrorCallback = std::function<void(NodeError)>;
    using SeekCompletedCallback = std::function<void(SeekCompleted)>;

    MediaEndedCallback on_media_ended_;  ///< 全流播放完毕事件出口
    NodeErrorCallback on_node_error_;  ///< 节点异常致命错误上报出口
    SeekCompletedCallback on_seek_completed_;  ///< 定位首帧交付完成事件出口
};

}  // namespace mediacore
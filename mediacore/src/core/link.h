/**
 * @file link.h
 * @brief 强类型连接器 Link<T>——DAG 中连接 "上游节点输出" 与 "下游节点输入" 的语义化管道
 * @details 在底层 ThreadSafeQueue<T> 有界阻塞队列 + 背压 + Abort/Close 之上，
 *          封装出面向数据流语义的一对方法：
 *            - Send(T)  ： 上游把一个数据项投入管道
 *            - Receive()： 下游从管道中取出下一个数据项
 *
 * @par 为什么要有 Link，而不是直接用 ThreadSafeQueue？
 *   1. 强类型固定拓扑：Link<Packet> 与 Link<VideoFrame> 是不同类型，把 "产 Packet 的输出"
 *      误接到 "吃 VideoFrame 的输入" 会在编译期报错。
 *   2. 语义收敛：节点代码只看到 Send/Receive/CloseInput/Abort 这组数据流动词，
 *      不直接触碰队列的 Push/Pop 细节，抽象层次更清晰，也便于 D9 后续接入 epoch/统计而不改调用方。
 *   3. 所有权模型：一条 Link 由上下游两个节点共同持有（各握一个 std::shared_ptr<Link<T>>，
 *      指向同一对象）。生产节点调用发送端、消费节点调用接收端，Link 随最后一个持有者一起析构，
 *      天然规避 "队列被谁释放" 的生命周期竞态。
 */
#pragma once

#include <cstddef> // std::size_t
#include <optional>
#include <utility> // std::move

#include "utils/thread_safe_queue.h"

namespace mediacore {

/**
 * @class Link
 * @brief 连接两个节点的强类型、有界、可终止的单向管道
 * @tparam T 流经管道的数据项类型，DAG 中通常是 std::shared_ptr<Packet/VideoFrame> 等轻量对象
 */
template <typename T>
class Link {
public:
    /**
     * @brief 构造一条容量为 capacity 的管道。
     * @param capacity 管道可缓冲的数据项上限（背压阈值），必须显式指定！
     */
    explicit Link(std::size_t capacity) : queue_(capacity) {}

    /**
     * @brief 析构函数默认即可
     */
    ~Link() = default;

    // Link 内嵌不可拷贝/移动的 ThreadSafeQueue，故自身同样不可拷贝/移动；
    // 跨节点共享一律通过 std::shared_ptr<Link<T>> 完成。
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    Link(Link&&) = delete;
    Link& operator=(Link&&) = delete;

    /**
     * @brief 上游发送一个数据项，管道满时阻塞（背压）
     * @param item 上游发送的数据项（按值接收后 std::move 移入，支持零拷贝）
     * @return true 成功送入； false 管道已 Close / Abort，拒绝接收
     */
    bool Send(T item) { return queue_.Push(std::move(item)); }

    /**
     * @brief 下游接收一个数据项，管道空的时候阻塞，直到有数据或者被 Close / Abort
     * @return 含值的 optional 表示读取到数值；std::nullopt 表示管道已终止且无数据可读
     */
    std::optional<T> Receive() { return queue_.Pop(); }

    /**
     * @brief 温和关闭发送端：上游数据发送完后调用，禁止再 Send，但是下游可以继续 Receive 排空存量
     * @note 对应 EOF / 正常结束场景
     */
    void CloseInput() { queue_.Close(); }

    /**
     * @brief 强制终止：立即唤醒收发两端，Send/Receive 一律失败，管道内存量数据按丢弃处理
     * @note 对应 Seek / 关闭播放器等需要立即打断等待的强制操作
     */
    void Abort() { queue_.Abort(); }

    /**
     * @brief 获取 queue 内缓冲的数据个数（瞬时快照）
     * @warning 返回即过期，不可用作“先查再操作”的同步依据，仅供调试或统计
     */
    std::size_t Depth() const { return queue_.Size(); }

    /**
     * @brief 管道是否已被 Close（温和关闭）
     */
    bool IsClosed() const { return queue_.IsClosed(); }

    /**
     * @brief 管道是否已被 Abort（强制中止）
     */
    bool IsAborted() const { return queue_.IsAborted(); }

private:
    /// 底层线程安全的有界阻塞队列： Link 的一切并发/背压/停止语义由它来实现
    ThreadSafeQueue<T> queue_;
};

} // namespace mediacore    
/**
 * @file link.h
 * @brief Pipeline 节点之间传递数据的强类型、有界单向连接
 *
 * @details
 * Link 是 ThreadSafeQueue<T> 的类型安全包装，负责在相邻 Pipeline Node
 * 之间传递数据，并通过内部有界队列提供线程安全的发送、接收和背压控制
 *
 * 主要职责：
 * 1. 数据传输：提供 Send()、Receive() 等接口，在上下游节点之间传递数据
 * 2. 有界背压：队列满时限制生产者继续发送，避免数据无限堆积
 * 3. 超时收发：提供 SendFor()、ReceiveFor()，使工作线程能够定期返回并检查控制状态
 * 4. 统一管理：通过非模板接口 ILinkControl，让 Pipeline 可以统一管理不同数据类型的 Link
 *
 * Link 的结束与清理语义：
 * - Flush()：清空当前队列，但不终止 Link，之后仍然可以继续发送和接收
 * - Abort()：永久终止 Link，清空队列并唤醒阻塞的发送和接收线程
 * - EndOfStream：正常的媒体数据结束通过 StreamItem 中的 EndOfStream 传递，
 *   不通过关闭或终止 Link 表达
 *
 * 注意：
 * Link 本身不创建工作线程，也不负责数据的解码、同步或其他媒体处理
 * 这些工作由上下游 Node 完成
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#include "mediacore/logger.h"
#include "utils/thread_safe_queue.h"

namespace mediacore {

class Pipeline;

/**
 * @class LinkCreationToken
 * @brief Pipeline 创建 Link 时使用的构造凭证
 *
 * @details
 * Link 构造函数要求传入 LinkCreationToken
 * 该令牌的默认构造函数是 private，只有 Pipeline 可以创建初始令牌
 *
 * Pipeline::CreateLink() 创建令牌并传给 Link 构造函数，
 * 从而限制 Link 只能通过 Pipeline 提供的创建入口进行创建和登记
 */
class LinkCreationToken final {
public:
    /// 允许复制令牌
    LinkCreationToken(const LinkCreationToken&) = default;

    /// 允许复制赋值
    LinkCreationToken& operator=(const LinkCreationToken&) = default;

    /// 允许移动令牌
    LinkCreationToken(LinkCreationToken&&) = default;

    /// 允许移动赋值
    LinkCreationToken& operator=(LinkCreationToken&&) = default;

private:
    friend class Pipeline;

    /**
     * @brief 创建 Link 的构造凭证
     * @note 仅 Pipeline 可以调用，外部代码不能直接创建默认令牌
     * @details 
     * 显式提供构造函数，使该类型在 C++17 下不属于聚合类型，避免通过聚合初始化绕过私有默认构造函数的访问限制
     */
    LinkCreationToken() noexcept {}
};

// 确保令牌不是聚合类型，避免通过聚合初始化绕过私有构造限制
static_assert(
    !std::is_aggregate_v<LinkCreationToken>,
    "LinkCreationToken must not be an aggregate in C++17");

// 确保外部代码无法直接默认构造令牌
static_assert(
    !std::is_default_constructible_v<LinkCreationToken>,
    "LinkCreationToken default construction must remain private");


/**
 * @class ILinkControl
 * @brief Pipeline 统一管理不同类型 Link 的非模板控制接口
 *
 * @details
 * 不同模板参数会产生不同的 Link 类型，例如：
 *
 * Link<Packet>
 * Link<VideoFrame>
 *
 * 这些类型无法直接作为同一种类型存入一个容器
 *
 * 因此，Pipeline 使用 shared_ptr<ILinkControl> 保存不同 Link 的控制接口，
 * 不需要知道 Link 内部具体存储的是什么数据类型
 *
 * Pipeline 可以通过该接口统一执行：
 * - Flush()：清空 Link 中的待处理数据
 * - Abort()：永久终止 Link，并唤醒阻塞的收发线程
 *
 * 这两个控制操作只允许 Pipeline 调用，普通 Node 只能通过 Link 的公开收发接口传输数据
 */
class ILinkControl {
public:
    /// 支持通过基类指针安全析构具体的 Link 对象
    virtual ~ILinkControl() = default;

protected:
    /**
     * @brief 构造控制接口的基类部分
     * @note 仅供派生类 Link 构造时使用
     */
    ILinkControl() = default;

private:
    friend class Pipeline;

    // 禁止复制和移动，保证 Link 控制对象的身份与管理关系稳定
    ILinkControl(const ILinkControl&) = delete;
    ILinkControl& operator=(const ILinkControl&) = delete;
    ILinkControl(ILinkControl&&) = delete;
    ILinkControl& operator=(ILinkControl&&) = delete;

    /**
     * @brief 清空 Link 中当前缓存的数据，但保持 Link 可继续使用
     * @note 仅允许 Pipeline 调用
     */
    virtual void Flush() = 0;

    /**
     * @brief 永久终止 Link，清空缓存并唤醒阻塞的收发线程
     * @note 仅允许 Pipeline 调用
     */
    virtual void Abort() = 0;
};


/**
 * @class Link
 * @brief 使用有界线程安全队列传输数据的单向连接
 *
 * @tparam T 队列中存储的数据类型，例如 Packet、StreamItem<Packet> 或 VideoFrame
 *
 * @details
 * Link 连接 Pipeline 中的上游 Node 和下游 Node：
 *
 * - 上游 Node 通过 Send() 或 SendFor() 向队列写入数据
 * - 下游 Node 通过 Receive() 或 ReceiveFor() 从队列读取数据
 *
 * Link 内部使用 ThreadSafeQueue<T> 保存数据，因此具备有界队列、
 * 生产者阻塞、消费者阻塞和 Abort 等能力
 *
 * Pipeline 与相邻 Node 可以通过 std::shared_ptr 共同持有 Link
 * Link 本身不创建线程，具体的数据处理由上下游 Node 的工作线程完成
 */
template <typename T>
class Link final : public ILinkControl {
public:
    /**
     * @brief 构造一个指定容量的 Link
     *
     * @param capacity 内部队列允许缓存的最大元素数量，必须大于 0
     * @param token 由 Pipeline 创建的 Link 构造凭证
     *
     * @throws std::invalid_argument 当 capacity 为 0 时，由 ThreadSafeQueue 抛出
     */
    Link(std::size_t capacity, LinkCreationToken token)
        : queue_(capacity) {
        // token 仅用于限制 Link 的创建入口，本身不参与数据传输
        (void)token;

        LOG_INFO("Link constructed, capacity {}", capacity);
    }

    /**
     * @brief 析构 Link 并释放内部队列资源
     *
     * @note 析构前必须确保上下游 Node 已经停止使用该 Link，
     *       并且不存在仍阻塞在队列收发操作中的工作线程
     *       通常由 Pipeline 先 Abort 队列，再等待相关工作线程退出
     */
    ~Link() override {
        LOG_INFO("Link destroyed");
    }

    // Link 由 Pipeline 创建并管理，不允许复制或移动
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    Link(Link&&) = delete;
    Link& operator=(Link&&) = delete;


    /**
     * @brief 向 Link 队尾发送一个数据项
     *
     * @param item 待发送的数据项
     *
     * @return true 表示成功入队；false 表示 Link 已经 Abort
     *
     * @details
     * 内部调用 ThreadSafeQueue::Push()：
     * - 队列有空位时，立即将数据放入队列
     * - 队列已满时，阻塞等待下游消费者取走数据
     * - 队列被 Abort 时，返回 false
     *
     * 数据成功入队后，等待中的下游消费者会被唤醒
     *
     * @note 正常的 EndOfStream 是普通的数据项，由 StreamItem 表达；
     *       false 表示队列被终止，不表示媒体播放结束
     */
    bool Send(T item) {
        return queue_.Push(std::move(item));
    }


    /**
     * @brief 尝试发送数据，队列满时最多等待指定时间
     *
     * @param item 待发送的数据，仅成功入队时才会被移动
     * @param timeout 最长等待时间
     *
     * @return kPushed：发送成功；
     *         kTimedOut：等待超时，数据仍保留在调用者手中；
     *         kAborted：Link 已经终止，未执行入队
     *
     * @note 限时阻塞发送，主要用于 Seek 预滚，
     *       避免某一路队列长期满载而阻塞 Demuxer 工作线程
     */
    QueuePushResult SendFor(
        T& item,
        std::chrono::milliseconds timeout) {
        return queue_.PushFor(item, timeout);
    }


    /**
     * @brief 从 Link 队首接收一个数据项
     *
     * @return 成功时返回包含数据项的 std::optional<T>；
     *         Link 已经 Abort 时返回 std::nullopt
     *
     * @details
     * 内部调用 ThreadSafeQueue::Pop()：
     * - 队列有数据时，立即取出队首元素
     * - 队列为空时，阻塞等待上游生产者发送数据
     * - 队列被 Abort 时，返回 std::nullopt
     *
     * @note std::nullopt 表示 Link 已终止，不表示媒体 EOF
     *       正常媒体结束通过 StreamItem::EndOfStream 传递
     */
    std::optional<T> Receive() {
        return queue_.Pop();
    }


    /**
     * @brief 尝试接收数据，队列为空时最多等待指定时间
     *
     * @param timeout 最长等待时间
     *
     * @return 成功时返回取出的数据项；
     *         等待超时或 Link 已经 Abort 时返回 std::nullopt
     *
     * @note 限时阻塞接收，适用于需要定期检查 Pause、Stop 或时钟状态的工作线程，
     *       避免因无限期等待队列数据而延迟处理控制状态
     *
     *       当前返回类型无法区分等待超时和 Abort，需要调用者结合自身停止状态判断
     */
    std::optional<T> ReceiveFor(
        std::chrono::milliseconds timeout) {
        return queue_.PopFor(timeout);
    }

    /**
     * @brief 查询队列是否已被 Pipeline 终止
     * @note 用于区分"限时接收超时"与"Link 已 Abort"
     */
    bool IsAborted() const { return queue_.IsAborted(); }

private:
    // Pipeline 可以通过 ILinkControl 和具体 Link 执行统一管理操作
    friend class Pipeline;

    /**
     * @brief 清空队列中已有的数据，但不终止 Link
     *
     * @details
     * 清空后，Link 仍然可以继续 Send() 和 Receive()
     * 主要用于 Seek 等需要丢弃旧数据的场景
     */
    void Flush() override {
        LOG_INFO("Link flush");
        queue_.Flush();
    }

    /**
     * @brief 永久终止 Link，并清理队列中的待处理数据
     *
     * @details
     * Abort 后，后续发送操作会失败，阻塞中的发送和接收线程会被唤醒
     * Link 不支持从 Abort 状态恢复
     */
    void Abort() override {
        LOG_INFO("Link abort");
        queue_.Abort();
    }

private:
    // 实际的数据存储和线程同步由有界线程安全队列完成
    ThreadSafeQueue<T> queue_;
};

}  // namespace mediacore
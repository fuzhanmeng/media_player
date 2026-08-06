/**
 * @file link.h
 * @brief DAG 中传递强类型数据的有界单向边
 *
 * @details
 * Node 通过 Link<T>::Send() 和 Link<T>::Receive() 传递同一种类型的数据
 * Pipeline 通过 ILinkControl 保存不同 T 类型的 Link，并在 Stop() 中统一调用 Abort() 解除阻塞
 * Link 有两种结束方式：
 * 1. Close() 由上游发送方调用，禁止继续发送，但允许下游接收并排空已有数据；排空后 Receive() 返回 nullopt
 * 2. Abort() 由 Pipeline 调用，立即唤醒阻塞的发送方和接收方，并丢弃尚未处理的数据
 *
 * Link 是单向的：上游负责 Send() 和正常 Close()，下游只负责 Receive()
 * 因此没有 CloseInput()；接收方不能替上游宣布 EOF
 */
#pragma once

#include <cstddef> // std::size_t
#include <optional>
#include <type_traits>
#include <utility> // std::move

#include "utils/thread_safe_queue.h"

namespace mediacore {

class Pipeline;

/**
 * @class LinkCreationToken
 * @brief 限制 Link 只能由 Pipeline::CreateLink 创建和登记的构造令牌
 * @details Link 构造函数要求传入本类型令牌构造函数是 private，因此其他代码即使能看见 Link 的构造函数，
 *          也无法构造出合法的令牌
 */
class LinkCreationToken final {
public:
    // 因为要把 Token “按值传递”给 Link 的构造函数，甚至穿透 std::make_shared 的可变参数模板，
    // 所以一旦 Token 被合法制造出来，必须允许它被拷贝和移动
    LinkCreationToken(const LinkCreationToken&) = default;
    LinkCreationToken& operator=(const LinkCreationToken&) = default;

    LinkCreationToken(LinkCreationToken&&) = default;
    LinkCreationToken& operator=(LinkCreationToken&&) = default;

private:
    friend Pipeline;

    /**
     * @brief 私有构造函数，仅允许 Pipeline 创建令牌
     * @details 使用用户提供的构造函数，确保 C++17 不会把令牌识别为 aggregate，从而绕过访问控制
     */
    LinkCreationToken() noexcept {}
};

static_assert(!std::is_aggregate_v<LinkCreationToken>, "LinkCreationToken must not be an aggregate in C++17");
static_assert(!std::is_default_constructible_v<LinkCreationToken>,
              "LinkCreationToken default construction must remain private");

/**
 * @class ILinkControl
 * @brief Pipeline 用于统一停止不同 Link<T> 的非模板控制接口
 * @details
 * Link<int> 和 Link<Packet> 是不同的类型，不能直接放入同一个 vector，Pipeline 将它们转换为
 * shared_ptr<ILinkControl> 保存，因此 Stop() 不需要知道具体的数据类型，Abort() 是私有控制接口，
 * 只有 Pipeline 可以调用
 */
class ILinkControl {
public:
    virtual ~ILinkControl() = default;

    // Link 含有线程安全队列，控制接口本身不允许复制或移动
    ILinkControl(const ILinkControl&) = delete;
    ILinkControl& operator=(const ILinkControl&) = delete;
    ILinkControl(ILinkControl&&) = delete;
    ILinkControl& operator=(ILinkControl&&) = delete;

protected:
    /** @brief 只允许 Link 派生类构造控制接口部分 */
    ILinkControl() = default;

private:
    friend class Pipeline;

    /** @brief 立即中止 Link，并唤醒阻塞的发送方和接收方 */
    virtual void Abort() = 0;
};

/**
 * @class Link
 * @brief 携带 T 的强类型、有界、单向 DAG 数据边
 * @tparam T 流经管道的数据项类型，DAG 中通常是 std::shared_ptr<Packet/VideoFrame> 等轻量对象
 * @details Pipeline 和相邻的 Node 通过 std::shared_ptr 共同持有 Link，Link 本身不创建线程，背压、
 * 阻塞、 Close 和 Abort 语义都由内部的 ThreadSafeQueue 实现
 * @note DAG 中一条 Link 默认连接一个上游发送方和一个下游接收方，广播需要由 Tee Node 为每个下游创建独立 Link
 */
template <typename T>
class Link final : public ILinkControl {
public:
    /**
     * @brief 创建指定容量的数据边
     * @param capacity 最大缓存元素数（背压阈值），必须大于 0
     * @param token Pipeline 创建的通行证，不携带运行时数据，只用于限制构造位置
     * @throws std::invalid_argument capacity 为 0 时由 ThreadSafeQueue 构造函数抛出
     */
    Link(std::size_t capacity, LinkCreationToken token) : queue_(capacity) {
        // 通行证没有运行时数据；构造函数能够收到它，就说明调用方已经通过创建权限检查
        (void)token;
    }

    /**
     * @brief 销毁数据边
     * @note 销毁前 Pipeline 必须已经 Join 所有访问该 Link 的 Node
     */
    ~Link() override = default;

    // Link 内嵌不可拷贝/移动的 ThreadSafeQueue，故自身同样不可拷贝/移动；
    // 跨节点共享一律通过 std::shared_ptr<Link<T>> 完成
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    Link(Link&&) = delete;
    Link& operator=(Link&&) = delete;

    /**
     * @brief 由上游发送一个数据项，队列已满时等待空位
     * @param item 待发送的数据；成功时移动到队列中
     * @return true 成功送入； false 管道已 Close / Abort，拒绝接收
     */
    bool Send(T item) { return queue_.Push(std::move(item)); }

    /**
     * @brief 由下游接收一个数据项，队列为空时等待数据或结束通知
     * @return 有值 (has_value) 表示收到数据；nullopt 表示数据已枯竭 (EOF) 或管道已被强制 Abort
     */
    std::optional<T> Receive() { return queue_.Pop(); }

    /**
     * @brief 由上游正常关闭发送端并向下游传播 EOF
     * @details 关闭后 Send() 返回 false，队列中的存量仍可由下游 Receive() 排空
     * @note 上游 Node 正常结束时调用；用户停止或错误退出使用 Pipeline 的 Abort()
     */
    void Close() { queue_.Close(); }

    /**
     * @brief 返回当前缓存元素数
     * @return 加锁读取的瞬时值
     * @warning 返回即可能过期，不可用作“先查再操作”的同步依据，仅供调试或统计
     */
    std::size_t Depth() const { return queue_.Size(); }

    /**
     * @brief 查询 Link 是否已经正常关闭
     * @return Close() 已调用返回 true
     */
    bool IsClosed() const { return queue_.IsClosed(); }

    /**
     * @brief 查询 Link 是否已经立即中止
     * @return Pipeline 已调用 Abort() 返回 true
     */
    bool IsAborted() const { return queue_.IsAborted(); }

private:
    friend class Pipeline;

    /**
     * @brief 立即中止收发并停止处理缓存
     * @details Pipeline 通过 ILinkControl 调用本函数，Node 和其他业务代码无法访问
     */
    void Abort() override {
        // ThreadSafeQueue 先发布中止状态并通知等待线程，再处理缓存，保证 Stop 可以继续 Join
        queue_.Abort();
    }

    /// 保存数据并实现背压、正常关闭和立即中止；所有内部共享状态由队列自己的 mutex 保护
    ThreadSafeQueue<T> queue_;
};

} // namespace mediacore

/**
 * @file link.h
 * @brief 强类型、有界的单向数据 Link
 *
 * @details
 * Link 是 ThreadSafeQueue<T> 的强类型语义包装，Node 通过它 Send/Receice 数据
 * Pipeline 用非模板的 ILinkControl 统一保存不同类型的 Link，以便在 Stop 时 Abort
 * 在 Seek 时 Flush，而无需知道具体的数据类型
 *
 * 结束语义
 * Abort() 永久终止，唤醒阻塞的收发双方并丢弃存量，用于立即终止
 * Flush() 清空当前差存量，但 Link 仍然科技继续使用，用于 Seek 时丢弃旧世代的数据
 * 正常的媒体关闭不用 Link 关闭表达，而是由 StreamItem 的 EndOfStream 传递
 */
#pragma once

#include <cstddef>  // std::size_t
#include <optional>
#include <type_traits>
#include <utility>  // std::move

#include "mediacore/logger.h"
#include "utils/thread_safe_queue.h"

namespace mediacore {

class Pipeline;

/**
 * @class Pipeline 创建 Link 时使用的令牌
 * @brief 限制 Link 只能由 Pipeline::CreateLink 创建和登记的构造令牌
 * @details
 * Link 构造函数要求传入本类型令牌，而令牌的默认构造是 private,且只对 Pipeline 开放，因此只有
 * Pipeline::CreateLink() 能创造 Link，保证每条 Link都被登记进 Links_，Stop 时不会漏掉某条边
 */
class LinkCreationToken final {
public:
    /// @brief 复制令牌
    LinkCreationToken(const LinkCreationToken&) = default;
    /// @brief 复制赋值
    LinkCreationToken& operator=(const LinkCreationToken&) = default;
    /// @brief 移动令牌
    LinkCreationToken(LinkCreationToken&&) = default;
    /// @brief 移动赋值
    LinkCreationToken& operator=(LinkCreationToken&&) = default;

private:
    friend Pipeline;

    /**
     * @brief 创建令牌；仅 Pipeline 可调用
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
    /**
     * @brief 虚析构函数
     */
    virtual ~ILinkControl() = default;

protected:
protected:
    /**
     * @brief 构造控制接口,只允许 Link 派生类构造控制接口部分
     */
    ILinkControl() = default;

private:
    friend class Pipeline;

    /// @brief 禁止复制
    ILinkControl(const ILinkControl&) = delete;
    /// @brief 禁止复制赋值
    ILinkControl& operator=(const ILinkControl&) = delete;
    /// @brief 禁止移动
    ILinkControl(ILinkControl&&) = delete;
    /// @brief 禁止移动赋值
    ILinkControl& operator=(ILinkControl&&) = delete;

    /**
     * @brief 清空当前数据
     */
    virtual void Flush() = 0;

    /**
     * @brief 永久停止 Link 并唤醒阻塞的收发操作
     */
    virtual void Abort() = 0;
};

/**
 * @class Link
 * @brief 使用有界队列传输数据的单向 Link
 * @tparam T 流经管道的数据项类型，DAG 中通常是 std::shared_ptr<Packet/VideoFrame> 等轻量对象
 * @details
 * Pipeline 和相邻的 Node 通过 std::shared_ptr 共同持有 Link，Link 本身不创建线程，背压、阻塞、
 * Close 和 Abort 语义都由内部的 ThreadSafeQueue 实现
 */
template <typename T>
class Link final : public ILinkControl {
public:
    /**
     * @brief 构造指定容量的数据边
     * @param capacity 内部有界队列的最大元素数，必须大于 0
     * @param token Link 创建令牌
     * @throws std::invalid_argument capacity 为 0 时由 ThreadSafeQueue 抛出
     */
    Link(std::size_t capacity, LinkCreationToken token) : queue_(capacity) {
        (void)token;
        LOG_INFO("Link constructed, capacity {}", capacity);
    }

    /**
     * @brief 销毁 Link
     * @note 调用前应停止使用它的 Node
     */
    ~Link() override { LOG_INFO("Link destroyed"); }

    /**
     * @brief 发送一个数据项
     * @param item 待发送的数据项
     * @return 发送成功返回 true；Link 已 Abort 时返回 false
     * @note nullopt 只表示永久 Abort，不表示 EOF，EOF 由 StreamItem::EndOfStream 表达
     */
    bool Send(T item) { return queue_.Push(std::move(item)); }

    /**
     * @brief 接收一个数据项
     * @return 队列为空时阻塞；Link 已 Abort 时返回 nullopt
     * @note nullopt 只表示永久 Abort，不表示 EOF，EOF 由 StreamItem::EndOfStream 表达
     */
    std::optional<T> Receive() { return queue_.Pop(); }

private:
    friend class Pipeline;

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    Link(Link&&) = delete;
    Link& operator=(Link&&) = delete;

    /**
     * @brief 清空队列
     */
    void Flush() override {
        LOG_INFO("Link flush");
        queue_.Flush();
    }

    /**
     * @brief 停止队列
     */
    void Abort() override {
        LOG_INFO("Link abort");
        queue_.Abort();
    }

    /// Link 的有界数据队列
    ThreadSafeQueue<T> queue_;
};

}  // namespace mediacore

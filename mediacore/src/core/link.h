/**
 * @file link.h
 * @brief DAG 强类型数据边及其供 Pipeline 使用的非模板控制接口。
 *
 * @details
 * 【架构设计意图】：
 * 1. 数据平面：Node 使用强类型的 Link<T> 的 Send/Receive 传递数据（如 Packet, VideoFrame）
 * 2. 控制平面：Pipeline 仅通过 ILinkControl 保存这些异构的 Link
 * 3. 容错防御：在 Pipeline 立即停止 (Stop) 时，统一调用所有 Link 的 Abort
 *    这样，整个系统安全停止的正确性，不再依赖于各个业务 Node 的开发者是否“记得”在自己的类里去清理或中止 Link
 */
#pragma once

#include <cstddef> // std::size_t
#include <optional>
#include <utility> // std::move

#include "utils/thread_safe_queue.h"

namespace mediacore {

class Pipeline;

/**
 * @class LinkCreationToken
 * @brief 限制 Link 只能由 Pipeline::CreateLink 创建和登记的构造令牌,Passkey Idiom (通行证模式)
 */
class LinkCreationToken final {
public:
    // 因为要把 Token “按值传递”给 Link 的构造函数，甚至穿透 std::make_shared 的可变参数模板，
    // 所以一旦 Token 被合法制造出来，必须允许它被拷贝和移动。
    LinkCreationToken(const LinkCreationToken&) = default;
    LinkCreationToken& operator=(const LinkCreationToken&) = default;

private:
    friend Pipeline;

    /**
     * @brief 构造函数私有化
     */
    LinkCreationToken() = default;
};

/**
 * @class ILinkControl
 * @brief Type Erasure 类型擦除，抹去 Link<T> 上的泛型 T，提取出纯粹的生命周期控制接口
 * @details Pipeline 需要一个 std::vector 数组来集中保存并管理图中所有的 Link，但 Link<Packet> 和
 *          Link<VideoFrame> 是不同的类型，无法放进同一个数组，抽象出 ILinkControl 后，Pipeline 只需持有
 *          std::vector<std::shared_ptr<ILinkControl>> 即可、
 */
class ILinkControl {
public:
    virtual ~ILinkControl() = default;

    // 禁用拷贝构造函数 / 移动构造函数
    ILinkControl(const ILinkControl&) = delete;
    ILinkControl& operator=(const ILinkControl&) = delete;

    /**
     * @brief 立即中止该 DAG 边并唤醒收发双方
     */
    virtual void Abort() = 0;

protected:
    ILinkControl() = default;
};

/**
 * @class Link
 * @brief 携带 T 的强类型、有界、单向 DAG 数据边
 * @tparam T 流经管道的数据项类型，DAG 中通常是 std::shared_ptr<Packet/VideoFrame> 等轻量对象
 */
template <typename T>
class Link : public ILinkControl {
public:
    /**
     * @brief 创建指定容量的数据边
     * @param capacity 最大缓存元素数（背压阈值），必须大于 0
     * @param token 通行证，该参数没有名字，因为它是一个 purely syntactic marker， 仅用于编译时期权限校验
     */
    Link(std::size_t capacity, LinkCreationToken) : queue_(capacity) {}

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
     * @param item 上游发送的数据项；右值会移动入队，左值会先复制到该参数
     * @return true 成功送入； false 管道已 Close / Abort，拒绝接收
     */
    bool Send(T item) { return queue_.Push(std::move(item)); }

    /**
     * @brief 下游阻塞接收一个元素
     * @return 有值 (has_value) 表示收到数据；nullopt 表示数据已枯竭 (EOF) 或管道已被强制 Abort
     */
    std::optional<T> Receive() { return queue_.Pop(); }

    /**
     * @brief 关闭 Link，后续 Send() 返回 false，已经入队的数据仍可被 Receive() 取完
     * @note 上游 Node 正常结束时调用，用于传递 EOF；立即停止使用 Abort()
     */
    void Close() { queue_.Close(); }

    /**
     * @brief 立即中止 Link，唤醒阻塞的 Send() 和 Receive()，并丢弃尚未接收的数据
     * @note Abort 后 Link 不可恢复，只用于 Pipeline 停止
     */
    void Abort() override { queue_.Abort(); }

    /**
     * @brief 获取 queue 内缓冲的数据个数（瞬时快照）
     * @warning 返回即过期，不可用作“先查再操作”的同步依据，仅供调试或统计
     */
    std::size_t Depth() const { return queue_.Size(); }

    /**
     * @brief 查询正常关闭状态
     */
    bool IsClosed() const { return queue_.IsClosed(); }

    /**
     * @brief 查询立即中止状态
     */
    bool IsAborted() const { return queue_.IsAborted(); }

private:
    /// 底层线程安全的有界阻塞队列： Link 的一切并发/背压/停止语义由它来实现
    ThreadSafeQueue<T> queue_;
};

} // namespace mediacore

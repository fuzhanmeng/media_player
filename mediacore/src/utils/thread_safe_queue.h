/**
 * @file thread_safe_queue.h
 * @brief C++17 有界阻塞队列，提供背压、正常关闭和立即中止语义
 * @details
 * ThreadSafeQueue 是 Link<T> 的底层存储：队列满时 Push() 阻塞发送线程，队列空时 Pop() 阻塞接收线程
 * 所有共享数据都由 mutex_ 保护，两个 condition_variable 分别等待“未满”和“非空”
 *
 * 关闭方式分为两种：
 * 1. Close() 表示正常 EOF，禁止 Push()，但允许 Pop() 排空已有数据，若排空后又有新的 Push()
 * 队列会自动重新打开
 * 2. Abort() 表示立即停止，Push() / Pop() 都返回失败，并清空尚未处理的数据，且不可恢复
 */
#pragma once

#include <condition_variable>
#include <cstddef>  // std::size_t
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>  // 标准异常类
#include <utility>  // std::move / std::forward

#include "mediacore/logger.h"

namespace mediacore {

/**
 * @class ThreadSafeQueue
 * @brief 支持单/多生产者和单/多消费者的有界阻塞队列
 * @tparam T 元素类型，DAG 中通常都是 std::shared_ptr<Packet/VideoFrame> 等轻量句柄
 * @note 本类线程安全，但 Size()、IsClosed() 和 IsAborted() 返回的都只是调用时的瞬时快照
 */
template <typename T>
class ThreadSafeQueue {
public:
    /**
     * @brief 构造一个容量上限为 capacity 的有界队列
     * @param capacity 最大缓存元素数，必须 > 0
     * @throws std::invalid_argument capacity 为 0 时抛出，避免 Push 永久等待
     */
    explicit ThreadSafeQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            LOG_ERROR("ThreadSafeQueue construction failed: capacity must be greater than zero");
            throw std::invalid_argument("ThreadSafeQueue capacity must be greater than zero");
        } else {
            LOG_DEBUG("ThreadSafeQueue constructed, capacity {}", capacity_);
        }
    }

    /**
     * @brief 析构函数，销毁队列
     * @note 调用方必须保证已经没有线程在 Push() 或 Pop() 中等待
     */
    ~ThreadSafeQueue() = default;

    // 队列持有 mutex / condition_variable，不可拷贝、不可移动（这些同步原语本身不可拷贝/移动）
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

    /**
     * @brief 阻塞式入队，队列满时，挂起当前线程，直到有空位或者被 Abort/Close
     * @param value 待元素入队，按值接收后 std::move 进队列
     * @return true 成功入队， false 入队失败，表示队列已 Close 或 Abort
     */
    bool Push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 1. 如果 Abort 了，直接返回
        if (aborted_) {
            return false;
        }

        // 2. 如果队列已 close 并且队列中的数据被排空，那么就重新打开队列
        if (closed_ && queue_.empty()) {
            closed_ = false;
            LOG_DEBUG("ThreadSafeQueue auto-reopened: closed + empty -> accepting new data");
        }

        // 队列有空位时可以入队；Close/Abort 也会使等待结束
        if (capacity_ > 0) {
            not_full_cv_.wait(lock, [this]() { return queue_.size() < capacity_ || closed_ || aborted_; });
        }

        // 被唤醒不等于可以写入：Close/Abort 的通知同样会唤醒本线程
        if (closed_ || aborted_) {
            LOG_DEBUG("ThreadSafeQueue push rejected: closed {}, aborted {}", closed_, aborted_);
            return false;
        }

        // 在锁保护下入队，保证多个生产者不会同时修改 queue_
        queue_.push(std::move(value));
        const std::size_t size_after_push = queue_.size();

        // 修改共享数据后先解锁，再通知消费者，避免被唤醒线程立刻阻塞在同一把 mutex 上
        lock.unlock();
        not_empty_cv_.notify_one();
        LOG_TRACE("ThreadSafeQueue push completed, size {}/{}", size_after_push, capacity_);
        return true;
    }

    /**
     * @brief 阻塞式出队：队列空时挂起当前线程，直到有数据或者被 Abort/Close
     * @return 取到元素则返回 std::optional<T>；返回 std::nullopt 表示没有更多数据：
     *         Abort 时立即返回，Close 且队列排空后返回
     * @note 典型的消费循环：while (auto item = queue.Pop()) { process(std::move(*item)); }
     */
    std::optional<T> Pop() {
        std::unique_lock<std::mutex> lock(mutex_);

        // 醒来条件：有数据可取 / Abort / Close
        not_empty_cv_.wait(lock, [this]() { return !queue_.empty() || aborted_ || closed_; });

        // 醒来后检查是否强制终止
        if (aborted_) {
            LOG_DEBUG("ThreadSafeQueue pop aborted");
            return std::nullopt;
        }

        // 非 Abort 情况下，空队列只可能表示已 Close 且存量已经排空
        if (queue_.empty()) {
            LOG_DEBUG("ThreadSafeQueue pop reached end: queue closed and empty");
            return std::nullopt;
        }

        // 取出元素
        T value = std::move(queue_.front());
        queue_.pop();
        const std::size_t size_after_pop = queue_.size();

        // 唤醒一个生产者
        lock.unlock();
        not_full_cv_.notify_one();

        LOG_TRACE("ThreadSafeQueue pop completed, size {}", size_after_pop);
        return value;
    }

    /**
     * @brief 正常关闭：禁止再 Push，但保留存量供消费者排空，排空后 Pop 返回 std::nullopt
     * @details
     * 后续的 Push() 返回 false，Pop() 继续排空队列内的数据，排空后返回 std::nullopt
     * 若排空之后又调用 Push()，会按上面所述的自动重新打开逻辑恢复生产
     * @note 幂等，用于正常 EOF，不用于用户点击停止或错误退出
     */
    void Close() {
        std::size_t remaining = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            remaining = queue_.size();
        }

        // 唤醒所有因阻塞等待的线程
        not_full_cv_.notify_all();
        not_empty_cv_.notify_all();
        LOG_INFO("ThreadSafeQueue closed, remaining items {}", remaining);
    }

    /**
     * @brief 强制终止：立即唤醒所有等待者， Push/Pop 一律返回失败，未处理数据按 "丢弃" 语义处理
     * @details Push()/Pop() 在看到 aborted_ 后立即失败，不再处理缓存数据
     * @note 幂等；Abort 后队列不可恢复，只用于立即停止，不参与 Close 之后的自动重新打开
     */
    void Abort() {
        std::size_t discarded_count = 0;

        // 第一步：只做不可省略的同步操作，先让所有线程都能观察到中止状态
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
            discarded_count = queue_.size();
        }

        // 第二步：立即唤醒满队列上的发送者和空队列上的接收者
        not_full_cv_.notify_all();
        not_empty_cv_.notify_all();

        // 中止状态已经发布并完成通知后，再交换出缓存，这样即使回收阶段出现异常，解除阻塞的前两步也已经完成
        std::queue<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            discarded.swap(queue_);
        }
        LOG_INFO("ThreadSafeQueue aborted, discarded items {}", discarded_count);
    }

    /**
     * @brief 返回当前缓存元素数
     * @return 加锁读取的瞬时值
     * @note 返回后其他线程可能立即改变队列，不能把该值用于“先检查、再操作”的同步判断
     */
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief 查询队列是否已经正常关闭 Close()
     * @return Close() 已调用返回 true；若之后被自动重新打开，则返回 false
     */
    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    /**
     * @brief 查询队列是否已经立即中止 Abort()
     * @return Abort() 已调用返回 true
     */
    bool IsAborted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return aborted_;
    }

private:
    /// 保护 queue_、closed_ 和 aborted_；const 查询也需要加锁，因此声明为 mutable
    mutable std::mutex mutex_;

    /// 队列出现空位，或 Close/Abort 时唤醒发送线程
    std::condition_variable not_full_cv_;

    /// 队列出现数据，或 Close/Abort 时唤醒接收线程
    std::condition_variable not_empty_cv_;

    /// 实际缓存容器，所有访问都必须持有 mutex_
    std::queue<T> queue_;

    /// 构造后不再变化的容量上限
    const std::size_t capacity_;

    /// 正常关闭标志为 true 时拒绝 Push，但允许 Pop 排空缓存；队列排空后下一次 Push 会自动复位为 false
    bool closed_{false};

    /// 立即中止标志；为 true 时 Push 和 Pop 都立即失败，且不会被自动复位
    bool aborted_{false};
};
}  // namespace mediacore

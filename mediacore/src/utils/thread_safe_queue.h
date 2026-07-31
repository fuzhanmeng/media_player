/**
 * @file thread_safe_queue.h
 * @brief 泛型的有界阻塞队列 背压防 OOM - DAG 节点间数据通道的底层
 * @details 生产者-消费者模型 多线程安全的 定容队列，满时阻塞生产者，空时阻塞消费者
 *          停止语义分为两级
            1. Close：禁止再Push，但是已经入队的数据仍然可以被 Pop，用于 EOF/正常结束
            2. Abort：强制关闭，立即唤醒所有等待者，Push/Pop 立即返回失败，用于 Seek / 关闭播放器等操作
 */
#pragma once

#include <condition_variable>
#include <cstddef> // std::size_t
#include <mutex>
#include <optional>
#include <queue>
#include <utility> // std::move / std::forward

namespace mediacore {

/**
 * @class ThreadSafeQueue
 * @brief 定容、阻塞、可中止的线程安全队列
 * @tparam T 元素类型， DAG 中通常都是 std::shared_ptr<Packet> / std::shared_ptr<VideoFrame>
           等轻量句柄，移动开销极低，本类内部一律 用 std::move 转移，避免深拷贝
 * 队列中有 2 种等待者，等待条件完全相反
   - 生产者等 "不满" NotFull， 只有队列不满时，生产者继续生产
   - 消费者等 "不空" NotEmpty，只有队列不空时，消费者继续消费
 */
template <typename T>
class ThreadSafeQueue {
public:
    /**
     * @brief 构造一个容量上限为 capacity 的有界队列
     */
    explicit ThreadSafeQueue(std::size_t capacity) : kCapacity(capacity) {}

    /**
     * @brief 析构函数
     */
    ~ThreadSafeQueue() = default;

    // 队列持有 mutex / condition_variable，不可拷贝、不可移动（这些同步原语本身不可拷贝/移动）。
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&& other) = delete;

    /**
     * @brief 阻塞式入队，队列满时，挂起当前线程，直到有空位或者被 Abort/Close
     * @param value 待元素入队，按值接收后 std::move 进队列
     * @return true 成功入队， false 入队失败
     */
    bool Push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);

        // wait(lock, pred) ： pred 为假时释放锁并挂起，被notify 唤醒后重新持锁并复查 pred
        // 被唤醒的条件，有空位/Abort/Close
        notFull_.wait(lock, [this]() { return queue_.size() < kCapacity || aborted_ || closed_; });

        // 醒来后再次检,已中止或已关闭：拒绝入队
        if (aborted_ || closed_) {
            return false;
        }

        queue_.push(std::move(value));

        // 定向唤醒一个消费者：队列此刻至少有一个元素可取。
        notEmpty_.notify_one();

        return true;
    }

    /**
     * @brief 阻塞式出队：队列空时挂起当前线程，直到有数据或者被 Abort/Close
     * @return 取到元素则返回 std::optional<T>，返回 std::nullopt 表示 "取不到"
               - 被 Abort：立即返回 nullopt
               - 被 Close 且队列已空：返回 nullopt
    * @note 典型的消费循环： while(auto item = queue.pop()) { process(std::move(*item)); }
     */
    std::optional<T> Pop() {
        std::unique_lock<std::mutex> lock(mutex_);

        // 醒来条件：有数据可取 / Abort / Close
        notEmpty_.wait(lock, [this]() { return !queue_.empty() || aborted_ || closed_; });

        // 醒来后检查是否强制终止
        if (aborted_) {
            return std::nullopt;
        }

        // 如果是 Close，
        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop();

        // 唤醒一个生产者
        notFull_.notify_one();
        return value;
    }

    /**
     * @brief 正常关闭：禁止再 Push，但保留存量供消费者排空，排空后 Pop 返回 std::nullopt
     * @note 幂等，可重复调用，用于 EOF / 正常收尾
     */
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }

        // 唤醒所有因阻塞等待的线程
        notFull_.notify_all();
        notEmpty_.notify_all();
    }

    /**
     * @brief 强制终止：立即唤醒所有等待者， Push/Pop 一律返回失败，未处理数据按 "丢弃" 语义处理
     * @note 幂等，用于 Seek / 窗口关闭等需要 立即停止的 情况
     */
    void Abort() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            aborted_ = true;
        }

        notFull_.notify_all();
        notEmpty_.notify_all();
    }

    /**
     * @brief 查询当前队列元素个数
     */
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief 是否已被 Abort
     */
    bool IsAborted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return aborted_;
    }

    /**
     * @brief 是否已被 Close
     */
    bool IsClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    /// 保护以下所有共享状态的互斥锁，mutable->  const 查询方法内也需要加锁
    mutable std::mutex mutex_;

    /// 生产者等待 "不满" 的变量条件：队列出现空位置时被唤醒
    std::condition_variable notFull_;

    /// 消费者等待 "不空" 的条件变量：队列中有新元素入队时被唤醒
    std::condition_variable notEmpty_;

    /// 底层容器： std::queue
    std::queue<T> queue_;

    /// 容量上限
    const std::size_t kCapacity;

    /// 强制终止的标志
    bool aborted_{false};

    /// 正常关闭的标志
    bool closed_{false};
};
} // namespace mediacore
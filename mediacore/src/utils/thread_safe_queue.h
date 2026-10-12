/**
 * @file thread_safe_queue.h
 * @brief C++17 有界阻塞队列实现（支持反向背压、锁外清空与协作式终止）
 * @details
 * 作为 Pipeline 各工作节点间 Link 的底层存储与同步中枢：
 * 1. 语义完全解耦：纯数据容器，内部不包含任何音视频媒体语义（无 PTS、无流标记）；
 * 2. 背压机制（Backpressure）：
 *    - Push()：队列满时阻塞等待，仅在队列被 Abort 后返回 false；
 *    - Pop()：队列空时阻塞等待，仅在队列被 Abort 后返回 std::nullopt；
 * 3. 锁外析构清空（Out-of-lock Destruction）：
 *    - Flush()：通过 std::queue::swap 快速置换，在互斥锁外析构被抛弃的元素，用于 Seek 快速丢弃历史数据；
 *    - Abort()：永久终止队列，并同时唤醒所有阻塞在生产与消费端条件变量上的线程；
 * 4. 优化通知时序：在释放互斥锁后调用 condition_variable::notify，避免唤醒线程立即陷入对当前锁的二次竞争（Hurrying to
 * Wait）。
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>

#include "mediacore/logger.h"

namespace mediacore {

/**
 * @class ThreadSafeQueue
 * @brief 支持多生产者与多消费者的有界线程安全阻塞队列
 * @tparam T 存储的元素类型（建议具备高效的移动构造与移动赋值语义）
 * @note 本类所有对外公共接口均受内部互斥锁保护；但瞬时状态查询（如 Size()）返回后即可能改变，不应作为外部强同步依据。
 */
template <typename T>
class ThreadSafeQueue {
public:
    /**
     * @brief 构造指定容量上限的有界队列
     * @param capacity 队列允许缓存的最大元素数量，必须大于 0
     * @throws std::invalid_argument 当 capacity == 0 时抛出异常，防止生产者永久阻塞
     */
    explicit ThreadSafeQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            LOG_ERROR("ThreadSafeQueue construction failed: capacity must be greater than zero");
            throw std::invalid_argument("ThreadSafeQueue capacity must be greater than zero");
        }
        LOG_DEBUG("ThreadSafeQueue constructed, capacity {}", capacity_);
    }

    /**
     * @brief 析构函数：释放队列资源
     * @note 调用方应确保此前已调用 Abort() 或各工作节点线程已安全退出，严禁在有线程阻塞于 Push/Pop 时析构
     */
    ~ThreadSafeQueue() = default;

    // 严禁拷贝与移动（包含 std::mutex 与 std::condition_variable 不可复制/移动的原语）
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

    /**
     * @brief 向队尾压入一个元素；队列达到容量上限时阻塞等待
     * @param value 待入队的元素（右值或具名对象，内部通过 std::move 转移）
     * @return true 表示成功入队；false 表示队列已被 Abort 终止
     */
    bool Push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 阻塞等待条件：队列尚未达上限，或者队列已被强制终止
        not_full_cv_.wait(lock, [this]() { return aborted_ || queue_.size() < capacity_; });

        if (aborted_) {
            LOG_DEBUG("ThreadSafeQueue push aborted");
            return false;
        }

        queue_.push(std::move(value));

        // 关键性能优化：先手动解锁，再唤醒单条等待的消费者线程，避免其刚被唤醒就立刻卡在 mutex_ 争抢上
        lock.unlock();
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief 从队首弹出一个元素；队列为空时阻塞等待
     * @return 成功取出时返回包含元素的 std::optional<T>；队列已被 Abort 终止时返回 std::nullopt
     */
    std::optional<T> Pop() {
        std::unique_lock<std::mutex> lock(mutex_);

        // 阻塞等待条件：队列非空，或者队列已被强制终止
        not_empty_cv_.wait(lock, [this]() { return !queue_.empty() || aborted_; });

        if (aborted_) {
            LOG_DEBUG("ThreadSafeQueue pop aborted");
            return std::nullopt;
        }

        std::optional<T> value(std::in_place, std::move(queue_.front()));
        queue_.pop();
        const std::size_t size_after_pop = queue_.size();

        // 先释放锁，再唤醒可能被背压阻塞的生产者
        lock.unlock();
        not_full_cv_.notify_one();

        LOG_TRACE("ThreadSafeQueue pop completed, remaining size {}", size_after_pop);
        return value;
    }

    /**
     * @brief 清空当前队列中的所有已存元素，队列保持可用状态
     * @details
     * 1. 采用 swap  idiom：在锁内极短时间内置换出数据容器，在互斥锁外执行元素的深层析构，
     *    避免复杂析构逻辑长久占用队列互斥锁引发死锁或吞吐暴跌；
     * 2. 仅唤醒因满队列而阻塞的 Push 生产者，空队列等待数据的 Pop 消费者不受打扰，继续挂起等待新数据。
     */
    void Flush() {
        std::size_t discarded_count = 0;
        std::queue<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            discarded_count = queue_.size();
            queue_.swap(discarded);
        }

        // 唤醒所有因达到 capacity 限制而挂起的生产者
        not_full_cv_.notify_all();
        LOG_DEBUG("ThreadSafeQueue flushed, discarded {} items", discarded_count);
    }

    /**
     * @brief 永久终止队列并强行打断所有处于等待中的阻塞线程
     * @details
     * 1. 幂等设计：多次调用是安全的；
     * 2. 置位 aborted_ 为 true，将现有残留数据在锁外释放，并广播唤醒所有 Push 和 Pop 线程；
     * 3. 队列终止后不可复原，后续所有 Push 均返回 false，Pop 均返回 std::nullopt。
     */
    void Abort() {
        std::size_t discarded_count = 0;
        std::queue<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
            discarded_count = queue_.size();
            queue_.swap(discarded);
        }

        // 同时广播唤醒两端的阻塞线程
        not_full_cv_.notify_all();
        not_empty_cv_.notify_all();
        LOG_INFO("ThreadSafeQueue aborted, discarded {} items", discarded_count);
    }

    /**
     * @brief 获取当前队列中的元素瞬时快照数量
     */
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief 查询当前队列是否为空的瞬时状态
     */
    bool Empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * @brief 查询队列是否已被 Abort 终止
     */
    bool IsAborted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return aborted_;
    }

    /**
     * @brief 获取队列设定的最大容量上限
     */
    std::size_t Capacity() const noexcept { return capacity_; }

private:
    mutable std::mutex mutex_;  ///< 保护 queue_ 与 aborted_ 的互斥锁
    std::condition_variable not_full_cv_;  ///< 队列腾出可用空间（或 Abort）时用于唤醒 Push
    std::condition_variable not_empty_cv_;  ///< 队列到达新数据（或 Abort）时用于唤醒 Pop
    std::queue<T> queue_;  ///< 内部实际存储元素数据的 FIFO 标准队列
    const std::size_t capacity_;  ///< 队列容量上限阈值（构造初始化后不可变）
    bool aborted_{false};  ///< 是否已被永久终止与打断的标志位
};

}  // namespace mediacore
/**
 * @file thread_safe_queue.h
 * @brief C++17 有界线程安全队列，支持阻塞/限时入队出队、清空和终止。
 *
 * @details
 * 本类作为 Pipeline 各节点之间 Link 的底层数据容器，只负责元素存储和线程同步，
 * 不包含音视频流、PTS、Packet 等媒体业务逻辑。
 *
 * 主要功能：
 * 1. 有界背压：队列满时生产者可以等待；队列空时消费者可以等待。
 * 2. 限时操作：PushFor() 和 PopFor() 最多等待指定时间，便于工作线程定期检查控制状态。
 * 3. 锁外释放元素：Flush() 和 Abort() 先在锁内交换队列，再在锁外析构被丢弃的元素，
 *    减少元素析构过程占用队列互斥锁的时间。
 * 4. 协作式终止：Abort() 永久终止队列，并唤醒等待入队和等待出队的线程。
 *
 * 注意：Abort() 是终止操作，不是清空后继续使用；Flush() 只清空现有元素，队列仍可使用。
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <utility>

#include "mediacore/logger.h"

namespace mediacore {

/**
 * @brief 有界队列限时入队的结果。
 *
 * @details
 * 用于区分入队成功、等待空位超时和队列已被终止三种情况。
 */
enum class QueuePushResult : std::uint8_t {
    kPushed,  ///< 入队成功，传入的数据已移动到队列中
    kTimedOut,  ///< 等待空位超时，未入队，传入的数据仍由调用方持有
    kAborted  ///< 队列已被 Abort() 终止，未执行入队
};

/**
 * @class ThreadSafeQueue
 * @brief 支持多生产者、多消费者的有界线程安全 FIFO 队列
 * @tparam T 队列元素类型，建议具有高效且最好不抛异常的移动构造/移动赋值操作
 *
 * @details
 * 队列容量由构造函数指定，创建后保持不变
 * 对 queue_ 和 aborted_ 的访问均由 mutex_ 保护；Capacity() 只读取构造后不变的容量值，因此不需要加锁
 *
 * Size()、Empty() 等查询结果只是调用时刻的状态快照，函数返回后队列仍可能被其他线程修改，
 * 不应将这些结果作为跨线程同步或资源预留依据
 */
template <typename T>
class ThreadSafeQueue {
public:
    /**
     * @brief 构造指定容量的有界队列
     * @param capacity 队列最多允许存放的元素数量，必须大于 0
     * @throws std::invalid_argument 当 capacity 为 0 时抛出异常
     */
    explicit ThreadSafeQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            LOG_ERROR("ThreadSafeQueue construction failed: capacity must be greater than zero");
            throw std::invalid_argument("ThreadSafeQueue capacity must be greater than zero");
        }
        LOG_DEBUG("ThreadSafeQueue constructed, capacity {}", capacity_);
    }

    /**
     * @brief 析构队列并释放剩余元素
     * @note 析构前必须确保所有访问该队列的工作线程都已退出或不再访问队列
     *       通常应先由外部调用 Abort() 唤醒阻塞线程，再等待相关线程退出，最后销毁队列
     *       不能在仍有线程执行 Push()/Pop()/PushFor()/PopFor() 时直接析构
     */
    ~ThreadSafeQueue() = default;

    // 队列包含 mutex 和 condition_variable 等同步原语，不允许复制或移动，保证同步对象地址稳定。
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

    /**
     * @brief 向队尾放入一个元素；队列已满时持续等待，直到出现空位或队列被终止
     * @param value 待入队元素，按值接收；传入左值时会先复制到参数，传入右值时可移动到参数
     * @return true 表示成功入队；false 表示等待期间队列被 Abort() 终止
     * @note 入队时会将参数 value 移动到队列中。队列已终止时返回 false；元素复制、移动或内存分配
     * 过程中发生的异常仍会向调用方传播
     */
    bool Push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 队列未满时可以入队；队列已满时等待消费者取走元素
        // Abort() 也会唤醒等待者，因此必须把 aborted_ 一并放入等待条件
        not_full_cv_.wait(lock, [this]() { return aborted_ || queue_.size() < capacity_; });

        if (aborted_) {
            LOG_DEBUG("ThreadSafeQueue push aborted");
            return false;
        }

        // 将参数中的元素移动到队尾，避免在队列中额外复制元素
        queue_.push(std::move(value));

        // 先释放队列锁，再通知一个等待数据的消费者，减少被唤醒线程与当前线程争抢互斥锁的机会
        lock.unlock();
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief 尝试将数据放入队列；队列满时最多等待指定时间
     * @param value 待入队的数据，仅成功入队时才会被移动
     * @param timeout 队列满时的最长等待时间
     * @return kPushed：入队成功；kTimedOut：等待超时，数据仍由调用方持有；kAborted：队列已终止
     * @note 这是限时阻塞入队，不是立即返回的非阻塞入队。适用于 Seek 预滚等需要定期检查控制状态的场景
     */
    QueuePushResult PushFor(T& value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 队列有空位或已经 Abort() 时结束等待；否则最多等待 timeout
        // wait_for 会在等待期间释放 mutex_，被唤醒后重新加锁并再次检查条件
        if (!not_full_cv_.wait_for(lock, timeout, [this]() { return aborted_ || queue_.size() < capacity_; })) {
            // 超时表示仍未等到空位；此分支没有移动 value，调用方可以保留它并稍后重试
            return QueuePushResult::kTimedOut;
        }

        if (aborted_) {
            return QueuePushResult::kAborted;
        }

        // 只有确认队列未终止且存在空位后，才移动数据并完成入队
        queue_.push(std::move(value));
        lock.unlock();
        not_empty_cv_.notify_one();
        return QueuePushResult::kPushed;
    }

    /**
     * @brief 从队首取出一个元素；队列为空时持续等待，直到有数据或队列被终止
     * @return 成功时返回包含出队元素的 optional；队列被 Abort() 终止时返回 std::nullopt
     */
    std::optional<T> Pop() {
        std::unique_lock<std::mutex> lock(mutex_);

        // 队列非空时可以取数据；Abort() 也会唤醒消费者，因此两种情况都需要结束等待
        not_empty_cv_.wait(lock, [this]() { return !queue_.empty() || aborted_; });

        // Abort() 会清空队列并终止后续操作，因此终止状态下不再返回元素
        if (aborted_) {
            LOG_DEBUG("ThreadSafeQueue pop aborted");
            return std::nullopt;
        }

        // 将队首元素移动到返回值，再从队列中移除该元素
        std::optional<T> value(std::in_place, std::move(queue_.front()));
        queue_.pop();
        const std::size_t size_after_pop = queue_.size();

        // 出队后队列多出一个空位；释放锁后通知一个等待入队的生产者
        lock.unlock();
        not_full_cv_.notify_one();

        LOG_TRACE("ThreadSafeQueue pop completed, remaining size {}", size_after_pop);
        return value;
    }

    /**
     * @brief 尝试从队列取出一个元素；队列为空时最多等待指定时间
     * @param timeout 队列为空时的最长等待时间
     * @return 成功时返回取出的元素；超时或队列已终止时返回 std::nullopt
     * @note 这是限时阻塞出队。返回 std::nullopt 时无法区分超时和 Abort()，如需区分应扩展返回状态类型
     */
    std::optional<T> PopFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 队列出现数据或被 Abort() 时结束等待；否则最多等待 timeout
        if (!not_empty_cv_.wait_for(lock, timeout, [this]() { return aborted_ || !queue_.empty(); }) || aborted_) {
            return std::nullopt;
        }

        // 成功取得队首元素，并从队列中删除
        std::optional<T> result(std::in_place, std::move(queue_.front()));
        queue_.pop();

        // 出队后产生空位，通知一个可能正在等待 Push/PushFor 的生产者
        lock.unlock();
        not_full_cv_.notify_one();
        return result;
    }

    /**
     * @brief 清空队列中当前保存的所有元素，但不终止队列，之后仍可继续 Push/Pop
     * @details 先在锁内将原队列与局部空队列交换，再在锁外析构被清除的元素，缩短持锁时间
     *          清空后通知等待队列空位的生产者；不会通知等待新数据的消费者，因为队列仍为空
     */
    void Flush() {
        std::size_t discarded_count = 0;
        std::queue<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            discarded_count = queue_.size();
            // 交换后 queue_ 立即变为空队列；原有元素转移到 discarded，离开作用域时在锁外析构
            queue_.swap(discarded);
        }

        // 队列已清空，所有因队列满而等待的生产者都有机会继续尝试入队
        not_full_cv_.notify_all();
        LOG_DEBUG("ThreadSafeQueue flushed, discarded {} items", discarded_count);
    }

    /**
     * @brief 永久终止队列，丢弃剩余元素并唤醒所有等待中的生产者和消费者
     * @details Abort() 可以重复调用；第一次调用后 aborted_ 保持 true，队列不能恢复使用
     *          原队列中的元素会在互斥锁外析构，避免元素析构过程长时间占用队列锁
     *          终止后 Push()/PushFor() 不再入队，Pop()/PopFor() 不再返回元素
     */
    void Abort() {
        std::size_t discarded_count = 0;
        std::queue<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
            discarded_count = queue_.size();
            // 将待丢弃元素移到局部队列，锁释放后再由 discarded 的析构函数释放
            queue_.swap(discarded);
        }

        // 同时唤醒两端：生产者检查 aborted_ 后退出，消费者检查 aborted_ 后退出
        not_full_cv_.notify_all();
        not_empty_cv_.notify_all();
        LOG_INFO("ThreadSafeQueue aborted, discarded {} items", discarded_count);
    }

    /**
     * @brief 获取当前队列中的元素数量。
     * @return 加锁读取到的瞬时数量；返回后该数值可能立即因其他线程操作而变化
     */
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /**
     * @brief 查询队列在加锁检查时是否为空
     * @return true 表示检查时队列为空；返回后状态仍可能变化
     */
    bool Empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
     * @brief 查询队列是否已经被 Abort() 永久终止
     * @return true 表示队列已终止；false 表示当前尚未终止
     */
    bool IsAborted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return aborted_;
    }

    /**
     * @brief 获取队列的最大容量
     * @return 构造时指定的容量，该值创建后不变，因此读取时不需要加锁
     */
    std::size_t Capacity() const noexcept { return capacity_; }

private:
    mutable std::mutex mutex_;  ///< 保护 queue_ 和 aborted_ 的互斥锁
    std::condition_variable not_full_cv_;  ///< 队列有空位或被 Abort() 时，唤醒等待入队的生产者
    std::condition_variable not_empty_cv_;  ///< 队列有数据或被 Abort() 时，唤醒等待出队的消费者
    std::queue<T> queue_;  ///< 实际存储元素的 FIFO 队列
    const std::size_t capacity_;  ///< 队列容量上限，构造后不可修改
    bool aborted_{false};  ///< 队列是否已永久终止；由 mutex_ 保护
};

}  // namespace mediacore

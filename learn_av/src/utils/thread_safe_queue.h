/**
 * @file thread_safe_queue.h
 * @brief 泛型线程安全有界阻塞队列
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace av {

template <typename T>
class ThreadSafeQueue {
public:
    /**
     * @brief 构造函数
     * @param max_capacity 队列最大容量,达到此容量时，生产者(Push)将被强行阻塞
     */
    explicit ThreadSafeQueue(size_t max_capacity = 100) : max_capacity_(max_capacity), is_abort_(false) {}

    ~ThreadSafeQueue() { Abort(); }

    /**
     * @brief 塞入一个元素 (生产者调用，队列满时会阻塞休眠)
     * @return true 成功塞入; false 塞入被打断(处于 Abort 状态)，拒绝入队
     */
    bool Push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 队列满时生产者阻塞等待，直到消费者取走数据或队列被 Abort
        not_full_cv_.wait(lock, [this]() { return queue_.size() < max_capacity_ || is_abort_; });

        // 被 Abort 唤醒时丢弃数据并返回 false
        if (is_abort_) {
            return false;
        }

        queue_.push(std::move(item));

        // 唤醒一个等待数据的消费者
        not_empty_cv_.notify_one();
        return true;
    }

    /**
     * @brief 塞入一个元素 - 非阻塞
     */
    bool TryPush(T item) {
        // 加锁防竞争
        std::lock_guard<std::mutex> lock(mutex_);

        // 如果队列已经被标记终止，或者满了，立刻放弃入队
        if (is_abort_ || queue_.size() >= max_capacity_) {
            return false;
        }

        // 执行入队操作
        queue_.push(std::move(item));

        // 唤醒消费者
        not_empty_cv_.notify_all();

        return true;

    }

    /**
     * @brief 弹出一个元素 (消费者调用，队列空时会阻塞休眠)
     * @return true 成功弹出; false 弹出被打断且队列已空
     */
    bool Pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 队列空时消费者阻塞等待，直到生产者入队数据或队列被 Abort
        not_empty_cv_.wait(lock, [this]() { return !queue_.empty() || is_abort_; });

        if (is_abort_ && queue_.empty()) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();

        // 取走一个元素后队列有空位，唤醒等待入队的生产者
        not_full_cv_.notify_one();

        return true;
    }

    /**
     * @brief 尝试弹出一个元素 (非阻塞提取)
     * @details 专供高优先级硬件回调线程使用。若队列为空，立刻返回 false，绝不挂起线程。
     */
    bool TryPop(T& item) {
        // 此处仅需普通互斥锁即可，不需要 condition_variable 的 wait
        std::lock_guard<std::mutex> lock(mutex_);

        // 队列为空或已被 Abort 时立即返回，不阻塞
        if (queue_.empty() || is_abort_) {
            return false;
        }

        // 执行正常的元素提取
        item = std::move(queue_.front());
        queue_.pop();

        // 提取完毕，唤醒可能因队列满而挂起的生产者
        not_full_cv_.notify_one();

        return true;
    }

    /**
     * @brief 清空队列
     */
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty_queue;
        std::swap(empty_queue, queue_);
        // 清空后，肯定不满，唤醒所有因为队列满而阻塞的生产者
        not_full_cv_.notify_all();
    }

    /**
     * @brief 强行唤醒所有阻塞的线程并停止工作
     */
    void Abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        is_abort_ = true;
        // 强行唤醒所有阻塞的消费者和生产者，让他们安全退出死循环
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

    /**
     * @brief 打断阻塞并立即清空队列中的残留数据（Seek 专用）
     * @details 原子操作：标记 Abort -> 清空历史数据 -> 唤醒所有阻塞线程
     */
    void AbortAndClear() {
        std::lock_guard<std::mutex> lock(mutex_);
        is_abort_ = true;
        std::queue<T> empty_queue;
        std::swap(empty_queue, queue_); // 清空残留数据
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

    /**
     * @brief 重新激活队列，使其可以再次接收和弹出数据
     * @details 在线程重新 Start 或 Seek 成功准备恢复时调用
     */
    void Start() {
        std::lock_guard<std::mutex> lock(mutex_);
        is_abort_ = false;
    }

    /**
     * @brief 返回队列大小
     */
    int Size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    size_t max_capacity_; // 容量上限
    std::mutex mutex_;
    std::condition_variable not_empty_cv_; // 控制"空则阻塞"
    std::condition_variable not_full_cv_; // 控制"满则阻塞" (背压)
    std::atomic<bool> is_abort_{false};
};

} // namespace av
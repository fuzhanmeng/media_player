/**
 * @file thread_safe_queue.h
 * @brief 泛型线程安全有界阻塞队列
 */
#pragma once

#include "logger.h"

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
     */
    void Push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 如果队列满了，生产者线程在这里死等，直到消费者拿走数据或被 Abort
        not_full_cv_.wait(lock, [this]() { return queue_.size() < max_capacity_ || is_abort_; });

        if (is_abort_) {
            return;
        }

        queue_.push(std::move(item));

        // 唤醒一个正在等待数据的消费者
        not_empty_cv_.notify_one();
    }

    /**
     * @brief 弹出一个元素 (消费者调用，队列空时会阻塞休眠)
     */
    bool Pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        // 如果队列为空，消费者死等
        not_empty_cv_.wait(lock, [this]() { return !queue_.empty() || is_abort_; });

        if (is_abort_ && queue_.empty()) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();

        // 拿走了一个元素，队列有空位了，立刻唤醒正在挂起等待的生产者！
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

        // 如果队列为空，或者已经被标记终止，立刻放弃提取
        if (queue_.empty() || is_abort_) {
            LOG_INFO("the queue is empty");
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
     * @brief 重新激活队列，使其可以再次接收和弹出数据
     * @details 在线程重新 Start 时必须调用此函数解除脑死亡状态
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
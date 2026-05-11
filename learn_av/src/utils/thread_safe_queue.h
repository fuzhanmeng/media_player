/**
 * @file thread_safe_queue.h
 * @brief 泛型线程安全队列（生产者-消费者模型核心容器）
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
    ThreadSafeQueue() : is_abort_(false) {}
    ~ThreadSafeQueue() { Abort(); }

    /**
     * @brief 塞入一个元素 (生产者调用)
     */
    void Push(T item) {
        // 1. 加锁 (lock_guard)
        // 2. 把 item push 进 queue_
        // 3. 用 cv_ 唤醒一个正在等待的消费者 (notify_one)

        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
        cv_.notify_one();
    }

    /**
     * @brief 弹出一个元素 (消费者调用，会阻塞等待)
     * @param item 用于接收弹出的数据引用
     * @return 成功获取返回 true，如果队列被 Abort 终止则返回 false
     */
    bool Pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);

        cv_.wait(lock, [this]() { return !queue_.empty() || is_abort_; });
        if (is_abort_) {
            return false;
        }

        item = std::move(queue_.front());
        queue_.pop();

        return true;
    }

    /**
     * @brief 清空队列
     */
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);

        std::queue<T> empty_queue;
        std::swap(empty_queue, queue_);
    }

    /**
     * @brief 强行唤醒所有阻塞的线程并停止工作
     */
    void Abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        is_abort_ = true;
        cv_.notify_all();
    }

    int Size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> is_abort_{false};
};

} // namespace av
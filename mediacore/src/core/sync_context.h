/**
 * @file sync_context.h
 * @brief 全局控制块 -- SyncContext -- 被 DAG 所有节点共享的一个全局控制状态
 * @details Pipeline 创建一个 std::shared_ptr<SyncContext>，各节点各持有一份

 * @par 为什么核心状态用 atomic 而不是加锁？
 *  state_ / epoch_ 是"一个线程写、N 个线程每轮循环都读"的标志位,若用 mutex，每个节点都要抢锁
 *
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace mediacore {

class Pipeline;

/**
 * @enum PipelineRunState
 * @brief  工作线程的全局机械运行态；停止由每个 Node 的 stop flag 表达，两种状态 RUNNING / WAIT
 */
enum class PipelineRunState {
    kRunning, ///< 正常运行：节点循环持续收→处理→发
    kPaused, ///< 暂停：工作线程睡在 CV 上，不空转，不退出
};

/**
 * @class SyncContext
 * @brief 为全部 Node 广播运行态并提供无丢失唤醒的暂停等待
 */
class SyncContext {
public:
    /**
     * @brief 构造函数，默认实现
     */
    SyncContext() = default;

    /**
     * @brief 析构函数，默认实现
     */
    ~SyncContext() = default;

    // 全局唯一、被所有节点共享，禁止拷贝移动（一律通过 shared_ptr<SyncContext> 共享）
    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;

    SyncContext(SyncContext&&) = delete;
    SyncContext& operator=(SyncContext&&) = delete;

    /**
     * @brief 获取当前全局运行态
     * @return kRunning 或者 kPaused
     */
    PipelineRunState GetState() const { return state_.load(); }

    /**
     * @brief 暂停时等待，运行或停止时立即返回
     * @tparam StopPredicate 可调用对象类型
     * @param should_stop Node 注入的停止谓词
     * @details 使用模板避免 std::function 分配，谓词版本 wait 自动处理虚假唤醒
     */
    template <typename StopPredicate>
    void WaitWhilePaused(StopPredicate should_stop) {
        std::unique_lock<std::mutex> lock(pause_mutex_);

        if (state_.load() != PipelineRunState::kPaused || should_stop()) {
            return;
        }

        paused_waiter_count_.fetch_add(1);

        pause_cv_.wait(lock,
                       [this, &should_stop]() { return state_.load() != PipelineRunState::kPaused || should_stop(); });
        paused_waiter_count_.fetch_sub(1);
    }

    /**
     * @brief 读取当前 世代号，下游节点用它与自己内部的 epoch 比较，以判断是否要丢弃数据
     */
    std::uint64_t GetEpoch() const { return epoch_.load(); }

    /**
     * @brief 返回当前确实登记在 暂停 CV 上的线程数
     * @return 暂停等待数量的线程数量
     */
    std::size_t GetPauseWaiterCount() const { return paused_waiter_count_.load(); }

private:
    friend class Pipeline;

    /**
     * @brief 切换为运行状态并唤醒暂停中的 Node
     */
    void SetRunning() {
        {
            std::lock_guard<std::mutex> lock(pause_mutex_);
            state_.store(PipelineRunState::kRunning);
        }
        pause_cv_.notify_all();
    }

    /**
     * @brief 切换为暂停状态，Node 会在下一次循环开始时等待
     */
    void SetPaused() {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        state_.store(PipelineRunState::kPaused);
    }

    /**
     * @brief 唤醒等待暂停条件变量的 Node，不修改运行状态
     */
    void WakeAllPausedThreads() {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        pause_cv_.notify_all();
    }

    /**
     * @brief Seek 时递增 epoch 并返回新值
     */
    std::uint64_t BumpEpoch() { return epoch_.fetch_add(1) + 1; }

    /// 全局运行/暂停状态，高频读取使用 atomic
    std::atomic<PipelineRunState> state_{PipelineRunState::kRunning};

    /// Seek 世代号： 每 Seek 一次 + 1，下游比对世代号决定要不要丢弃数据
    std::atomic<std::uint64_t> epoch_{0};

    /// 保护暂停谓词检查、状态写入和等待注册
    std::mutex pause_mutex_;

    /// 暂停专用条件变量：Pause 时睡下， Running/Stopped 时 被 notfiy_all 唤醒
    std::condition_variable pause_cv_;

    /// 已登记暂停等待者数量，仅用于诊断和确定性并发测试
    std::atomic<std::size_t> paused_waiter_count_{0};
};

} // namespace mediacore

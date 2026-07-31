/**
 * @file sync_context.h
 * @brief 全局控制块 -- SyncContext -- 被 DAG 所有节点共享的一个全局控制状态
 * @details Pipeline 创建一个 std::shared_ptr<SyncContext>，各节点各持有一份

 * @par 为什么核心状态用 atomic 而不是加锁？
 *  state_ / epoch_ 是"一个线程写、N 个线程每轮循环都读"的标志位。若用 mutex，每个节点都要抢锁
 *
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <functional>

namespace mediacore {

/**
 * @enum PipelineState
 * @brief 驱动工作线程循环的 内部机械态，只有三种状态： RUNNING / WAIT
 */
enum class PipelineState {
    kRunning, ///< 正常运行：节点循环持续收→处理→发
    kPaused, ///< 暂停：工作线程睡在 CV 上，不空转，不退出
};

/**
 * @class SyncContext
 * @brief 全局播放态 + Seek 世代号(epoch) 的共享控制块，被所有节点共同持有
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
     * @brief 读取当前状态
     */
    PipelineState GetState() const { return state_.load(); }

    /**
     * @brief 切换到 Running 状态，并踹醒所有睡在暂停 CV 上的工作线程继续跑
     */
    void SetRunning() {
        state_.store(PipelineState::kRunning);
        pauseCv_.notify_all();
    }

    /**
     * @brief 切换到 Paused 状态，工作线程在下一轮循环顶部检测到后会睡入暂停 CV
     * @note 只置态、不阻塞主线程；真正 "睡下去" 的是各工作线程自己
     */
    void SetPaused() { state_.store(PipelineState::kPaused); }


    /**
     * @file sync_context.h
     * @brief 唤醒所有睡在暂停 CV 上的线程
     */
    void WakeAllPausedThreads() {
        pauseCv_.notify_all();
    }

    /**
     * @brief 暂停阻塞，IF 当前状态为 Paused，则阻塞调用线程，直到被切换为  Running/Stopped 状态
     */
    void WaitWhilePaused(const std::function<bool()>& should_stop) {
        std::unique_lock<std::mutex> lock(pauseMutex_);

        pauseCv_.wait(lock, [this, &should_stop] () {
            return state_.load() == PipelineState::kRunning || should_stop();
        });
    }

    /**
     * @brief 读取当前 世代号，下游节点用它与自己内部的 epoch 比较，以判断是否要丢弃数据
     */
    std::uint64_t GetEpoch() const { return epoch_.load(); }

    /**
     * @brief Seek 时递增世代号并返回新值 无锁原子递增
     * @details 一次原子加法，"作废旧帧"的信号便顺着数据流自行扩散：旧 item.epoch 小于
     *          新全局 epoch，被各节点自然丢弃。无需任何节点间握手
     */
    std::uint64_t BumpEpoch() { return epoch_.fetch_add(1) + 1; }

private:
    /// 运行态，主线程写，N个工作线程读
    std::atomic<PipelineState> state_{PipelineState::kRunning};

    /// Seek 世代号： 每 Seek 一次 + 1，下游比对世代号决定要不要丢弃数据
    std::atomic<std::uint64_t> epoch_{0};

    /// 暂停专用锁：仅服务 WaitWhilePaused 的 CV 等待
    std::mutex pauseMutex_;

    /// 暂停专用条件变量：Pause 时睡下， Running/Stopped 时 被 notfiy_all 唤醒
    std::condition_variable pauseCv_;
};

} // namespace mediacore

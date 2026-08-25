/**
 * @file sync_context.h
 * @brief Pipeline 内全部 Node 共享的运行/暂停协调对象
 * @details
 * 每个 Pipeline 创建一份 SyncContext，通过同一个 shared_ptr 注入该图内的全部 Node
 * 本类只做两件事：提供 Running/Paused 门控，以及保存一个供 Seek 使用的单调递增 epoch
 * 它不管理 Pipeline 生命周期、不保存 Node 的停止标志、不调用 Link::Abort()、不中断外部
 * I/O，也不负责执行 Seek、清空队列或刷新解码器——这些都由 Pipeline 和各个 Node 自己完成
 *
 * Pipeline 是唯一可以写入运行状态和 epoch 的类Node 只能读取状态，并在自己的工作循环
 * 边界调用 WaitWhilePaused()
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "mediacore/logger.h"

namespace mediacore {

class Pipeline;

/**
 * @enum PipelineRunState
 * @brief Pipeline 提供给 Node 的运行门控状态
 * @details
 * 只描述 Node 能否开始下一次 Tick()，不表示 Pipeline 的完整生命周期
 * Pipeline 自身的 Building、Running、Stopping 等状态由 PipelineState 单独管理
 */
enum class PipelineRunState {
    kRunning,  ///< Node 可以继续执行 Tick()
    kPaused,  ///< Node 在当前 Tick() 返回后、下一次 Tick() 前进入等待
};

/**
 * @brief 将运行门控状态转换为日志可读字符串
 */
inline const char* PipelineRunStateToString(PipelineRunState state) {
    switch (state) {
        case PipelineRunState::kRunning:
            return "Running";
        case PipelineRunState::kPaused:
            return "Paused";
    }

    return "Unknown";
}

/**
 * @class SyncContext
 * @brief Pipeline 内共享的线程安全运行状态对象
 * @details
 * Node 通过公有只读接口读取 Running/Paused 状态和当前 epoch，并可以在此等待暂停结束；
 * SetPaused()、SetRunning()、BumpEpoch() 均为 private，只允许 Pipeline 调用
 * Node 自身的停止请求不保存在本类中，而是由每个 INodeBase 各自维护
 */
class SyncContext final {
public:
    /**
     * @brief 构造函数，默认实现
     */
    SyncContext() = default;

    /**
     * @brief 析构函数，默认实现
     */
    ~SyncContext() = default;

    // 每个 Pipeline 只创建一份，通过 shared_ptr 在该 Pipeline 的全部 Node 间共享
    // SyncContext 内含 mutex 和 condition_variable，因此禁止复制和移动
    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;

    SyncContext(SyncContext&&) = delete;
    SyncContext& operator=(SyncContext&&) = delete;

    /**
     * @brief 读取当前运行门控状态
     * @return kRunning 或 kPaused
     */
    PipelineRunState GetState() const { return state_.load(); }

    /**
     * @brief 在 Paused 状态下阻塞等待，直到恢复运行或 Node 自身被要求停止
     * @tparam StopPredicate 停止判断谓词的可调用类型
     * @param should_stop 返回当前 Node 是否已收到停止请求，必须是快速且线程安全的读取操作
     * @details
     * 调用时先在锁外做一次快速判断：若状态已不是 Paused，或 should_stop() 已经为真，
     * 直接返回，不进入等待真正等待时，检查条件与登记等待放在同一把锁内完成，避免
     * SetRunning()/WakeAllPausedThreads() 的通知发生在检查之后、wait 注册之前而被错过
     * wait 的谓词在每次被唤醒时都会重新判断同一条件，因此也覆盖虚假唤醒
     * 用模板参数直接接收谓词而不是 std::function，避免每次调用产生额外的堆分配
     */
    template <typename StopPredicate>
    void WaitWhilePaused(StopPredicate should_stop) {
        // 已经恢复运行，或 Node 已收到停止请求，不需要进入等待
        if (state_.load() != PipelineRunState::kPaused || should_stop()) {
            return;
        }

        // 锁与状态写入共用同一套同步协议，避免检查和登记等待之间出现空档
        std::unique_lock<std::mutex> lock(pause_mutex_);

        // wait 会在持锁状态下反复检查谓词，自动处理虚假唤醒
        pause_cv_.wait(lock,
                       [this, &should_stop]() { return state_.load() != PipelineRunState::kPaused || should_stop(); });
    }

    /**
     * @brief 读取当前数据世代号
     * @return 当前 epoch，初始值为 0，Pipeline 每次开始新的 Seek 世代时递增
     * @note 读取 epoch 本身不会丢弃旧数据，也不会触发队列或解码器刷新，仅供 Node 判断数据是否过期
     */
    std::uint64_t GetEpoch() const noexcept { return epoch_.load(); }

private:
    friend class Pipeline;

    /**
     * @brief 切换为运行状态并唤醒所有阻塞在 WaitWhilePaused() 上的 Node
     * @details 只允许 Pipeline::Resume() 调用；不修改 epoch
     */
    void SetRunning() {
        {
            std::lock_guard<std::mutex> lock(pause_mutex_);
            state_.store(PipelineRunState::kRunning);
        }
        pause_cv_.notify_all();
        LOG_INFO("SyncContext state set to {}", PipelineRunStateToString(PipelineRunState::kRunning));
    }

    /**
     * @brief 切换为暂停状态
     * @details
     * 只允许 Pipeline::Pause() 调用正在执行 Tick() 或阻塞在 Tick() 内部的 Node 不会被
     * 强行打断，它会在下一次工作循环进入 WaitWhilePaused() 时才真正暂停
     */
    void SetPaused() {
        {
            std::lock_guard<std::mutex> lock(pause_mutex_);
            state_.store(PipelineRunState::kPaused);
        }
        LOG_INFO("SyncContext state set to {}", PipelineRunStateToString(PipelineRunState::kPaused));
    }

    /**
     * @brief 唤醒所有等待暂停结束的 Node，但不修改运行状态
     * @details
     * Pipeline::Stop() 会先给每个 Node 设置自己的停止标志，再调用本函数，使阻塞在
     * WaitWhilePaused() 上的 Node 能够重新检查 should_stop() 并退出；本函数本身不产生
     * 停止语义，也无法唤醒阻塞在 Link 收发或外部 I/O 中的线程，那部分由 Link::Abort()
     * 和 Node::InterruptBlockingIo() 负责
     */
    void WakeAllPausedThreads() {
        // 与 WaitWhilePaused() 使用同一把锁，保证通知和等待注册的顺序清晰
        std::lock_guard<std::mutex> lock(pause_mutex_);
        pause_cv_.notify_all();
        LOG_DEBUG("SyncContext woke all paused threads");
    }

    /**
     * @brief 递增数据世代号并返回新值
     * @return 递增后的 epoch
     * @details
     * 这是 Seek 流程使用的计数操作，本身不定位媒体、不清空 Link、不丢弃队列数据、
     * 不刷新设备，Node 需要各自根据新 epoch 判断并丢弃旧世代的数据
     */
    std::uint64_t BumpEpoch() noexcept {
        // fetch_add 返回递增前的值，因此加 1 后才是本次 Seek 对应的新 epoch
        const std::uint64_t next_epoch = epoch_.fetch_add(1) + 1;
        LOG_INFO("SyncContext epoch bumped to {}", next_epoch);
        return next_epoch;
    }

    /// 运行/暂停快照；写入时持有 pause_mutex_，读取可由 Node 并发执行
    std::atomic<PipelineRunState> state_{PipelineRunState::kRunning};

    /// 数据世代号；初始值 0，每次 Seek 递增一次，仅作计数，不主动触发任何 Seek 动作
    std::atomic<std::uint64_t> epoch_{0};

    /// 协调 Paused 状态写入、等待谓词检查和 condition_variable 的等待注册
    std::mutex pause_mutex_;

    /// Resume 或 Stop 时用于唤醒所有暂停中的 Node；必须始终配合谓词等待使用
    std::condition_variable pause_cv_;
};

}  // namespace mediacore

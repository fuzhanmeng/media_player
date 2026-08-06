/**
 * @file sync_context.h
 * @brief 一个 Pipeline 内全部 Node 共享的运行/暂停协调对象
 * @details
 * 每个 Pipeline 创建一份 SyncContext，并把同一个 shared_ptr 注入该图的全部 Node
 * 本类只负责两件事：提供 Running/Paused 门控，以及保存一个可供后续 Seek 使用的
 * 单调递增 epoch。它不负责 Pipeline 生命周期、Node 的停止标志、Link::Abort()、
 * 外部 I/O 中断，也不会自行完成 Seek、清空队列或刷新解码器
 *
 * Pipeline 是唯一可以写入运行状态和 epoch 的类。Node 只能读取状态，并在自己的
 * 工作循环边界调用 WaitWhilePaused()
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace mediacore {

class Pipeline;

/**
 * @enum PipelineRunState
 * @brief Pipeline 给 Node 使用的运行门控状态
 * @details
 * 这个枚举只描述 Node 是否可以开始下一次 Tick()，不表示 Pipeline 的完整生命周期
 * Pipeline 的 Building、Running、Stopping 等状态由 PipelineState 单独管理
 */
enum class PipelineRunState {
    kRunning, ///< Node 可以继续执行 Tick()
    kPaused, ///< Node 在当前 Tick() 返回后，于下一次 Tick() 前等待
};

/**
 * @class SyncContext
 * @brief 一个 Pipeline 内共享的线程安全运行状态对象
 * @details
 * Node 可以通过公有只读接口获取 Running/Paused 状态和当前 epoch，也可以等待暂停结束
 * SetPaused()、SetRunning() 和 BumpEpoch() 是 private，只允许 Pipeline 调用
 * Node 的停止请求不存放在本类中，而是由每个 INodeBase 自己保存
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

    // 每个 Pipeline 只创建一份；通过 shared_ptr 在该 Pipeline 的 Node 之间共享
    // SyncContext 含有互斥锁和条件变量，因此禁止复制和移动
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
     * @brief 暂停时等待，恢复运行或 Node 停止时返回
     * @tparam StopPredicate 停止谓词的可调用类型
     * @param should_stop 返回当前 Node 是否已经收到停止请求；必须是快速且线程安全的读取操作
     * @details
     * 如果状态已经是 Running，函数立即返回。Pause() 不会打断正在执行的 Tick()；Node
     * 回到工作循环后才会在这里等待。条件变量谓词会处理虚假唤醒和通知先于等待注册的情况。
     * 使用模板直接接收谓词，避免为每次等待构造 std::function
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
     * @return 当前 epoch；初始值为 0，控制线程每次开始新 Seek 世代时递增
     * @note 读取 epoch 不会丢弃旧数据，也不会触发队列或解码器刷新
     */
    std::uint64_t GetEpoch() const noexcept { return epoch_.load(); }

private:
    friend class Pipeline;

    /**
     * @brief 切换为运行状态并唤醒暂停中的 Node
     * @details 只允许 Pipeline::Resume() 调用；不会改变 epoch
     */
    void SetRunning() {
        {
            std::lock_guard<std::mutex> lock(pause_mutex_);
            state_.store(PipelineRunState::kRunning);
        }
        pause_cv_.notify_all();
    }

    /**
     * @brief 切换为暂停状态
     * @details 只允许 Pipeline::Pause() 调用。正在执行或阻塞在 Tick() 中的 Node 不会被强行打断，
     *          它在下一次工作循环进入 WaitWhilePaused() 时等待。
     */
    void SetPaused() {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        state_.store(PipelineRunState::kPaused);
    }

    /**
     * @brief 唤醒等待暂停的全部 Node，但不修改运行状态
     * @details Stop() 先给每个 Node 设置自己的停止标志，再调用本函数；本函数本身不产生停止语义，
     *          也不能唤醒阻塞在 Link 或外部 I/O 中的线程
     */
    void WakeAllPausedThreads() {
        // 与 WaitWhilePaused() 使用同一把锁，保证通知和等待注册的顺序清晰
        std::lock_guard<std::mutex> lock(pause_mutex_);
        pause_cv_.notify_all();
    }

    /**
     * @brief 递增数据世代号并返回新值
     * @return 递增后的 epoch
     * @details 这是后续 Seek 流程使用的计数操作，不负责定位媒体、清空 Link、丢弃队列数据或刷新设备
     */
    std::uint64_t BumpEpoch() noexcept {
        // fetch_add 返回递增前的值，因此加 1 后才是本次 Seek 对应的新 epoch
        return epoch_.fetch_add(1) + 1;
    }

    /// 运行/暂停快照；写入时持有 pause_mutex_，读取可由 Node 并发执行
    std::atomic<PipelineRunState> state_{PipelineRunState::kRunning};

    /// 数据世代号；0 表示尚未开始新的 Seek 世代，当前只保存计数，不执行 Seek
    std::atomic<std::uint64_t> epoch_{0};

    /// 协调 Paused 状态写入、等待谓词检查和 condition_variable 等待注册
    std::mutex pause_mutex_;

    /// Resume 或 Stop 时唤醒所有暂停 Node；必须始终配合谓词等待使用
    std::condition_variable pause_cv_;
};

} // namespace mediacore

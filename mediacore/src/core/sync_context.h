/**
 * @file sync_context.h
 * @brief Pipeline 内全部 Node 共享的运行/暂停状态与跨线程版本同步上下文
 * @details
 * 每个 Pipeline 实例独占一份 SyncContext，通过 std::shared_ptr 注入该拓扑内的全部 Node。
 * 核心承担三大职责：
 * 1. 运行门控协调（Gatekeeper）：维护 Running / Paused 运行状态，各 Node 工作线程在每次 Tick()
 *    前通过 WaitWhilePaused() 挂起等待，实现管线的毫秒级协作式启停；
 * 2. 媒体世代追踪（Media Epoch）：维护单调递增的 epoch_，Seek 触发时递增，各节点据此安全识别并丢弃过期旧数据；
 * 3. 运行世代追踪（Run Generation）：维护单调递增的 run_generation_，每次从 Paused 恢复为 Running 时递增；
 *    终端渲染节点据此识别暂停事件并重置音画同步时钟锚点，杜绝暂停时长被误算为内容播放时长导致跳帧。
 *
 * @note 写入接口全部对内设为 private，仅授权友元 Pipeline 和 INodeBase 操作；外部各 Node 仅具备只读访问权限。
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "mediacore/logger.h"

namespace mediacore {

class Pipeline;
class INodeBase;

/**
 * @enum PipelineRunState
 * @brief 运行门控状态：指示 Node 是否被允许开始下一次 Tick()
 * @details
 * 仅用于控制工作线程是否挂起，不代表 Pipeline 完整的拓扑生命周期
 * （Pipeline 整体的 Building、Running、Stopping、Stopped 状态由 PipelineState 统一管理）。
 */
enum class PipelineRunState : std::uint8_t {
    kRunning,  ///< 节点处于活跃运行态，允许持续驱动 Tick()
    kPaused,  ///< 节点处于暂停门控态，完成当前 Tick() 后在下一次 Tick() 前挂起等待
};

/**
 * @brief 将运行门控状态枚举转换为可读字符串
 * @param state 待转换的运行门控状态
 * @return 对应的字符串常量（"Running"、"Paused" 或 "Unknown"）
 */
inline const char* PipelineRunStateToString(PipelineRunState state) noexcept {
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
 * @brief Pipeline 拓扑内部共享的线程安全同步控制中枢
 */
class SyncContext final {
public:
    /**
     * @brief 默认构造函数：初始状态置为 Paused
     * @details 初始置为 Paused 是为了让 Pipeline::Start() 启动并创建各工作线程后，
     *          所有线程在门控处挂起待命，直到控制层显式调用 Resume() 后才真正启动流式消费。
     */
    SyncContext() = default;

    /**
     * @brief 析构函数
     */
    ~SyncContext() = default;

    // 禁用拷贝与移动（包含不可移动的同步原语与原子状态）
    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;
    SyncContext(SyncContext&&) = delete;
    SyncContext& operator=(SyncContext&&) = delete;

    /**
     * @brief 线程安全地读取当前运行门控状态
     * @return 当前状态（kRunning 或 kPaused）
     */
    PipelineRunState GetState() const noexcept { return state_.load(std::memory_order_acquire); }

    /**
     * @brief 线程安全地读取当前媒体流世代号（Epoch）
     * @return 当前世代号（初始为 0，每次 Seek 递增）
     * @note 仅用于版本校验；数据包所带 Epoch 与此不符时应直接丢弃
     */
    std::uint64_t GetEpoch() const noexcept { return epoch_.load(std::memory_order_acquire); }

    /**
     * @brief 线程安全地读取当前运行世代号（Run Generation）
     * @return 当前运行世代号（初始为 0，每次从 Paused 恢复为 Running 时递增）
     * @note 终端渲染节点在锚定 PTS 与墙钟映射时记录该值；若发现变化，需重新校准时钟基准
     */
    std::uint64_t GetRunGeneration() const noexcept { return run_generation_.load(std::memory_order_acquire); }

    /**
     * @brief 在 Paused 状态下挂起等待，直到管线恢复运行或外部中断条件满足
     * @tparam StopPredicate 判定是否应提前中断等待的可调用对象类型（函数签名：bool()）
     * @param wake_requested 谓词回调，返回 true 时表示收到停机请求或控制中断，提前结束等待
     * @details
     * 1. 快速无锁检查（Fast Path）：若已处于 kRunning 或 wake_requested() 成立，直接返回零开销跳过；
     * 2. 条件变量等待（Slow Path）：加锁注册并利用谓词 wait 挂起，天然免疫虚假唤醒。
     */
    template <typename StopPredicate>
    void WaitWhilePaused(StopPredicate wake_requested) {
        // Fast path：无锁快速检查，正在运行或已请求停止时不抢锁
        if (GetState() != PipelineRunState::kPaused || wake_requested()) {
            return;
        }

        // Slow path：进入加锁等待，消除检查与通知之间的竞态
        std::unique_lock<std::mutex> lock(pause_mutex_);
        pause_cv_.wait(
            lock, [this, &wake_requested]() { return GetState() != PipelineRunState::kPaused || wake_requested(); });
    }

private:
    friend class Pipeline;
    friend class INodeBase;

    /**
     * @brief 从暂停状态恢复为正常运行状态
     * @details
     * 关键时序保证：必须先递增 run_generation_，再将 state_ 写入为 kRunning。
     * 防止终端渲染节点被唤醒后读取到的仍是旧的 run_generation_，从而将暂停期间消耗的真实时间
     * 误算入音画同步时钟漂移中。
     */
    void SetRunning() {
        {
            std::lock_guard<std::mutex> lock(pause_mutex_);
            // 1. 先递增运行世代，发布新的时钟锚点基准
            run_generation_.fetch_add(1, std::memory_order_release);
            // 2. 再切换运行门控状态
            state_.store(PipelineRunState::kRunning, std::memory_order_release);
        }
        pause_cv_.notify_all();
        LOG_INFO("SyncContext state set to {}", PipelineRunStateToString(PipelineRunState::kRunning));
    }

    /**
     * @brief 切换为暂停状态
     * @details 暂停仅修改门控状态，不递增世代号，亦无需唤醒等待者；各工作线程会在完成当前 Tick() 后自然挂起。
     */
    void SetPaused() {
        {
            std::lock_guard<std::mutex> lock(pause_mutex_);
            state_.store(PipelineRunState::kPaused, std::memory_order_release);
        }
        LOG_INFO("SyncContext state set to {}", PipelineRunStateToString(PipelineRunState::kPaused));
    }

    /**
     * @brief 唤醒所有正阻塞在暂停门控中的工作线程，但不改变当前的运行状态
     * @details 用于管线终止（Pipeline::Stop()）时让休眠中的节点线程醒来并感知停机信号。
     */
    void WakeAllPausedThreads() {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        pause_cv_.notify_all();
        LOG_DEBUG("SyncContext woke all paused threads");
    }

    /**
     * @brief 原子递增并返回新的媒体流世代号
     * @return 递增后的全新 Epoch 版本号
     * @details
     * 本函数仅负责全局版本的原子递增，不执行具体的缓冲区清空操作；
     * 由外部 Seek 状态机负责在此前后向各节点分发中断与 StreamReset 信令。
     */
    std::uint64_t BumpEpoch() noexcept {
        const std::uint64_t next_epoch = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
        LOG_INFO("SyncContext epoch bumped to {}", next_epoch);
        return next_epoch;
    }

    /// 运行门控状态；受 pause_mutex_ 保护写入，通过 acquire-release 保证多线程可见性
    std::atomic<PipelineRunState> state_{PipelineRunState::kPaused};

    /// 媒体流数据世代号；每次 Seek 触发递增
    std::atomic<std::uint64_t> epoch_{0};

    /// 运行状态世代号；每次 Pause -> Running 恢复时递增
    std::atomic<std::uint64_t> run_generation_{0};

    /// 保护运行状态写入与挂起注册的互斥锁
    std::mutex pause_mutex_;

    /// 用于在恢复运行或外部中断时唤醒休眠线程的条件变量
    std::condition_variable pause_cv_;
};

}  // namespace mediacore
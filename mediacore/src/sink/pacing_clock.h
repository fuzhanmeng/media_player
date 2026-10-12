/**
 * @file pacing_clock.h
 * @brief 单调时钟与可中断定时等待抽象接口及基础实现
 * @details
 * 提供基于单调时间的休眠与协作式中断原语：
 * 1. 职责边界：纯粹的计时等待工具，不决定媒体播放的主时钟逻辑（如音视频同步主从关系）；
 * 2. 帧率节奏控制（Pacing）：后续渲染调度器可基于此接口休眠至下一帧的目标 Presentation Timestamp（PTS），
 *    并在遇到 Stop、Pause、Flush 或 Seek 时通过 Wake() 立即中断休眠，避免 UI 卡顿。
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "mediacore/logger.h"

namespace mediacore {

/**
 * @class IPacingClock
 * @brief 单调计时与协作中断等待的纯虚接口
 * @details 抽象系统单调时间源与定时等待操作，便于不同平台定制时钟或单元测试注入模拟时钟（Mock Clock）。
 */
class IPacingClock {
public:
    /// @brief 底层单调递增时钟类型（保证时间绝不倒退）
    using Clock = std::chrono::steady_clock;

    /// @brief 绝对时间点类型，用于表示某个具体的目标唤醒瞬间
    using TimePoint = Clock::time_point;

    /// @brief 时间间隔跨度类型（如帧间隔 duration）
    using Duration = Clock::duration;

    /**
     * @brief 虚析构函数，保证多态安全释放
     */
    virtual ~IPacingClock() = default;

    /**
     * @brief 获取当前单调时钟的绝对时间点
     * @return 当前稳态绝对时间点
     */
    virtual TimePoint Now() const = 0;

    /**
     * @brief 阻塞当前线程直到到达目标时刻，或被 Wake() 显式异步中断
     * @param deadline 目标唤醒时间点
     * @return true 表示达到或超过 deadline 正常超时返回；false 表示被外部调用 Wake() 主动中断
     * @note 退出时会自动消费并复位唤醒标志（Auto-reset）
     */
    virtual bool WaitUntil(TimePoint deadline) = 0;

    /**
     * @brief 异步中断正在 WaitUntil() 阻塞的等待线程
     * @details 常用于 Pipeline 发生 Stop、Seek、Flush 或状态迁移时，立即唤醒休眠线程以响应控制流
     */
    virtual void Wake() = 0;

    /**
     * @brief 显式清除当前未被消费的唤醒请求
     * @note 仅复位内部标志位，不会唤醒等待线程，亦不影响时钟进度
     */
    virtual void ResetWake() = 0;
};

/**
 * @class SteadyPacingClock
 * @brief 基于 std::chrono::steady_clock 与条件变量实现的定时等待类
 * @details
 * 内部基于 std::mutex 与 std::condition_variable 运转，通过布尔标志位 woken_ 区分
 * 自然超时与外部中断，并天然免疫条件变量的虚假唤醒（Spurious Wakeup）。
 */
class SteadyPacingClock : public IPacingClock {
public:
    /**
     * @brief 默认构造函数
     */
    SteadyPacingClock() = default;

    /**
     * @brief 虚析构函数
     */
    ~SteadyPacingClock() override = default;

    // 严禁拷贝与移动（包含不可移动的同步原语）
    SteadyPacingClock(const SteadyPacingClock&) = delete;
    SteadyPacingClock& operator=(const SteadyPacingClock&) = delete;
    SteadyPacingClock(SteadyPacingClock&&) = delete;
    SteadyPacingClock& operator=(SteadyPacingClock&&) = delete;

    /**
     * @brief 获取系统当前的 steady_clock 绝对时间点
     */
    TimePoint Now() const override { return Clock::now(); }

    /**
     * @brief 阻塞当前线程直到达到指定时间点或收到 Wake() 信号
     * @param deadline 目标等待截止时刻
     * @return true 表示达到或超过截止时间；false 表示收到中断信号提前返回
     */
    bool WaitUntil(TimePoint deadline) override {
        bool naturally_timed_out = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // 阻塞等待，直到满足 deadline 超时或者被外部 Wake() 中断
            cv_.wait_until(lock, deadline, [this]() { return woken_; });

            // 若 woken_ 为 false 说明达到或超过截止时间；为 true 则说明是收到 Wake() 中断
            naturally_timed_out = !woken_;
            woken_ = false;  // 自动复位并消费本次唤醒信号（Auto-reset 语义）
        }

        return naturally_timed_out;
    }

    /**
     * @brief 置位唤醒标志并触发条件变量广播通知
     */
    void Wake() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            woken_ = true;
        }

        cv_.notify_all();
        LOG_DEBUG("SteadyPacingClock woken");
    }

    /**
     * @brief 线程安全地显式清除未消费的唤醒标志
     */
    void ResetWake() override {
        std::lock_guard<std::mutex> lock(mutex_);
        woken_ = false;
    }

private:
    mutable std::mutex mutex_;  ///< 保护 woken_ 状态并发读写的互斥锁
    std::condition_variable cv_;  ///< 用于定时阻塞与主动中断唤醒的条件变量
    bool woken_{false};  ///< 外部唤醒事件标志，用于过滤虚假唤醒并标识中断事件
};

}  // namespace mediacore
/**
 * @file playback_sync.h
 * @brief 全局播放状态同步上下文定义
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "playback_state.h"

namespace av {

/**
 * @struct SyncContext
 * @brief 播放器全局状态同步容器
 * @details 将同步原语物理实体封装于此,通过 std::shared_ptr 在全局（Player/Demuxer/Decoder）间共享
 */
struct SyncContext {
    /// 全局播放状态原子实体
    std::atomic<PlaybackState> state_{PlaybackState::kIdle};

    /// 状态访问互斥锁实体 (保护状态读取与 wait 过程的原子性)
    std::mutex state_mutex_;

    /// 状态变更广播条件变量实体 (配合 state_mutex_ 使用)
    std::condition_variable state_cv_;

    /// 跳转瞬态标志位实体
    std::atomic<bool> is_seeking_{false};

    /// 跳转精确时间 (单位：微秒) - 变更为 int64_t
    std::atomic<int64_t> target_time_us_{0};

    // true: 跳转前正在播放，完成后需要自动恢复
    // false: 跳转前处于暂停，完成后保持挂起
    std::atomic<bool> resume_play_after_seek_{false};

    /**
     * @brief 构造函数
     */
    SyncContext() = default;

    /**
     * @brief 禁用拷贝构造与赋值操作符 (因为 std::mutex 和 std::condition_variable 不可拷贝)
     */
    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;
};

} // namespace av
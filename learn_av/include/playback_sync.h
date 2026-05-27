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
 * @details 将同步原语物理实体封装于此。通过 std::shared_ptr 在全管线（Player/Demuxer/Decoder）间共享。
 * 依靠智能指针引用计数机制，确保底层线程在访问同步原语时，物理内存始终存活，彻底杜绝野指针崩溃。
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

    /**
     * @brief 构造函数
     */
    SyncContext() = default;

    /**
     * @brief 禁用拷贝构造与赋值操作符 (因为 std::mutex 和 std::condition_variable 不可拷贝)
     */
    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;

    /**
     * @brief 实体化后，只要对象存在，内存即合法，此校验始终为 true
     */
    bool IsValid() const { return true; }
};

} // namespace av
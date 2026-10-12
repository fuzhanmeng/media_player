/**
 * @file playback_state.h
 * @brief 播放器核心对外呈现的高层状态机枚举及工具函数定义
 * @details
 * 负责定义上层应用（如 UI 渲染层、客户端业务逻辑）能够感知的核心播放状态：
 * 1. 状态所有权与流转：状态的校验与流转由 Player
 * 控制线程独占负责，外部调用方仅通过快照（Snapshot）进行只读轮询或响应状态变更通知；
 * 2. 状态正交性：此处的 PlaybackState 面向用户交互语义，与底层管线的 PipelineState（拓扑构建态）以及
 * PipelineRunState（门控运行态）解耦；
 * 3. 边界操作：明确支持在 kEnded 状态下触发 Seek 重新寻帧，或通过 Play() 执行循环重播。
 */

#pragma once

#include <cstdint>

namespace mediacore {

/**
 * @enum PlaybackState
 * @brief Player 对外发布的业务播放状态机枚举
 */
enum class PlaybackState : std::uint8_t {
    kIdle,  ///< 初始空闲状态：尚未打开任何媒体资源
    kOpening,  ///< 打开中状态：正在异步解封装、探测媒体元数据并构建拓扑管线
    kPlaying,  ///< 播放中状态：时钟正常前进，音视频帧按序持续交付渲染
    kPaused,  ///< 暂停状态：时钟挂起，各节点在管线门控处待命
    kSeeking,  ///< 跳转定位中：正在寻找目标时间戳帧，画面暂未稳定定格
    kEnded,  ///< 播放完毕状态：媒体流已完全播放至末尾（支持直接发起 Seek 或 Play 重新起播）
    kStopped,  ///< 已停止状态：管线已被显式关闭，相关资源已释放
    kError,  ///< 错误异常状态：底层遭遇不可恢复的致命异常，管线已终止
};

/**
 * @brief 将播放状态枚举转换为日志及调试友好的字符串常量
 * @param state 待转换的播放状态枚举
 * @return 对应的常量字符串指针（生命周期贯穿程序全程，无需调用方释放）
 */
inline const char* PlaybackStateToString(PlaybackState state) noexcept {
    switch (state) {
        case PlaybackState::kIdle:
            return "Idle";
        case PlaybackState::kOpening:
            return "Opening";
        case PlaybackState::kPlaying:
            return "Playing";
        case PlaybackState::kPaused:
            return "Paused";
        case PlaybackState::kSeeking:
            return "Seeking";
        case PlaybackState::kEnded:
            return "Ended";
        case PlaybackState::kStopped:
            return "Stopped";
        case PlaybackState::kError:
            return "Error";
    }
    return "Unknown";
}

}  // namespace mediacore
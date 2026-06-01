/**
 * @file playback_state.h
 * @brief 全局播放状态机枚举定义
 */
#pragma once

namespace av {

/**
 * @brief 播放器全局有限状态机枚举
 */
enum class PlaybackState {
    kIdle, ///< 初始空闲状态
    kOpened, ///< 媒体文件探测成功，等待播放
    kPlaying, ///< 时钟同步引擎运转中，正在渲染
    kPaused, ///< 全链路挂起，时钟冻结
    kStopped ///< 全链路终止，资源释放
};

} // namespace av
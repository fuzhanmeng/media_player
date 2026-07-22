/**
 * @file playback_state.h
 * @brief 播放器生命周期的合法状态枚举定义
 */
#pragma once

namespace mediacore {

/**
 * @brief 播放器生命周期状态
 */
enum class PlaybackState {
    kIdle, ///<  初始状态：尚未加载任何资源
    kOpening, ///< 正在打开媒体资源 -- 探测流信息、构建 Pipeline
    kPlaying, ///< 播放中：DAG 节点线程运行，音视频正常推进
    kPaused, ///< 已暂停：节点线程挂起，可恢复到 kPlaying
    kStopeed, ///< 已停止：Pipeline 已拆除， 需重新 kOpening 才能播放
    kError, ///< 错误态：发生不可恢复的错误，需用户显式重置
};
} // namespace mediacore
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
    kIdle,  ///< 初始状态：尚未加载任何资源
    kOpening,  ///< 正在打开媒体资源：探测流信息、构建 Pipeline
    kPlaying,  ///< 播放中：DAG 节点线程运行，音视频正常推进
    kPaused,  ///< 已暂停：节点线程挂起，可恢复到 kPlaying
    kSeeking,  ///< 正在执行 Seek：等待 Flush 包到达，完成后恢复到 kPlaying 或 kPaused
    kEnded,  ///< 播放结束：已到达 EOF，可通过 Seek(0) 加 Play 重新播放
    kBuffering,  ///< 缓冲中：网络流数据不足，等待数据恢复；当前仅预留状态位，尚无触发该状态的探测逻辑
    kStopped,  ///< 已停止：Pipeline 已拆除，需重新进入 kOpening 才能播放
    kError,  ///< 错误态：发生不可恢复的错误，需用户显式重置
};
}  // namespace mediacore
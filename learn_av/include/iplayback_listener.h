/**
 * @file iplayback_listener.h
 * @brief 播放器状态回调监听接口
 */
#pragma once

namespace av {

class IPlaybackListener {
public:
    virtual ~IPlaybackListener() = default;

    // 播放开始
    virtual void NotifyPlaybackStarted() = 0;

    // 播放进度更新
    // current_pos: 当前时间，duration: 总时长
    virtual void NotifyPlaybackTimeChanged(float current_pos, float duration) = 0;

    // 播放暂停
    virtual void NotifyPlaybackPaused() = 0;

    // 播放结束 (End Of File)
    virtual void NotifyPlaybackEOF() = 0;
};

} // namespace av
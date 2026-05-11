/**
 * @file iplayback_listener.h
 * @brief 播放器状态回调监听接口
 */
#pragma once

namespace av {

class IPlaybackListener {
public:
    virtual ~IPlaybackListener() = default;

    // Start Play
    virtual void NotifyPlaybackStarted() = 0;

    // Playback progress update
    // current_pos: 当前时间，duration: 总时长
    virtual void NotifyPlaybackTimeChanged(float current_pos, float duration) = 0;

    // Pause
    virtual void NotifyPlaybackPaused() = 0;

    // End
    virtual void NotifyPlaybackEOF() = 0;
};

} // namespace av
/**
 * @file iplayback_listener.h
 * @brief 播放事件监听器接口，面向UI层
 */
#pragma once

#include <string>

namespace av {

/**
 * @brief 播放事件监听器接口
 * @details 所有需要接收播放器状态变化的UI类都需要实现这个接口
 *          所有回调都保证在主线程中调用
 */
class IPlaybackListener {
public:
    virtual ~IPlaybackListener() = default;

    /**
     * @brief 播放开始
     */
    virtual void OnPlaybackStarted() = 0;

    /**
     * @brief 播放暂停
     */
    virtual void OnPlaybackPaused() = 0;

    /**
     * @brief 播放恢复
     */
    virtual void OnPlaybackResumed() = 0;

    /**
     * @brief 播放完成
     * @details 所有音视频帧都已经渲染完成时调用
     */
    virtual void OnPlaybackCompleted() = 0;

    /**
     * @brief 播放错误
     * @param error_code 错误码
     * @param error_msg 错误信息
     */
    virtual void OnPlaybackError(int error_code, const std::string& error_msg) = 0;

    /**
     * @brief 播放进度更新
     * @param progress 当前进度[0.0, 1.0]
     * @param current_time 当前时间(秒)
     * @param total_time 总时长(秒)
     */
    virtual void OnPlaybackProgress(float progress, float current_time, float total_time) = 0;
};
}; // namespace av
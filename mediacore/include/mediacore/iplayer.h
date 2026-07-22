/**
 * @file iplayer.h
 * @brief 播放器的 Facade 接口 UI层和mediacore 内核唯一的边界契约
 * @details UI(PlayerWidget/ControllerWidget)依赖此接口，不感知 Demuxer/Decoder/Pipeline 等
 *          任何底层实现细节
 */

#pragma once

#include <cstdint>
#include <string>

#include "playback_state.h"

namespace mediacore {

/**
 * @brief 播放器统一操作契约入口
 */
class Player {
public:
    /**
     * @brief 虚析构函数
     */
    virtual ~Player() = default;

    /**
     * @brief 打开媒体源，异步进入 kOpening 状态
     */
    virtual void Open(const std::string& url) = 0;

    /**
     * @brief 开始/恢复播放，驱动状态跃迁到 kPlaying
     */
    virtual void Play() = 0;

    /**
     * @brief 暂停播放，驱动状态跃迁到 kPaused
     */
    virtual void Pause() = 0;

    /**
     * @brief 停止播放并拆除底层管线，驱动状态跃迁到 kStopped
     */
    virtual void Stop() = 0;

    /**
     * @brief 跳转到指定时间点
     * @param timestamp_us，跳转的目标时间戳，单位-微秒
     */
    virtual void Seek(int64_t timestamp_us) = 0;

    /**
     * @brief 查询当前播放转装
     */
    virtual PlaybackState GetState() const = 0; 

private:
    // member variables
};
} // namespace mediacore
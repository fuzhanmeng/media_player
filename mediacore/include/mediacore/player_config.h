/**
 * @file player_config.h
 * @brief 播放器启动前需要先定好的参数
 * @details
 * PlayerConfig 保存会影响队列容量和音频缓冲时长的参数。
 * 这些值会在后续 Build Pipeline 时用于创建 Link、Node 或音频设备。
 *
 * 这里暂时不做配置文件解析，也不做环境变量覆盖；先保证代码里有一份清楚、可校验的默认配置。
 *
 * 生命周期：
 * - 在创建 Pipeline 之前先准备一份 PlayerConfig
 * - Build / Open 时读取这些值
 * - 配置构建完成后就按只读值对象使用
 */

#pragma once

#include <cstddef>
#include <string>

namespace mediacore {

/**
 * @struct ConfigValidationResult
 * @brief PlayerConfig 的校验结果
 * @details
 * 这个结构只做最简单的两件事：
 * 1. ok_ 告诉调用方能不能继续用这个配置
 * 2. message_ 保存第一条失败原因
 */
struct ConfigValidationResult {
    bool ok_{true};  ///< true 表示配置合法
    std::string message_{};  ///< ok_ 为 false 时保存第一条错误原因
};

/**
 * @struct DemuxConfig
 * @brief 解封装阶段的压缩包队列容量
 * @details
 * Demuxer 会把从文件或网络里读出来的压缩包先放进这里对应的队列
 * 队列太小，上游容易卡住；队列太大，内存会涨，Stop 也会更慢
 */
struct DemuxConfig {
    std::size_t video_packet_capacity_{200};  ///< 视频压缩包队列最多能放多少个包
    std::size_t audio_packet_capacity_{200};  ///< 音频压缩包队列最多能放多少个包
};

/**
 * @struct DecodeConfig
 * @brief 解码后帧队列容量
 * @details
 * 解码器把压缩包变成帧以后，帧不会立刻显示出来，而是先进入这一层队列
 * 这个值越大，越不容易因为下游偶尔慢一下而卡住，但额外延迟也会变大
 */
struct DecodeConfig {
    std::size_t video_frame_capacity_{3};  ///< 解码后视频帧队列最多缓存多少帧
    std::size_t audio_frame_capacity_{32};  ///< 解码后音频帧队列最多缓存多少帧
};

/**
 * @struct AudioConfig
 * @brief 音频输出设备和音频总缓冲
 * @details
 * 音频通常会比视频更敏感：缓冲太少容易爆音、卡顿，缓冲太多又会让暂停和 Seek 变慢
 */
struct AudioConfig {
    int device_buffer_ms_{200};  ///< 音频设备里先预留多少毫秒的数据
    int max_buffer_ms_{500};  ///< 播放器允许的音频总缓存上限，包含队列和设备缓冲
};

/**
 * @struct PlayerConfig
 * @brief 播放器当前阶段真正会用到的配置
 * @details
 * 当前只保留 Demuxer、Decoder、Audio 近期会用到的字段。
 * 网络、录制、Jitter Buffer 等后期模块的配置等对应模块开始实现时再加入。
 */
struct PlayerConfig {
    DemuxConfig demux_{};  ///< 解封装相关配置
    DecodeConfig decode_{};  ///< 解码相关配置
    AudioConfig audio_{};  ///< 音频输出相关配置

    /**
     * @brief 返回一份使用默认值的配置
     * @return 默认 PlayerConfig
     */
    static PlayerConfig Default();

    /**
     * @brief 检查配置是否合理
     * @return 成功时 ok_ 为 true；失败时 message_ 保存第一条错误原因
     */
    ConfigValidationResult Validate() const;
};

}  // namespace mediacore
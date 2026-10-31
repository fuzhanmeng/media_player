/**
 * @file player_config.h
 * @brief 播放器的队列容量、音频缓存预算和音画同步配置
 * @details
 * 本文件只负责保存配置并声明校验接口，不负责创建队列、Pipeline 或音频设备
 * 具体默认值通过各成员的默认成员初始化提供，校验逻辑实现在 player_config.cpp 中
 */

#pragma once

#include <cstddef>
#include <string>

#include "mediacore/av_sync_config.h"

namespace mediacore {

/**
 * @struct ConfigValidationResult
 * @brief 配置校验结果；失败时 message_ 记录首个错误原因
 * @details
 * ok_ 为 true 表示校验通过，message_ 通常为空
 * ok_ 为 false 表示校验失败，message_ 保存首个发现的问题描述
 */
struct ConfigValidationResult final {
    bool ok_{true};  ///< 配置是否合法可用
    std::string message_{};  ///< 失败时的诊断信息
};

/**
 * @struct DemuxConfig
 * @brief 解封装阶段的压缩包队列容量
 * @details
 * DemuxerNode 将从媒体流解出的压缩数据包（Packet）投递至下游 Link：
 * - 队列过小：可能因解码器偶发抖动而频繁阻塞 Demuxer 线程，引发 I/O 背压过大；
 * - 队列过大：会占用过多内存，并在管线执行 Stop 协议或 Seek 丢包时增加排空（Drain/Flush）耗时。
 */
struct DemuxConfig final {
    std::size_t video_packet_capacity_{200};  ///< 视频压缩包 Link 队列容量上限（包数量）
    std::size_t audio_packet_capacity_{200};  ///< 音频压缩包 Link 队列容量上限（包数量）
};

/**
 * @struct DecodeConfig
 * @brief 解码后的原始帧队列容量
 * @details
 * 解码器将输出的原始图像帧（VideoFrame）与音频采样帧（AudioFrame）缓存至此层队列：
 * - 视频帧队列：通常保持较小深度（默认 3 帧），以控制未经压缩的 YUV/RGB 图像显存占用并减小渲染延迟；
 * - 音频帧队列：默认容纳更多小粒度音频帧（默认 32 帧），防止调度延迟造成声卡断流。
 */
struct DecodeConfig final {
    std::size_t video_frame_capacity_{3};  ///< 解码后原始视频帧 Link 队列容量上限（帧数量）
    std::size_t audio_frame_capacity_{32};  ///< 解码后原始音频帧 Link 队列容量上限（帧数量）
};

/**
 * @struct AudioConfig
 * @brief 音频设备缓冲请求值和音频总缓冲预算。
 *
 * @details
 * device_buffer_ms_ 是向音频输出实现请求的 PCM buffer 时长，单位为毫秒，
 * 并不保证设备最终采用完全相同的容量，也不直接决定设备何时开始播放
 * 起播时机通常由 ALSA 软件参数中的 start_threshold 等设置决定
 *
 * max_buffer_ms_ 是项目用于检查音频缓存配置是否自洽的总预算。
 * 当前校验将解码 PCM 队列、两个在途 PCM 块和请求的设备 buffer 一起估算
 * 这个字段本身不会自动限制运行时的实际积压；运行时流控仍需由节点实现
 */
struct AudioConfig final {
    int device_buffer_ms_{200};  ///< 请求的 PCM buffer 时长（毫秒，允许范围 [5, 2000]）
    int max_buffer_ms_{500};  ///< 音频缓存总预算（毫秒，允许范围 [5, 2000]）
};

/**
 * @struct PlayerConfig
 * @brief 播放器引擎全局启动配置聚合结构
 * @details
 * 汇聚 Demuxer、Decoder 及 Audio 输出的基础配置项。
 * 遵循确定性构造原则，直接使用各子结构的默认安全值就地初始化。
 */
struct PlayerConfig final {
    DemuxConfig demux_{};  ///< 解封装模块参数
    DecodeConfig decode_{};  ///< 解码模块参数
    AudioConfig audio_{};  ///< 音频输出模块参数
    SyncConfig sync_{};  ///< 音画同步与 Seek 超时参数

    /**
     * @brief 创建一份默认播放器配置。
     * @return 各字段采用其默认成员初始化值的 PlayerConfig
     * @note 该函数不执行校验，也不创建任何播放资源。
     */
    static PlayerConfig Default();

    /**
     * @brief 校验与媒体轨道无关的公共配置，供尚未打开媒体的 Player 构造阶段使用。
     * @return 同步与 Seek 参数合法时 ok_ 为 true；否则返回首个错误原因。
     * @details 轨道确认后仍须调用 ValidateForTracks()，避免无关的轨道配置阻止纯音频或纯视频播放。
     */
    ConfigValidationResult ValidateCommon() const;

    /**
     * @brief 校验配置中的全部参数，不考虑媒体实际包含哪些轨道
     * @return 全部校验通过时 ok_ 为 true；否则返回首个错误原因
     * @note 适用于希望在打开媒体前就检查音频、视频两路配置的场景
     */
    ConfigValidationResult Validate() const;

    /**
     * @brief 根据媒体实际存在的轨道校验配置
     * @param has_video 媒体是否包含视频轨
     * @param has_audio 媒体是否包含音频轨
     * @return 通过时 ok_ 为 true；失败时 message_ 说明首个错误原因
     *
     * @details
     * 至少需要一条音频或视频轨。同步配置始终会校验；视频队列配置只在存在视频轨时校验
     * 音频队列及音频缓冲预算只在存在音频轨时校验
     */
    ConfigValidationResult ValidateForTracks(bool has_video, bool has_audio) const;
};

}  // namespace mediacore

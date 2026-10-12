/**
 * @file player_config.h
 * @brief 播放器核心工作参数与队列容量配置定义
 * @details
 * PlayerConfig 统筹管理管线构建期所需的静态配置：
 * 1. 队列缓冲容量：解封装压缩包队列（Demux Packet Queue）与解码原始帧队列（Decode Frame Queue）的深度上限；
 * 2. 音频缓冲参数：底层音频硬件设备环形缓冲时长与软件总缓冲时长的上限；
 * 3. 校验时机：在调用 Pipeline 构建之前准备好该配置，并通过 Validate() 确保所有参数满足自洽闭环条件。
 */

#pragma once

#include <cstddef>
#include <string>

namespace mediacore {

/**
 * @struct ConfigValidationResult
 * @brief PlayerConfig 参数自洽性校验结果
 * @details
 * 采用轻量化的短路错误报告机制：
 * - ok_：指示该配置是否满足启动的最小安全闭环要求；
 * - message_：当 ok_ 为 false 时，记录触发校验失败的首个参数的具体原因。
 */
struct ConfigValidationResult final {
    bool ok_{true};  ///< 配置是否合法可用（true 表示合法）
    std::string message_{};  ///< 校验失败时的错误诊断信息
};

/**
 * @struct DemuxConfig
 * @brief 解封装阶段的压缩包缓冲队列容量配置
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
 * @brief 解码阶段的原始媒体帧缓冲队列容量配置
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
 * @brief 音频输出硬件与软件总缓冲时长配置
 * @details
 * 音频流对时钟抖动极度敏感：
 * - device_buffer_ms_：操作系统音频驱动层预留的固有环形硬件缓冲；过小易发生 Underrun（破音/爆音）；
 * - max_buffer_ms_：管线内部软件缓冲加上设备硬件缓冲的总上限；过大会导致 Seek 或 Pause 响应迟滞。
 */
struct AudioConfig final {
    int device_buffer_ms_{200};  ///< 音频输出设备底层硬件预留缓冲时长（毫秒）
    int max_buffer_ms_{500};  ///< 播放器允许积压的音频总缓冲时长上限（毫秒，必须 >= device_buffer_ms_）
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

    /**
     * @brief 工厂方法：获取一份具备默认安全值的标准配置
     * @return 默认初始化的 PlayerConfig 对象
     */
    static PlayerConfig Default();

    /**
     * @brief 严格校验当前配置参数的合法性与逻辑自洽性
     * @return ConfigValidationResult 校验结果对象；包含成功标记及首个错误原因
     */
    ConfigValidationResult Validate() const;
};

}  // namespace mediacore
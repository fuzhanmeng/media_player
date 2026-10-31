/**
 * @file player_config.cpp
 * @brief PlayerConfig 的默认构造入口和配置校验实现
 * @details
 * 本文件职责严格限定于配置层：
 * 1. 提供工厂方法返回合法的默认播放器配置；
 * 2. 检查配置项是否满足管线启动的最小自洽闭环条件；
 * 3. 不依赖具体的 I/O 文件读取，不解析命令行，不引入任何 Qt 或 FFmpeg 依赖
 */

#include "mediacore/player_config.h"

#include <cstddef>
#include <string>

namespace mediacore {

namespace {

/// 与 IAudioSink 实现约定的最小/最大硬件缓冲时长（毫秒）
constexpr int kMinDeviceBufferMs = 5;
constexpr int kMaxBufferMs = 2000;

/// PCM 解码块时长上限：解码器每块最多 240 帧，即 48 kHz 下的 5ms
constexpr int kMaxPcmBlockDurationMs = 5;
constexpr int kInFlightPcmBlockCount = 2;

/**
 * @brief 检查队列容量是否大于零
 * @param capacity 队列允许容纳的元素数量
 * @param config_name 用于错误信息的配置字段名称
 * @return 容量非零时校验通过，否则返回错误原因
 */
ConfigValidationResult ValidatePositiveCapacity(std::size_t capacity, const char* config_name) {
    if (capacity == 0) {
        return {false, std::string(config_name) + " must be greater than zero"};
    }

    return {true, {}};
}

/**
 * @brief 校验以毫秒表示的时长是否大于零
 * @param duration_ms 时长，单位为毫秒
 * @param config_name 用于描述错误信息的配置字段名称
 * @return 时长大于零时校验通过，否则返回错误原因
 */
ConfigValidationResult ValidatePositiveMilliseconds(int duration_ms, const char* config_name) {
    if (duration_ms <= 0) {
        return {false, std::string(config_name) + " must be greater than zero"};
    }

    return {true, {}};
}

/**
 * @brief 检验音频缓存预算是否合理
 * @param audio 音频设备 buffer 的请求值和总预算配置
 * @param audio_pcm_block_capacity 解码后音频队列可容纳的 PCM 块数量
 * @return 配置合理时通过校验，否则返回首个不满足的约束
 * @details
 * 此处估算的预算由三部分组成：
 *   1. 解码队列：每个 PCM 块按最多 kMaxPcmBlockDurationMs 毫秒估算；
 *   2. 在途数据：按两个 PCM 块估算；
 *   3. 设备端：使用 device_buffer_ms_ 作为请求的 PCM buffer 时长
 *
 * 要求三部分之和不超过 max_buffer_ms_
 */
ConfigValidationResult ValidateAudioBufferBudget(const AudioConfig& audio, std::size_t audio_pcm_block_capacity) {
    ConfigValidationResult result = ValidatePositiveMilliseconds(audio.device_buffer_ms_, "audio.device_buffer_ms");

    if (!result.ok_) {
        return result;
    }

    if (audio.device_buffer_ms_ < kMinDeviceBufferMs || audio.device_buffer_ms_ > kMaxBufferMs) {
        return {false, "audio.device_buffer_ms must be within [5,2000]"};
    }

    result = ValidatePositiveMilliseconds(audio.max_buffer_ms_, "audio.max_buffer_ms");

    if (!result.ok_) {
        return result;
    }

    if (audio.max_buffer_ms_ > kMaxBufferMs) {
        return {false, "audio.max_buffer_ms must not exceed 2000"};
    }

    if (audio.max_buffer_ms_ < audio.device_buffer_ms_) {
        return {false, "audio.max_buffer_ms must be greater than or equal to audio.device_buffer_ms"};
    }

    constexpr int kInFlightMs = kInFlightPcmBlockCount * kMaxPcmBlockDurationMs;

    const int budget_after_device_ms = audio.max_buffer_ms_ - audio.device_buffer_ms_;
    if (budget_after_device_ms < kInFlightMs) {
        return {false, "audio.max_buffer_ms must cover the device buffer and two kInFlightMs PCM block"};
    }

    const int budget_for_queue_ms = budget_after_device_ms - kInFlightMs;
    const auto max_pcm_blocks = static_cast<std::size_t>(budget_for_queue_ms / kMaxPcmBlockDurationMs);

    if (audio_pcm_block_capacity > max_pcm_blocks) {
        return {false,
                "audio PCM queue + two in-flight PCM blocks + requested device buffer "
                "must not exceed audio.max_buffer_ms"};
    }

    return {true, {}};
}

/**
 * @brief 校验音画同步以及 Seek 相关参数
 * @param sync 音画同步配置
 * @return 参数符合校验时通过检查，否则返回首个错误原因
 */
ConfigValidationResult ValidateSyncConfig(const SyncConfig& sync) {
    if (sync.late_drop_us_ <= 0) {
        return {false, "sync.late_drop_us must be greater than zero"};
    }

    if (sync.reanchor_timeout_.count() <= 0) {
        return {false, "sync.reanchor_timeout must be greater than zero"};
    }

    if (sync.seek_timeout_.count() <= 0) {
        return {false, "sync.seek_timeout must be greater than zero"};
    }

    if (sync.seek_timeout_ <= sync.reanchor_timeout_) {
        return {false, "sync.seek_timeout must be greater than sync.reanchor_timeout"};
    }

    return {true, {}};
}

}  // namespace

/**
 * @brief 返回一份具备完整安全初始值的播放器默认配置
 * @details 默认值直接由结构体内部各字段就地初始化定义，此处返回默认实例。
 * @return 包含默认工作参数的 PlayerConfig 对象
 */
PlayerConfig PlayerConfig::Default() {
    return PlayerConfig{};
}

/**
 * @brief 校验与媒体轨道无关的公共配置，供尚未打开媒体的 Player 构造阶段使用。
 * @return 同步与 Seek 参数合法时 ok_ 为 true；否则返回首个错误原因。
 * @details 轨道确认后仍须调用 ValidateForTracks()，避免无关的轨道配置阻止纯音频或纯视频播放。
 */
ConfigValidationResult PlayerConfig::ValidateCommon() const {
    return ValidateSyncConfig(sync_);
}

/**
 * @brief 检查播放器配置是否满足当前阶段的最小闭环要求
 * @details
 * 校验顺序遵循数据流由前至后的正向管线拓扑：
 * 1. 解封装包队列容量（Demux Packet Capacities）；
 * 2. 解码帧队列容量（Decode Frame Capacities）；
 * 3. 音频硬件与总缓冲时长（Audio Buffering Durations）；
 * 4. 跨参数自洽性约束（max_buffer_ms >= device_buffer_ms）。
 *
 * 采用 Short-circuit 策略：发现首个非法参数立即终止并返回，避免多重错误信息淹没根本问题。
 * @return 校验成功返回 ok_ 为 true 的结果；失败返回首个错误原因
 */
ConfigValidationResult PlayerConfig::Validate() const {
    ConfigValidationResult result;

    // 1. 校验解封装阶段的压缩包队列容量
    result = ValidatePositiveCapacity(demux_.video_packet_capacity_, "demux.video_packet_capacity");
    if (!result.ok_) {
        return result;
    }

    result = ValidatePositiveCapacity(demux_.audio_packet_capacity_, "demux.audio_packet_capacity");
    if (!result.ok_) {
        return result;
    }

    // 2. 校验解码后的数据队列容量
    result = ValidatePositiveCapacity(decode_.video_frame_capacity_, "decode.video_frame_capacity");
    if (!result.ok_) {
        return result;
    }

    // audio_frame_capacity_ 的单位是 PCM 块数量，不是单块内的采样帧数量
    result = ValidatePositiveCapacity(decode_.audio_frame_capacity_, "decode.audio_frame_capacity");
    if (!result.ok_) {
        return result;
    }

    // 3. 检查解码队列、在途块与设备 buffer 组合后的音频缓存预算
    result = ValidateAudioBufferBudget(audio_, decode_.audio_frame_capacity_);
    if (!result.ok_) {
        return result;
    }

    // 4. 校验音画同步和 Seek 相关参数
    return ValidateCommon();
}

/**
 * @brief 根据媒体实际包含的音视频轨道，校验播放器配置
 * @param has_video 是否包含视频轨道
 * @param has_audio 是否包含音频轨道
 * @return true 表示校验通过，false 表示校验失败
 * @details
 * 本函数只负责校验配置，不负责解析媒体文件或检测音视频轨道
 */
ConfigValidationResult PlayerConfig::ValidateForTracks(bool has_video, bool has_audio) const {
    // 1. 检查媒体是否至少包含了一条轨道
    if (!has_audio && !has_video) {
        return {false, "media has neither a video nor an audio stream"};
    }

    // 2. 检验播放器的公共同步配置：无论是纯音频/视频还是混合媒体，都执行此检查
    ConfigValidationResult result = ValidateCommon();
    if (!result.ok_) {
        return result;
    }

    // 3. 如果媒体包含视频轨道，校验视频相关配置
    if (has_video) {
        // 视频压缩包队列容量必须 > 0
        result = ValidatePositiveCapacity(demux_.video_packet_capacity_, "demux.video_packet_capacity");

        if (!result.ok_) {
            return result;
        }

        // 视频解码帧队列容量必须 > 0
        result = ValidatePositiveCapacity(decode_.video_frame_capacity_, "decode.video_frame_capacity");

        if (!result.ok_) {
            return result;
        }
    }

    // 4. 如果媒体包含音频轨道，校验音频相关配置
    if (has_audio) {
        // 音频压缩包队列容量必须 > 0
        result = ValidatePositiveCapacity(demux_.audio_packet_capacity_, "demux.audio_packet_capacity");

        if (!result.ok_) {
            return result;
        }

        // 音频解码队列容量必须 > 0, 这里的容量表示 PCM 块的数量， 不是 PCM 块中采样帧的数量
        result = ValidatePositiveCapacity(decode_.audio_frame_capacity_, "decode.audio_frame_capacity");

        if (!result.ok_) {
            return result;
        }

        // 检测音频设备缓冲时长、总缓冲预算以及解码队列之间的约束
        return ValidateAudioBufferBudget(audio_, decode_.audio_frame_capacity_);
    }

    return {true, {}};
}

}  // namespace mediacore

/**
 * @file player_config.cpp
 * @brief PlayerConfig 默认值构建与参数合法性校验实现
 * @details
 * 本文件职责严格限定于配置层：
 * 1. 提供工厂方法返回合法的默认播放器配置；
 * 2. 检查配置项是否满足管线启动的最小自洽闭环条件；
 * 3. 不依赖具体的 I/O 文件读取，不解析命令行，不引入任何 Qt 或 FFmpeg 依赖。
 */

#include "mediacore/player_config.h"

#include <cstddef>
#include <string>

namespace mediacore {

namespace {

/**
 * @brief 校验队列容量必须大于 0
 * @param value 待校验的队列容量值
 * @param name 配置项路径名称（用于构造错误提示信息）
 * @return 校验结果；若 value 为 0 则返回错误原因
 * @details
 * name 来源于固定字符串常量，因此使用 const char* 接收，零动态分配开销；
 * 仅在校验失败分支构造 std::string 描述文本。
 */
ConfigValidationResult ValidatePositiveCapacity(std::size_t value, const char* name) {
    if (value == 0) {
        return {false, std::string(name) + " must be greater than zero"};
    }
    return {true, {}};
}

/**
 * @brief 校验毫秒级时间跨度必须大于 0
 * @param value 待校验的时间长度（单位：毫秒）
 * @param name 配置项路径名称（用于构造错误提示信息）
 * @return 校验结果；若 value <= 0 则返回错误原因
 * @details
 * 毫秒参数选用有符号 int 接收，目的是在校验阶段能精准拦截负数配置；
 * 若使用无符号类型接收，负数（如 -1）会被隐式类型转换为极大的正整数，导致校验失效。
 */
ConfigValidationResult ValidatePositiveDuration(int value, const char* name) {
    if (value <= 0) {
        return {false, std::string(name) + " must be greater than zero"};
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
    // 1. 解封装阶段视频压缩包队列容量校验
    // 解封装阶段会为视频压缩包创建有界 Link 队列。
    // 容量为 0 时，Demuxer 读取的 Packet 无处缓冲，下游 Link/ThreadSafeQueue 构造也会直接抛出异常。
    ConfigValidationResult result =
        ValidatePositiveCapacity(demux_.video_packet_capacity_, "demux.video_packet_capacity");
    if (!result.ok_) {
        return result;
    }

    // 2. 解封装阶段音频压缩包队列容量校验
    // 与视频包队列一致，必须保证至少能容纳一个压缩包。
    // Demuxer 解析出音频流数据包时需送入该队列，不可为 0。
    result = ValidatePositiveCapacity(demux_.audio_packet_capacity_, "demux.audio_packet_capacity");
    if (!result.ok_) {
        return result;
    }

    // 3. 视频原始帧队列容量校验
    // 视频帧队列用于缓存解码后的原始未压缩图像数据（VideoFrame）。
    // 若容量为 0，视频解码器无法完成向渲染节点或同步模块的帧投递。
    result = ValidatePositiveCapacity(decode_.video_frame_capacity_, "decode.video_frame_capacity");
    if (!result.ok_) {
        return result;
    }

    // 4. 音频原始帧队列容量校验
    // 音频帧队列用于存储解码后的原始 PCM 音频帧。
    // 该容量将直接作为底层 Link/ThreadSafeQueue 的上限，必须大于 0。
    result = ValidatePositiveCapacity(decode_.audio_frame_capacity_, "decode.audio_frame_capacity");
    if (!result.ok_) {
        return result;
    }

    // 5. 音频底层设备硬件缓冲时长校验
    // 表示声卡/操作系统底层音频后端自身持有的固有环形缓冲时长。
    // 值为 0 或负数在物理实现上无效，会导致音频设备打开初始化失败。
    result = ValidatePositiveDuration(audio_.device_buffer_ms_, "audio.device_buffer_ms");
    if (!result.ok_) {
        return result;
    }

    // 6. 音频总缓冲上限时长校验
    // 表示“管线内部软件队列 + 底层音频硬件”总计允许积压的最大音频时长。
    // 该值过小容易因调度抖动产生欠载破音，过大则会导致 Seek 或 Pause 时旧声音残留延迟明显。
    result = ValidatePositiveDuration(audio_.max_buffer_ms_, "audio.max_buffer_ms");
    if (!result.ok_) {
        return result;
    }

    // 7. 音频参数自洽性校验
    // 总缓存上限必须大于或等于底层设备固有硬件缓冲。
    // 若设备硬件本身需缓存 200ms，而管线总上限限制在 100ms，参数构成逻辑冲突，拒绝通过。
    if (audio_.max_buffer_ms_ < audio_.device_buffer_ms_) {
        return {false, "audio.max_buffer_ms must be greater than or equal to audio.device_buffer_ms"};
    }

    return {true, {}};
}

}  // namespace mediacore
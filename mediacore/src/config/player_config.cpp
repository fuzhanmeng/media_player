/**
 * @file player_config.cpp
 * @brief PlayerConfig 默认值与校验规则
 * @details
 * 这个文件只做两件事：
 * 1. 给配置结构提供默认值
 * 2. 检查这些默认值或外部传入值是否自洽
 *
 * 它不负责读文件、不负责解析命令行，也不碰 Qt / FFmpeg
 */

#include "mediacore/player_config.h"

namespace mediacore {

namespace {
/**
 * @brief 校验队列容量必须大于 0
 * @param value 待校验容量
 * @param name 配置项名称，用于生成错误文本
 * @return 成功或失败结果
 * @details
 * name 只来自固定配置项名称，所以这里用 const char* 接收即可
 * 只有真正出错时才构造 std::string 组成错误文本
 */
ConfigValidationResult ValidatePositiveCapacity(std::size_t value, const char* name) {
    if (value == 0) {
        return {false, std::string(name) + " must be greater than zero"};
    }

    return {};
}

/**
 * @brief 校验毫秒级时间必须大于 0
 * @param value 待校验时长
 * @param name 配置项名称，用于生成错误文本
 * @return 成功或失败结果
 * @details
 * 毫秒值使用 int 是为了允许校验阶段发现负数
 * 如果一开始用无符号类型，-1 可能会被转换成一个很大的正数，反而更难发现错误
 */
ConfigValidationResult ValidatePositiveDuration(int value, const char* name) {
    if (value <= 0) {
        return {false, std::string(name) + " must be greater than zero"};
    }

    return {};
}

}  // namespace

/**
 * @brief 返回一份完整的播放器默认配置
 * @details
 * 默认值统一写在结构体成员初始化里，这里只是把它们组装成一份完整对象返回
 *
 * @return 使用默认参数的 PlayerConfig
 */
PlayerConfig PlayerConfig::Default() {
    return PlayerConfig{};
}

/**
 * @brief 检查播放器配置是否满足当前阶段的最小闭环要求
 * @details
 * 检查顺序按数据流从前到后排：
 * 1. 解封装队列
 * 2. 解码队列
 * 3. 音频缓冲
 *
 * 出错后只返回第一条原因，避免一次报太多条把真正问题淹没
 *
 * @return 配置合法返回 ok_ 为 true 的结果；否则返回第一条错误原因
 */
ConfigValidationResult PlayerConfig::Validate() const {
    // 解封装阶段会为视频压缩包创建一条有界队列
    // 容量为 0 时，Demuxer 读出来的 AVPacket 没有地方放，后续 Link/Queue 构造也会失败
    ConfigValidationResult result =
        ValidatePositiveCapacity(demux_.video_packet_capacity_, "demux.video_packet_capacity");
    if (!result.ok_) {
        return result;
    }

    // 音频压缩包队列和视频压缩包队列一样，都必须至少能容纳一个包
    // 后面本地文件有音频轨时，Demuxer 会把音频 AVPacket 送进这个队列
    result = ValidatePositiveCapacity(demux_.audio_packet_capacity_, "demux.audio_packet_capacity");
    if (!result.ok_) {
        return result;
    }

    // 视频帧队列保存解码后的原始画面
    // 这里不能为 0，否则 Decoder 解出来的 VideoFrame 无法交给渲染或同步模块
    result = ValidatePositiveCapacity(decode_.video_frame_capacity_, "decode.video_frame_capacity");
    if (!result.ok_) {
        return result;
    }

    // 音频帧队列保存解码后的 PCM 数据
    // 当前还没接音频输出，但这个容量会在音频解码阶段直接传给对应的 Link/Queue
    result = ValidatePositiveCapacity(decode_.audio_frame_capacity_, "decode.audio_frame_capacity");
    if (!result.ok_) {
        return result;
    }

    // 音频设备缓冲表示系统音频设备自己先吃进去多少毫秒的数据
    // 为 0 或负数没有物理意义，也会让后续音频打开参数不可信
    result = ValidatePositiveDuration(audio_.device_buffer_ms_, "audio.device_buffer_ms");
    if (!result.ok_) {
        return result;
    }

    // 音频总缓存上限表示“播放器内部队列 + 音频设备”最多允许积压多少毫秒
    // 这个值太小容易卡顿，太大则会让 Pause / Seek 后旧声音残留更久
    result = ValidatePositiveDuration(audio_.max_buffer_ms_, "audio.max_buffer_ms");
    if (!result.ok_) {
        return result;
    }

    // 总缓存上限必须大于等于设备缓冲
    // 如果设备自己就要缓存 200ms，而总上限只允许 100ms，这份配置本身就是矛盾的
    if (audio_.max_buffer_ms_ < audio_.device_buffer_ms_) {
        return {false, "audio.max_buffer_ms must be greater than or equal to audio.device_buffer_ms"};
    }

    return {};
}

}  // namespace mediacore
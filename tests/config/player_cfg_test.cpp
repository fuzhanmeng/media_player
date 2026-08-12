/**
 * @file player_config_tests.cpp
 * @brief PlayerConfig 默认值和校验规则测试。
 */

#include <iostream>
#include <stdexcept>
#include <string>

#include "mediacore/player_config.h"

namespace {

/**
 * @brief 测试断言失败时抛出的异常。
 */
class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

/**
 * @brief 要求条件成立，否则终止当前测试。
 */
void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

/**
 * @brief 默认配置必须完整且可通过校验。
 */
void DefaultConfigIsValid() {
    const mediacore::PlayerConfig config = mediacore::PlayerConfig::Default();
    const mediacore::ConfigValidationResult result = config.Validate();

    Require(result.ok_, result.message_);
    Require(config.demux_.video_packet_capacity_ == 200, "unexpected default demux.video_packet_capacity");
    Require(config.demux_.audio_packet_capacity_ == 200, "unexpected default demux.audio_packet_capacity");
    Require(config.decode_.video_frame_capacity_ == 3, "unexpected default decode.video_frame_capacity");
    Require(config.decode_.audio_frame_capacity_ == 32, "unexpected default decode.audio_frame_capacity");
    Require(config.audio_.device_buffer_ms_ == 200, "unexpected default audio.device_buffer_ms");
    Require(config.audio_.max_buffer_ms_ == 500, "unexpected default audio.max_buffer_ms");
}

/**
 * @brief 容量为 0 时必须拒绝，避免后续有界队列永久不可用。
 */
void RejectZeroCapacity() {
    mediacore::PlayerConfig config = mediacore::PlayerConfig::Default();
    config.demux_.video_packet_capacity_ = 0;

    const mediacore::ConfigValidationResult result = config.Validate();
    Require(!result.ok_, "zero capacity should be rejected");
    Require(result.message_ == "demux.video_packet_capacity must be greater than zero",
            "unexpected zero capacity error");
}

/**
 * @brief 音频总缓冲不能小于设备缓冲，否则预算本身互相矛盾。
 */
void RejectInvalidAudioBudget() {
    mediacore::PlayerConfig config = mediacore::PlayerConfig::Default();
    config.audio_.device_buffer_ms_ = 600;
    config.audio_.max_buffer_ms_ = 500;

    const mediacore::ConfigValidationResult result = config.Validate();
    Require(!result.ok_, "invalid audio budget should be rejected");
    Require(result.message_ == "audio.max_buffer_ms must be greater than or equal to audio.device_buffer_ms",
            "unexpected audio budget error");
}

/**
 * @brief 毫秒级时间不能为负数。
 */
void RejectNegativeDuration() {
    mediacore::PlayerConfig config = mediacore::PlayerConfig::Default();
    config.audio_.device_buffer_ms_ = -1;

    const mediacore::ConfigValidationResult result = config.Validate();
    Require(!result.ok_, "negative duration should be rejected");
    Require(result.message_ == "audio.device_buffer_ms must be greater than zero", "unexpected duration error");
}

}  // namespace

int main() {
    try {
        DefaultConfigIsValid();
        RejectZeroCapacity();
        RejectInvalidAudioBudget();
        RejectNegativeDuration();
    } catch (const std::exception& error) {
        std::cerr << "player_config_tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "player_config_tests passed\n";
    return 0;
}
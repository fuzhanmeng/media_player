/**
 * @file media_error_test.cpp
 * @brief MediaError 基础行为测试。
 */

#include "mediacore/media_error.h"

#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

/**
 * @class TestFailure
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
 * @brief 默认错误对象应该表示“没有错误”。
 */
void DefaultErrorIsEmpty() {
    const mediacore::MediaError error;
    Require(!error.HasError(), "default MediaError should not contain an error");
    Require(error.GetCategory() == mediacore::MediaErrorCategory::kNone, "unexpected default GetCategory");
    Require(error.Code() == 0, "unexpected default code");
}

/**
 * @brief FFmpeg 错误需要保留模块、错误码和上下文文本。
 */
void FfmpegErrorKeepsContext() {
    const mediacore::MediaError error = mediacore::MediaError::FromFfmpeg("Demuxer", -2, "open input failed");

    Require(error.HasError(), "FFmpeg MediaError should contain an error");
    Require(error.GetCategory() == mediacore::MediaErrorCategory::kFfmpeg, "unexpected FFmpeg GetCategory");
    Require(error.Module() == "Demuxer", "unexpected FFmpeg module");
    Require(error.Code() == -2, "unexpected FFmpeg code");
    Require(error.Message().find("open input failed") != std::string::npos, "missing FFmpeg context message");
}

/**
 * @brief 系统错误需要把 errno 转成可读文本。
 */
void SystemErrorKeepsErrno() {
    const mediacore::MediaError error = mediacore::MediaError::FromSystem("FileReader", ENOENT, "open file failed");

    Require(error.HasError(), "system MediaError should contain an error");
    Require(error.GetCategory() == mediacore::MediaErrorCategory::kSystem, "unexpected system GetCategory");
    Require(error.Code() == ENOENT, "unexpected errno");
    Require(error.Message().find("open file failed") != std::string::npos, "missing system context message");
}

/**
 * @brief 状态错误不需要底层错误码。
 */
void StateErrorHasNoCode() {
    const mediacore::MediaError error = mediacore::MediaError::FromState("PlayerFacade", "Play before Open");

    Require(error.HasError(), "state MediaError should contain an error");
    Require(error.GetCategory() == mediacore::MediaErrorCategory::kState, "unexpected state GetCategory");
    Require(error.Code() == 0, "state error should not require a code");
    Require(!error.IsRecoverable(), "state error should default to non-recoverable");
}

}  // namespace

int main() {
    try {
        DefaultErrorIsEmpty();
        FfmpegErrorKeepsContext();
        SystemErrorKeepsErrno();
        StateErrorHasNoCode();
    } catch (const std::exception& error) {
        std::cerr << "media_error_test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "media_error_test passed\n";
    return 0;
}

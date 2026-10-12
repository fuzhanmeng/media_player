/**
 * @file player_control_test.cpp
 * @brief 从 IPlayer 公共接口验证打开后自动播放、暂停恢复和停止。
 * @details 使用临时 Y4M 文件和可控的视频接收端，不依赖 GUI 或外部媒体。
 */

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "mediacore/av_types/video_frame.h"
#include "mediacore/ivideo_sink.h"
#include "mediacore/player_factory.h"

namespace {

using namespace std::chrono_literals;
using mediacore::PlaybackState;

constexpr int kFrameCount = 8;
constexpr std::int64_t kFrameDurationUs = 40000;
constexpr auto kWaitTimeout = 2s;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryVideo final {
public:
    TemporaryVideo() {
        const auto temporary_root = std::filesystem::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = temporary_root /
                ("ai-media-player-control-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                directory_ = std::move(candidate);
                break;
            }
            Require(!error, "failed to create temporary directory: " + error.message());
        }
        Require(!directory_.empty(), "failed to choose a unique temporary directory");

        try {
            path_ = directory_ / "video.y4m";
            std::ofstream output(path_, std::ios::binary);
            Require(output.is_open(), "failed to create test video");
            output << "YUV4MPEG2 W4 H4 F25:1 Ip A1:1 C420jpeg\n";
            std::array<unsigned char, 24> frame{};
            frame.fill(128);
            for (int index = 0; index < kFrameCount; ++index) {
                output << "FRAME\n";
                output.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
            }
            output.close();
            Require(output.good(), "failed to write test video");
        } catch (...) {
            Cleanup();
            throw;
        }
    }

    ~TemporaryVideo() { Cleanup(); }
    TemporaryVideo(const TemporaryVideo&) = delete;
    TemporaryVideo& operator=(const TemporaryVideo&) = delete;

    std::string Path() const { return path_.string(); }

private:
    void Cleanup() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
};

/** @brief 控制每次帧回调何时返回，避免视频在测试命令到达前就已经播放结束。 */
class GatedVideoSink final : public mediacore::IVideoSink {
public:
    void OnReceiveVideoFrame(std::shared_ptr<mediacore::VideoFrame> frame) override {
        std::unique_lock<std::mutex> lock(mutex_);
        frame_pts_.push_back(frame ? frame->pts_us_ : std::nullopt);
        const std::size_t callback_number = frame_pts_.size();
        cv_.notify_all();
        cv_.wait(lock, [&] { return release_all_ || callbacks_allowed_to_return_ >= callback_number; });
    }

    void WaitForFrames(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        Require(cv_.wait_for(lock, kWaitTimeout, [&] { return frame_pts_.size() >= count; }),
                "timed out waiting for video frame " + std::to_string(count));
    }

    std::optional<std::int64_t> FramePts(std::size_t index) {
        std::lock_guard<std::mutex> lock(mutex_);
        return frame_pts_.at(index);
    }

    void ReleaseOne() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++callbacks_allowed_to_return_;
        cv_.notify_all();
    }

    void ReleaseAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        release_all_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::optional<std::int64_t>> frame_pts_;
    std::size_t callbacks_allowed_to_return_{0};
    bool release_all_{false};
};

/** @brief 即使断言失败，也先放行帧回调，再回收播放器线程。 */
class PlayerSession final {
public:
    PlayerSession() : player(mediacore::CreatePlayer()) {
        Require(player != nullptr, "failed to create player");
        player->AttachDisplayView(&sink);
    }

    ~PlayerSession() {
        sink.ReleaseAll();
        player->DetachDisplayView(&sink);
        player.reset();
    }

    GatedVideoSink sink;
    std::unique_ptr<mediacore::IPlayer> player;
};

void WaitForState(const mediacore::IPlayer& player, PlaybackState expected) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (player.GetState() == expected) {
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error(std::string("expected player state ") + mediacore::PlaybackStateToString(expected) +
                             ", got " + mediacore::PlaybackStateToString(player.GetState()));
}

void OpenThenPlayStartsPlayback(const TemporaryVideo& media) {
    PlayerSession session;
    session.player->Open(media.Path());
    session.player->Play();
    session.sink.WaitForFrames(1);
    WaitForState(*session.player, PlaybackState::kPlaying);
}

void PauseKeepsMediaAndPlayContinues(const TemporaryVideo& media) {
    PlayerSession session;
    session.player->Open(media.Path());
    WaitForState(*session.player, PlaybackState::kPaused);
    session.player->Play();
    session.sink.WaitForFrames(1);

    session.player->Pause();
    WaitForState(*session.player, PlaybackState::kPaused);
    Require(session.player->GetDurationUs() == kFrameCount * kFrameDurationUs, "Pause should retain the current media");

    session.player->Play();
    WaitForState(*session.player, PlaybackState::kPlaying);
    session.sink.ReleaseOne();
    session.sink.WaitForFrames(2);
    Require(session.sink.FramePts(0) == 0 && session.sink.FramePts(1) == kFrameDurationUs,
            "Play after Pause should continue with the next frame rather than reopen the video");
}

void StopReleasesMedia(const TemporaryVideo& media) {
    PlayerSession session;
    session.player->Open(media.Path());
    WaitForState(*session.player, PlaybackState::kPaused);
    session.player->Play();
    session.sink.WaitForFrames(1);

    session.player->Stop();
    session.sink.ReleaseAll();
    WaitForState(*session.player, PlaybackState::kStopped);
    Require(session.player->GetDurationUs() == -1, "Stop should release the current media information");
}

}  // namespace

int main() {
    try {
        const TemporaryVideo media;
        OpenThenPlayStartsPlayback(media);
        PauseKeepsMediaAndPlayContinues(media);
        StopReleasesMedia(media);
    } catch (const std::exception& error) {
        std::cerr << "player_control_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "player_control_test passed\n";
    return 0;
}

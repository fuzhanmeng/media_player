/**
 * @file playback_timeline_test.cpp
 * @brief 音频结束后的时钟切换回归测试。
 */

#include "av_sync/playback_timeline.h"
#include "core/pipeline.h"
#include "sink/audio_sink_node.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScriptedAudioSink final : public mediacore::IAudioSink {
public:
    void Open() override {}

    std::size_t Write(const std::int16_t*, std::size_t frames) override { return frames; }

    std::int64_t DelayFrames() override { return drained_ ? 0 : kQueuedFrames; }

    std::int64_t WritableFrames() override { return std::numeric_limits<std::int64_t>::max(); }

    bool Drain() override {
        drained_ = true;
        return true;
    }

    void Flush() override { drained_ = true; }
    void Pause(bool) override {}

    std::string DescribeState() const override {
        return drained_ ? "scripted drained" : "scripted queued";
    }

    static constexpr std::int64_t kQueuedFrames = 19'200;

private:
    bool drained_{false};
};

void AudioClockUpdatesAreIgnoredAfterAudioEnds() {
    auto timeline = std::make_shared<mediacore::PlaybackTimeline>(true, true);
    timeline->SetPlaying(true);
    timeline->PublishAudio(0, 1'000'000);

    Require(timeline->Get().media_position_us_ == 1'000'000, "audio clock did not establish its initial position");
    Require(!timeline->MarkEnded(mediacore::PlaybackTrack::kAudio, 0),
            "audio ending should not end a media that still has video");

    timeline->PublishAudio(0, 1'500'000);
    const auto position_after_late_update = timeline->Get().media_position_us_;
    Require(position_after_late_update < 1'100'000,
            "late audio clock update moved the timeline after audio had ended");

    timeline->BeginSeek(1, 2'000'000);
    timeline->CompleteSeek(true);
    timeline->PublishAudio(1, 2'050'000);
    Require(timeline->Get().media_position_us_ == 2'050'000,
            "a new seek epoch should accept audio clock updates after the previous epoch ended");
}

void AudioSinkDrainDoesNotJumpTheTimeline() {
    using namespace std::chrono_literals;

    constexpr std::size_t frame_count = 280;
    auto timeline = std::make_shared<mediacore::PlaybackTimeline>(false, true);
    timeline->SetPlaying(true);

    std::mutex mutex;
    std::condition_variable condition;
    bool media_ended = false;
    std::int64_t ended_position = 0;
    mediacore::PipelineEventCallbacks callbacks;
    callbacks.on_media_ended_ = [&](mediacore::MediaEnded) {
        const auto position = timeline->Get().media_position_us_;
        std::lock_guard<std::mutex> lock(mutex);
        ended_position = position;
        media_ended = true;
        condition.notify_all();
    };

    mediacore::Pipeline pipeline(std::move(callbacks));
    auto input = pipeline.CreateLink<mediacore::StreamItem<mediacore::PcmFrame>>(frame_count + 1);
    auto& sink = pipeline.EmplaceNode<mediacore::AudioSinkNode>(timeline,
                                                                 std::make_unique<ScriptedAudioSink>(),
                                                                 200);
    Require(sink.ConnectInput(input), "failed to connect scripted source to AudioSinkNode");

    for (std::size_t index = 0; index < frame_count; ++index) {
        mediacore::PcmFrame frame;
        frame.samples_.resize(mediacore::PcmFrame::kMaxBlockFrames * mediacore::PcmFrame::kChannels);
        frame.start_sample_ = static_cast<std::int64_t>(index * mediacore::PcmFrame::kMaxBlockFrames);
        Require(input->Send(mediacore::StreamItem<mediacore::PcmFrame>(std::move(frame))),
                "failed to queue PCM frame");
    }
    Require(input->Send(mediacore::StreamItem<mediacore::PcmFrame>(mediacore::EndOfStream{0})),
            "failed to queue audio EOS");
    Require(pipeline.Start() == mediacore::PipelineStartResult::kStarted, "audio test pipeline failed to start");
    Require(pipeline.Resume(), "audio test pipeline failed to resume");

    {
        std::unique_lock<std::mutex> lock(mutex);
        Require(condition.wait_for(lock, 2s, [&] { return media_ended; }),
                "AudioSinkNode did not report media end");
    }

    Require(ended_position >= 1'000'000 && ended_position < 1'100'000,
            "Drain published the cleared device delay as a timeline jump");

    std::this_thread::sleep_for(30ms);
    const auto final_position = timeline->Get().media_position_us_;
    Require(final_position >= ended_position && final_position < 1'350'000,
            "audio clock kept moving after EOS instead of following the monotonic fallback");
    Require(pipeline.Stop(), "audio test pipeline failed to stop");
}

}  // namespace

int main() {
    try {
        AudioClockUpdatesAreIgnoredAfterAudioEnds();
        AudioSinkDrainDoesNotJumpTheTimeline();
    } catch (const std::exception& error) {
        std::cerr << "playback_timeline_test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "playback_timeline_test passed\n";
    return 0;
}

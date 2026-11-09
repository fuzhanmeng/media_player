/**
 * @file audio_sink_watchdog_test.cpp
 * @brief AudioSinkNode watchdog pause/resume regression tests.
 */

#include "audio_decoder/iaudio_sink.h"
#include "audio_decoder/pcm_frame.h"
#include "av_sync/playback_timeline.h"
#include "core/pipeline.h"
#include "sink/audio_sink_node.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct SinkProbe final {
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t writable_queries{0};
    std::vector<bool> pause_states;
    bool node_error{false};
    std::string node_error_message;
};

class PermanentlyFullSink final : public mediacore::IAudioSink {
public:
    explicit PermanentlyFullSink(std::shared_ptr<SinkProbe> probe) : probe_(std::move(probe)) {}

    void Open() override {}
    std::size_t Write(const std::int16_t*, std::size_t) override { return 0; }
    std::int64_t DelayFrames() override { return 0; }

    std::int64_t WritableFrames() override {
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            ++probe_->writable_queries;
        }
        probe_->condition.notify_all();
        return 0;
    }

    bool Drain() override { return false; }
    void Flush() override {}

    void Pause(bool paused) override {
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            probe_->pause_states.push_back(paused);
        }
        probe_->condition.notify_all();
    }

    std::string DescribeState() const override { return "test device permanently full"; }

private:
    std::shared_ptr<SinkProbe> probe_;
};

class StalledAudioPipeline final {
public:
    StalledAudioPipeline()
        : timeline_(std::make_shared<mediacore::PlaybackTimeline>(false, true)),
          probe_(std::make_shared<SinkProbe>()),
          pipeline_(MakeCallbacks()),
          input_(pipeline_.CreateLink<mediacore::StreamItem<mediacore::PcmFrame>>(2)) {
        timeline_->SetPlaying(true);
        auto& node = pipeline_.EmplaceNode<mediacore::AudioSinkNode>(
            timeline_, std::make_unique<PermanentlyFullSink>(probe_), 200);
        Require(node.ConnectInput(input_), "failed to connect full-device test sink");

        mediacore::PcmFrame frame;
        frame.samples_.resize(mediacore::PcmFrame::kMaxBlockFrames * mediacore::PcmFrame::kChannels);
        frame.start_sample_ = 0;
        Require(input_->Send(mediacore::StreamItem<mediacore::PcmFrame>(std::move(frame))),
                "failed to queue test PCM frame");
        Require(pipeline_.Start() == mediacore::PipelineStartResult::kStarted,
                "full-device test pipeline failed to start");
        Require(pipeline_.Resume(), "full-device test pipeline failed to resume");
    }

    bool WaitForWritableQueries(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(probe_->mutex);
        return probe_->condition.wait_for(lock, timeout, [&] { return probe_->writable_queries >= count; });
    }

    bool WaitForPauseState(bool paused, std::size_t minimum_calls, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(probe_->mutex);
        return probe_->condition.wait_for(lock, timeout, [&] {
            return probe_->pause_states.size() >= minimum_calls && probe_->pause_states.back() == paused;
        });
    }

    bool WaitForError(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(probe_->mutex);
        return probe_->condition.wait_for(lock, timeout, [&] { return probe_->node_error; });
    }

    bool HasError() const {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        return probe_->node_error;
    }

    std::string ErrorMessage() const {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        return probe_->node_error_message;
    }

    void Pause() {
        timeline_->SetPlaying(false);
        Require(pipeline_.Pause(), "full-device test pipeline failed to pause");
    }

    void Resume() {
        timeline_->SetPlaying(true);
        Require(pipeline_.Resume(), "full-device test pipeline failed to resume after pause");
    }

    bool Stop() { return pipeline_.Stop(); }

private:
    mediacore::PipelineEventCallbacks MakeCallbacks() {
        mediacore::PipelineEventCallbacks callbacks;
        callbacks.on_media_ended_ = [](mediacore::MediaEnded) {};
        callbacks.on_node_error_ = [probe = probe_](mediacore::NodeError event) {
            {
                std::lock_guard<std::mutex> lock(probe->mutex);
                probe->node_error = true;
                probe->node_error_message = std::move(event.message_);
            }
            probe->condition.notify_all();
        };
        return callbacks;
    }

    std::shared_ptr<mediacore::PlaybackTimeline> timeline_;
    std::shared_ptr<SinkProbe> probe_;
    mediacore::Pipeline pipeline_;
    std::shared_ptr<mediacore::Link<mediacore::StreamItem<mediacore::PcmFrame>>> input_;
};

void PauseLongerThanWatchdogDoesNotCauseImmediateFailure() {
    StalledAudioPipeline session;

    // Multiple zero-writable observations guarantee the watchdog had started before pausing.
    Require(session.WaitForWritableQueries(3, 1s), "audio node did not observe the full device");
    session.Pause();
    Require(session.WaitForPauseState(true, 2, 1s), "audio sink did not enter paused state");

    Require(!session.WaitForError(2300ms), "watchdog fired while the pipeline was paused");

    session.Resume();
    Require(session.WaitForPauseState(false, 3, 1s), "audio sink did not resume playback");
    Require(!session.WaitForError(300ms), "stale pre-pause watchdog time caused an immediate failure");
    Require(session.Stop(), "paused watchdog test pipeline failed to stop");
}

void ContinuousFullDeviceStillTripsWatchdog() {
    StalledAudioPipeline session;
    Require(session.WaitForWritableQueries(3, 1s), "audio node did not observe the full device");
    Require(session.WaitForError(3s), "watchdog did not report a continuously full audio device");
    Require(session.ErrorMessage().find("audio device stopped consuming audio data") != std::string::npos,
            "continuous-full-device failure did not report the watchdog error");
    Require(session.Stop(), "continuous watchdog test pipeline failed to stop");
}

}  // namespace

int main() {
    try {
        PauseLongerThanWatchdogDoesNotCauseImmediateFailure();
        ContinuousFullDeviceStillTripsWatchdog();
    } catch (const std::exception& error) {
        std::cerr << "audio_sink_watchdog_test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "audio_sink_watchdog_test passed\n";
    return 0;
}

/**
 * @file ffmpeg_demuxer_node_test.cpp
 * @brief FFmpeg 视频解复用器的打开、读包、Seek 和取消测试。
 * @details 测试自行生成微型 Y4M 文件，不需要外部媒体、编码工具或网络。
 */

#include "demux/ffmpeg_demuxer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

namespace {

constexpr int kWidth = 4;
constexpr int kHeight = 4;
constexpr int kFrameCount = 3;
constexpr int kFrameRate = 25;
constexpr std::size_t kFrameSize = kWidth * kHeight * 3 / 2;
constexpr std::int64_t kFrameDurationUs = 1000000 / kFrameRate;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

/** @brief 独占一个临时目录，异常或测试结束时清理生成的媒体文件。 */
class TemporaryY4m final {
public:
    TemporaryY4m() {
        const auto temporary_root = std::filesystem::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = temporary_root /
                ("ai-media-player-demux-test-" + std::to_string(random()) + "-" + std::to_string(random()));
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
            Require(output.is_open(), "failed to create temporary Y4M file");
            output << "YUV4MPEG2 W4 H4 F25:1 Ip A1:1 C420jpeg\n";
            for (int index = 0; index < kFrameCount; ++index) {
                std::array<unsigned char, kFrameSize> frame{};
                frame.fill(128);
                std::fill_n(frame.begin(), kWidth * kHeight, static_cast<unsigned char>(16 + index * 20));
                output << "FRAME\n";
                output.write(reinterpret_cast<const char*>(frame.data()),
                             static_cast<std::streamsize>(frame.size()));
            }
            output.close();
            Require(output.good(), "failed to write temporary Y4M file");
        } catch (...) {
            Cleanup();
            throw;
        }
    }

    ~TemporaryY4m() { Cleanup(); }
    TemporaryY4m(const TemporaryY4m&) = delete;
    TemporaryY4m& operator=(const TemporaryY4m&) = delete;

    std::string Path() const { return path_.string(); }
    std::string MissingPath() const { return (directory_ / "missing.y4m").string(); }

private:
    void Cleanup() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
};

mediacore::FfmpegDemuxerOpenResult OpenVideo(
    const TemporaryY4m& media, mediacore::DemuxerInterruptHandle interrupt = {}) {
    auto opened = mediacore::FfmpegDemuxer::Open(media.Path(), std::move(interrupt));
    Require(!opened.error_.HasError(), "video open failed: " + opened.error_.ToString());
    Require(opened.demuxer_ != nullptr, "successful open did not return a demuxer");
    Require(opened.video_codec_parameters_.IsValid(), "successful open did not return video parameters");
    return opened;
}

void RequireFfmpegError(const mediacore::MediaError& error, int code, const std::string& operation) {
    Require(error.HasError(), operation + " should report an error");
    Require(error.GetCategory() == mediacore::MediaErrorCategory::kFfmpeg,
            operation + " should report an FFmpeg error");
    Require(error.Module() == "FfmpegDemuxer", operation + " reported the wrong module");
    Require(error.Code() == code, operation + " reported an unexpected error code");
}

/** @brief Open 返回的视频信息和参数副本不依赖 Demuxer 的生命周期。 */
void OpensVideoAndCopiesParameters(const TemporaryY4m& media) {
    auto opened = OpenVideo(media);
    const mediacore::MediaInfo info = opened.demuxer_->GetMediaInfo();
    Require(info.IsValid(), "opened media information should be valid");
    Require(info.container_name_ == "yuv4mpegpipe", "unexpected container name");
    Require(info.video_stream_.has_value(), "opened media has no video stream");
    Require(!info.audio_stream_.has_value(), "Y4M input should contain only video");
    Require(info.can_seek_, "local Y4M file should support seeking");
    Require(info.duration_us_ == kFrameCount * kFrameDurationUs, "unexpected media duration");

    const mediacore::VideoStreamInfo& video = *info.video_stream_;
    Require(video.stream_index_ == opened.demuxer_->GetVideoStreamIndex(), "unexpected video stream index");
    Require(video.codec_id_ == AV_CODEC_ID_RAWVIDEO, "unexpected video codec");
    Require(video.width_ == kWidth && video.height_ == kHeight, "unexpected video size");
    Require(video.pixel_format_ == mediacore::VideoPixelFormat::kYuv420p, "unexpected video pixel format");
    Require(video.frame_rate_.numerator_ == kFrameRate && video.frame_rate_.denominator_ == 1,
            "unexpected video frame rate");
    Require(video.time_base_.numerator_ == 1 && video.time_base_.denominator_ == kFrameRate,
            "unexpected video time base");
    Require(video.duration_us_ == kFrameCount * kFrameDurationUs, "unexpected video duration");

    auto parameters = std::move(opened.video_codec_parameters_);
    opened.demuxer_.reset();
    const AVCodecParameters* raw_parameters = parameters.Get();
    Require(raw_parameters != nullptr, "video parameters were lost when the demuxer was destroyed");
    Require(raw_parameters->codec_type == AVMEDIA_TYPE_VIDEO && raw_parameters->codec_id == AV_CODEC_ID_RAWVIDEO,
            "unexpected copied video codec parameters");
    Require(raw_parameters->width == video.width_ && raw_parameters->height == video.height_,
            "copied video dimensions do not match media information");
    Require(raw_parameters->format == AV_PIX_FMT_YUV420P, "unexpected copied pixel format");
    Require(info.IsValid(), "copied media information became invalid after demuxer destruction");
}

void RequireFrame(mediacore::FfmpegDemuxer& demuxer, mediacore::Packet& packet, int index) {
    const auto read = demuxer.Read(packet);
    Require(read.status_ == mediacore::FfmpegReadStatus::kPacket,
            "expected a video packet: " + read.error_.ToString());
    Require(!read.error_.HasError(), "successful packet read reported an error");
    Require(packet.GetStreamIndex() == demuxer.GetVideoStreamIndex(), "packet came from the wrong stream");
    Require(packet.GetSizeBytes() == kFrameSize, "unexpected packet size");
    Require(packet.GetPtsUs() == index * kFrameDurationUs, "unexpected packet presentation timestamp");
    Require(packet.GetDtsUs() == index * kFrameDurationUs, "unexpected packet decoding timestamp");
    Require(packet.GetDurationUs() == kFrameDurationUs, "unexpected packet duration");
    const AVPacket* raw_packet = packet.GetAVPacket();
    Require(raw_packet->pts == index, "unexpected underlying packet timestamp");
    Require(raw_packet->data != nullptr && raw_packet->data[0] == 16 + index * 20,
            "unexpected frame payload");
}

void RequireEof(mediacore::FfmpegDemuxer& demuxer, mediacore::Packet& packet) {
    const auto read = demuxer.Read(packet);
    Require(read.status_ == mediacore::FfmpegReadStatus::kEndOfStream, "expected end of stream");
    Require(!read.error_.HasError(), "end of stream should not report an error");
    Require(packet.GetSizeBytes() == 0 && !packet.HasPts() && !packet.HasDts(),
            "end of stream left stale packet metadata");
}

/** @brief 读取全部帧后保持 EOF；Seek 成功后可以再次读取同一视频。 */
void ReadsUntilEofAndResumesAfterSeek(const TemporaryY4m& media) {
    auto opened = OpenVideo(media);
    mediacore::Packet packet;
    for (int index = 0; index < kFrameCount; ++index) {
        RequireFrame(*opened.demuxer_, packet, index);
    }
    RequireEof(*opened.demuxer_, packet);
    RequireEof(*opened.demuxer_, packet);

    const auto seek = opened.demuxer_->Seek(0);
    Require(seek.IsSucceeded(), "seek after EOF failed: " + seek.error_.ToString());
    Require(!seek.error_.HasError(), "successful seek reported an error");
    for (int index = 0; index < kFrameCount; ++index) {
        RequireFrame(*opened.demuxer_, packet, index);
    }
    RequireEof(*opened.demuxer_, packet);
}

void ReportsErrorsAndCancelledOpen(const TemporaryY4m& media) {
    auto empty = mediacore::FfmpegDemuxer::Open("");
    RequireFfmpegError(empty.error_, AVERROR(EINVAL), "empty URI");
    Require(!empty.demuxer_ && !empty.video_codec_parameters_.IsValid(),
            "failed open returned usable media objects");

    auto missing = mediacore::FfmpegDemuxer::Open(media.MissingPath());
    RequireFfmpegError(missing.error_, AVERROR(ENOENT), "missing file");
    Require(!missing.demuxer_ && !missing.video_codec_parameters_.IsValid(),
            "missing input returned usable media objects");

    mediacore::DemuxerInterruptHandle interrupt;
    interrupt.RequestStop();
    auto cancelled = mediacore::FfmpegDemuxer::Open(media.Path(), interrupt);
    RequireFfmpegError(cancelled.error_, AVERROR_EXIT, "cancelled open");
    Require(!cancelled.demuxer_ && !cancelled.video_codec_parameters_.IsValid(),
            "cancelled open returned usable media objects");

    auto opened = OpenVideo(media);
    const auto seek = opened.demuxer_->Seek(-1);
    Require(seek.status_ == mediacore::FfmpegSeekStatus::kError, "negative seek target should fail");
    RequireFfmpegError(seek.error_, AVERROR(EINVAL), "negative seek target");
    mediacore::Packet packet;
    RequireFrame(*opened.demuxer_, packet, 0);
}

/** @brief 外部共享 Stop 请求会打断 Read 和 Seek，且不会被重新记录基准清除。 */
void SharedStopInterruptsReadAndSeek(const TemporaryY4m& media) {
    mediacore::DemuxerInterruptHandle interrupt;
    auto opened = OpenVideo(media, interrupt);
    interrupt.RequestStop();
    opened.demuxer_->PrepareNextIoForSeekInterrupt();
    mediacore::Packet packet;
    const auto read = opened.demuxer_->Read(packet);
    Require(read.status_ == mediacore::FfmpegReadStatus::kStopInterrupted, "shared stop did not interrupt read");
    Require(!read.error_.HasError(), "stop interruption should not report an error");
    const auto seek = opened.demuxer_->Seek(0);
    Require(seek.status_ == mediacore::FfmpegSeekStatus::kStopInterrupted, "shared stop did not interrupt seek");
    Require(!seek.error_.HasError(), "stop interruption should not report an error");
}

}  // namespace

int main() {
    try {
        const TemporaryY4m media;
        OpensVideoAndCopiesParameters(media);
        ReadsUntilEofAndResumesAfterSeek(media);
        ReportsErrorsAndCancelledOpen(media);
        SharedStopInterruptsReadAndSeek(media);
    } catch (const std::exception& error) {
        std::cerr << "ffmpeg_demuxer_node_test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "ffmpeg_demuxer_node_test passed\n";
    return 0;
}

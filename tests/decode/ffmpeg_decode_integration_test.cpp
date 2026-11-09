/**
 * @file ffmpeg_decode_integration_test.cpp
 * @brief Deterministic compressed A/V decode, drain, flush, and epoch integration tests.
 * @details Creates a short MPEG-4 Part 2/AAC Matroska fixture with FFmpeg's built-in encoders.
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "audio_decoder/ffmpeg_audio_decoder.h"
#include "audio_decoder/ffmpeg_audio_decoder_node.h"
#include "audio_decoder/pcm_frame.h"
#include "av_types/audio_codec_parameters.h"
#include "av_types/packet.h"
#include "core/pipeline.h"
#include "core/stream_item.h"
#include "demux/ffmpeg_demuxer.h"
#include "mediacore/av_types/video_frame.h"
#include "mediacore/player_config.h"
#include "video_decoder/ffmpeg_video_decoder.h"
#include "video_decoder/ffmpeg_video_decoder_node.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

namespace {

using namespace std::chrono_literals;
using mediacore::AudioCodecParameters;
using mediacore::EndOfStream;
using mediacore::FfmpegAudioDecoder;
using mediacore::FfmpegAudioDecoderNode;
using mediacore::FfmpegDemuxer;
using mediacore::FfmpegReadStatus;
using mediacore::FfmpegVideoDecoder;
using mediacore::FfmpegVideoDecoderNode;
using mediacore::Packet;
using mediacore::PcmFrame;
using mediacore::PcmFrameStreamItem;
using PacketStreamItem = mediacore::StreamItem<Packet>;
using mediacore::StreamReset;
using mediacore::VideoCodecParameters;
using mediacore::VideoFrame;
using mediacore::VideoFrameStreamItem;

constexpr int kVideoWidth = 64;
constexpr int kVideoHeight = 64;
constexpr int kVideoFrameCount = 30;
constexpr int kAudioFrameCount = 48;
constexpr std::uint64_t kInitialEpoch = 0;
constexpr auto kTestTimeout = 10s;

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string FfmpegError(int error) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, message, sizeof(message));
    return message;
}

void CheckFfmpeg(int result, const std::string& operation) {
    if (result < 0) {
        throw std::runtime_error(operation + " failed: " + FfmpegError(result));
    }
}

class TemporaryMediaDirectory final {
public:
    TemporaryMediaDirectory() {
        std::random_device random;
        const auto root = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = root / ("media-player-compressed-av-test-" + std::to_string(random()) + "-" +
                                     std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                directory_ = std::move(candidate);
                break;
            }
            Require(!error, "failed to create test directory: " + error.message());
        }
        Require(!directory_.empty(), "failed to choose a unique test directory");
        path_ = directory_ / "compressed_av.mkv";
    }

    ~TemporaryMediaDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    TemporaryMediaDirectory(const TemporaryMediaDirectory&) = delete;
    TemporaryMediaDirectory& operator=(const TemporaryMediaDirectory&) = delete;

    const std::filesystem::path& Path() const noexcept { return path_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) {
            if (context->pb != nullptr) {
                avio_closep(&context->pb);
            }
            avformat_free_context(context);
        }
    }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

struct Encoder final {
    CodecContextPtr context;
    AVStream* stream{nullptr};
};

struct EncodedPacket final {
    PacketPtr packet;
    bool video{false};
};

Encoder CreateVideoEncoder(AVFormatContext* output) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    Require(codec != nullptr, "FFmpeg build has no native MPEG-4 Part 2 encoder");

    Encoder encoder;
    encoder.context.reset(avcodec_alloc_context3(codec));
    Require(encoder.context != nullptr, "failed to allocate video encoder context");
    AVCodecContext* context = encoder.context.get();
    context->codec_type = AVMEDIA_TYPE_VIDEO;
    context->codec_id = codec->id;
    context->width = kVideoWidth;
    context->height = kVideoHeight;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = AVRational{1, 25};
    context->framerate = AVRational{25, 1};
    context->bit_rate = 160000;
    context->gop_size = 12;
    context->max_b_frames = 2;
    if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    CheckFfmpeg(avcodec_open2(context, codec, nullptr), "open MPEG-4 Part 2 encoder");

    encoder.stream = avformat_new_stream(output, nullptr);
    Require(encoder.stream != nullptr, "failed to create video output stream");
    encoder.stream->time_base = context->time_base;
    CheckFfmpeg(avcodec_parameters_from_context(encoder.stream->codecpar, context),
                "copy video encoder parameters");
    return encoder;
}

Encoder CreateAudioEncoder(AVFormatContext* output) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    Require(codec != nullptr, "FFmpeg build has no native AAC encoder");

    Encoder encoder;
    encoder.context.reset(avcodec_alloc_context3(codec));
    Require(encoder.context != nullptr, "failed to allocate audio encoder context");
    AVCodecContext* context = encoder.context.get();
    context->codec_type = AVMEDIA_TYPE_AUDIO;
    context->codec_id = codec->id;
    context->sample_rate = 48000;
    context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context->time_base = AVRational{1, context->sample_rate};
    context->bit_rate = 64000;
    av_channel_layout_default(&context->ch_layout, 2);
    if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    CheckFfmpeg(avcodec_open2(context, codec, nullptr), "open AAC encoder");

    encoder.stream = avformat_new_stream(output, nullptr);
    Require(encoder.stream != nullptr, "failed to create audio output stream");
    encoder.stream->time_base = context->time_base;
    CheckFfmpeg(avcodec_parameters_from_context(encoder.stream->codecpar, context),
                "copy audio encoder parameters");
    return encoder;
}

void CollectEncodedPackets(AVCodecContext* context, bool video, std::vector<EncodedPacket>& packets) {
    for (;;) {
        PacketPtr packet(av_packet_alloc());
        Require(packet != nullptr, "failed to allocate encoded packet");
        const int result = avcodec_receive_packet(context, packet.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return;
        }
        CheckFfmpeg(result, "receive encoded packet");
        packets.push_back(EncodedPacket{std::move(packet), video});
    }
}

void EncodeFrame(AVCodecContext* context, AVFrame* frame, bool video, std::vector<EncodedPacket>& packets) {
    for (;;) {
        const int result = avcodec_send_frame(context, frame);
        if (result == AVERROR(EAGAIN)) {
            CollectEncodedPackets(context, video, packets);
            continue;
        }
        CheckFfmpeg(result, "send frame to encoder");
        break;
    }
    CollectEncodedPackets(context, video, packets);
}

void DrainEncoder(AVCodecContext* context, bool video, std::vector<EncodedPacket>& packets) {
    for (;;) {
        const int result = avcodec_send_frame(context, nullptr);
        if (result == AVERROR(EAGAIN)) {
            CollectEncodedPackets(context, video, packets);
            continue;
        }
        if (result != AVERROR_EOF) {
            CheckFfmpeg(result, "start encoder drain");
        }
        break;
    }

    for (;;) {
        PacketPtr packet(av_packet_alloc());
        Require(packet != nullptr, "failed to allocate encoded drain packet");
        const int result = avcodec_receive_packet(context, packet.get());
        if (result == AVERROR_EOF) {
            return;
        }
        CheckFfmpeg(result, "receive encoder drain packet");
        packets.push_back(EncodedPacket{std::move(packet), video});
    }
}

FramePtr MakeVideoFrame(AVCodecContext* context, int index) {
    FramePtr frame(av_frame_alloc());
    Require(frame != nullptr, "failed to allocate source video frame");
    frame->format = context->pix_fmt;
    frame->width = context->width;
    frame->height = context->height;
    frame->pts = index;
    CheckFfmpeg(av_frame_get_buffer(frame.get(), 32), "allocate source video pixels");
    CheckFfmpeg(av_frame_make_writable(frame.get()), "make source video frame writable");

    for (int y = 0; y < context->height; ++y) {
        auto* row = frame->data[0] + y * frame->linesize[0];
        for (int x = 0; x < context->width; ++x) {
            row[x] = static_cast<std::uint8_t>(16 + (x * 3 + y * 5 + index * 7) % 220);
        }
    }
    for (int plane = 1; plane < 3; ++plane) {
        for (int y = 0; y < context->height / 2; ++y) {
            std::fill_n(frame->data[plane] + y * frame->linesize[plane], context->width / 2,
                        static_cast<std::uint8_t>(plane == 1 ? 96 + index % 32 : 160 - index % 32));
        }
    }
    return frame;
}

FramePtr MakeAudioFrame(AVCodecContext* context, int sample_count, std::int64_t pts) {
    FramePtr frame(av_frame_alloc());
    Require(frame != nullptr, "failed to allocate source audio frame");
    frame->format = context->sample_fmt;
    frame->sample_rate = context->sample_rate;
    frame->nb_samples = sample_count;
    frame->pts = pts;
    CheckFfmpeg(av_channel_layout_copy(&frame->ch_layout, &context->ch_layout), "copy source audio layout");
    CheckFfmpeg(av_frame_get_buffer(frame.get(), 0), "allocate source audio samples");
    CheckFfmpeg(av_samples_set_silence(frame->data, 0, sample_count, context->ch_layout.nb_channels,
                                       context->sample_fmt),
                "initialize source audio samples");
    return frame;
}

void WriteFixture(const std::filesystem::path& path, bool& saw_b_frames) {
    AVFormatContext* raw_output = nullptr;
    CheckFfmpeg(avformat_alloc_output_context2(&raw_output, nullptr, "matroska", path.c_str()),
                "allocate Matroska output context");
    Require(raw_output != nullptr, "Matroska output context is null");
    FormatContextPtr output(raw_output);

    Encoder video_encoder = CreateVideoEncoder(output.get());
    Encoder audio_encoder = CreateAudioEncoder(output.get());
    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        CheckFfmpeg(avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE), "open temporary media file");
    }
    CheckFfmpeg(avformat_write_header(output.get(), nullptr), "write temporary media header");

    std::vector<EncodedPacket> packets;
    for (int index = 0; index < kVideoFrameCount; ++index) {
        auto frame = MakeVideoFrame(video_encoder.context.get(), index);
        EncodeFrame(video_encoder.context.get(), frame.get(), true, packets);
    }
    DrainEncoder(video_encoder.context.get(), true, packets);

    const int audio_frame_size = audio_encoder.context->frame_size > 0 ? audio_encoder.context->frame_size : 1024;
    std::int64_t audio_pts = 0;
    for (int index = 0; index < kAudioFrameCount; ++index) {
        auto frame = MakeAudioFrame(audio_encoder.context.get(), audio_frame_size, audio_pts);
        EncodeFrame(audio_encoder.context.get(), frame.get(), false, packets);
        audio_pts += audio_frame_size;
    }
    DrainEncoder(audio_encoder.context.get(), false, packets);

    for (auto& entry : packets) {
        AVStream* stream = entry.video ? video_encoder.stream : audio_encoder.stream;
        AVCodecContext* encoder = entry.video ? video_encoder.context.get() : audio_encoder.context.get();
        if (entry.video && entry.packet->pts != AV_NOPTS_VALUE && entry.packet->dts != AV_NOPTS_VALUE &&
            entry.packet->pts != entry.packet->dts) {
            saw_b_frames = true;
        }
        av_packet_rescale_ts(entry.packet.get(), encoder->time_base, stream->time_base);
        entry.packet->stream_index = stream->index;
    }
    std::stable_sort(packets.begin(), packets.end(), [video_tb = video_encoder.stream->time_base,
                                                       audio_tb = audio_encoder.stream->time_base](const auto& lhs,
                                                                                                   const auto& rhs) {
        const AVRational left_tb = lhs.video ? video_tb : audio_tb;
        const AVRational right_tb = rhs.video ? video_tb : audio_tb;
        const std::int64_t left_dts = lhs.packet->dts == AV_NOPTS_VALUE ? lhs.packet->pts : lhs.packet->dts;
        const std::int64_t right_dts = rhs.packet->dts == AV_NOPTS_VALUE ? rhs.packet->pts : rhs.packet->dts;
        return av_compare_ts(left_dts, left_tb, right_dts, right_tb) < 0;
    });

    for (auto& entry : packets) {
        CheckFfmpeg(av_interleaved_write_frame(output.get(), entry.packet.get()),
                    "write encoded packet to Matroska");
    }
    CheckFfmpeg(av_write_trailer(output.get()), "write temporary media trailer");
    if (output->pb != nullptr) {
        CheckFfmpeg(avio_closep(&output->pb), "close temporary media file");
    }
}

struct OpenedMedia final {
    std::unique_ptr<FfmpegDemuxer> demuxer;
    VideoCodecParameters video_parameters;
    AudioCodecParameters audio_parameters;
};

OpenedMedia OpenMedia(const std::filesystem::path& path) {
    auto opened = FfmpegDemuxer::Open(path.string());
    Require(!opened.error_.HasError(), "failed to open generated media: " + opened.error_.ToString());
    Require(opened.demuxer_ != nullptr, "successful media open returned no demuxer");
    Require(opened.video_codec_parameters_.IsValid(), "generated media has no video codec parameters");
    Require(opened.audio_codec_parameters_.IsValid(), "generated media has no audio codec parameters");
    const auto& info = opened.demuxer_->GetMediaInfo();
    Require(info.video_stream_.has_value() && info.audio_stream_.has_value(),
            "generated media does not expose both audio and video streams");
    Require(info.video_stream_->codec_id_ == AV_CODEC_ID_MPEG4, "unexpected generated video codec");
    Require(info.audio_stream_->codec_id_ == AV_CODEC_ID_AAC, "unexpected generated audio codec");
    return OpenedMedia{std::move(opened.demuxer_), std::move(opened.video_codec_parameters_),
                       std::move(opened.audio_codec_parameters_)};
}

std::unique_ptr<FfmpegVideoDecoder> OpenVideoDecoder(VideoCodecParameters parameters,
                                                     const mediacore::Rational& time_base) {
    auto opened = FfmpegVideoDecoder::Open(std::move(parameters), time_base, mediacore::HardwareAccel::kOff);
    Require(opened.Succeeded(), "failed to open software video decoder: " + opened.error_.ToString());
    return std::move(opened.video_decoder_);
}

std::unique_ptr<FfmpegAudioDecoder> OpenAudioDecoder(AudioCodecParameters parameters,
                                                     const mediacore::Rational& time_base) {
    auto opened = FfmpegAudioDecoder::Open(std::move(parameters), time_base);
    Require(opened.Successed(), "failed to open audio decoder: " + opened.media_error_.ToString());
    return std::move(opened.audio_decoder_);
}

struct PacketBatch final {
    std::vector<Packet> video;
    std::vector<Packet> audio;
};

void AppendPacket(PacketBatch& batch, int video_index, int audio_index, Packet packet) {
    if (packet.GetStreamIndex() == video_index) {
        batch.video.push_back(std::move(packet));
    } else if (packet.GetStreamIndex() == audio_index) {
        batch.audio.push_back(std::move(packet));
    } else {
        throw std::runtime_error("demuxer returned a packet from an unexpected stream");
    }
}

PacketBatch ReadAllPackets(FfmpegDemuxer& demuxer) {
    PacketBatch batch;
    for (;;) {
        Packet packet;
        const auto read = demuxer.Read(packet);
        if (read.status_ == FfmpegReadStatus::kEndOfStream) {
            return batch;
        }
        if (read.status_ == FfmpegReadStatus::kSkipped) {
            continue;
        }
        Require(read.status_ == FfmpegReadStatus::kPacket,
                "demux failed while reading fixture: " + read.error_.ToString());
        AppendPacket(batch, demuxer.GetVideoStreamIndex(), demuxer.GetAudioStreamIndex(), std::move(packet));
    }
}

PacketBatch ReadPacketPrefix(FfmpegDemuxer& demuxer, std::size_t packets_per_track) {
    PacketBatch batch;
    while (batch.video.size() < packets_per_track || batch.audio.size() < packets_per_track) {
        Packet packet;
        const auto read = demuxer.Read(packet);
        if (read.status_ == FfmpegReadStatus::kSkipped) {
            continue;
        }
        Require(read.status_ == FfmpegReadStatus::kPacket,
                "fixture ended before both compressed stream prefixes were available");
        AppendPacket(batch, demuxer.GetVideoStreamIndex(), demuxer.GetAudioStreamIndex(), std::move(packet));
    }
    return batch;
}

Packet ClonePacket(const Packet& source) {
    Packet clone;
    CheckFfmpeg(av_packet_ref(clone.GetAVPacket(), source.GetAVPacket()), "clone stale compressed packet");
    clone.SetEpoch(source.GetEpoch());
    return clone;
}

void PushPackets(const std::vector<Packet>& packets,
                 std::uint64_t epoch,
                 const std::shared_ptr<mediacore::Link<PacketStreamItem>>& input) {
    for (const auto& original : packets) {
        Packet packet = ClonePacket(original);
        packet.SetEpoch(epoch);
        Require(input->Send(PacketStreamItem{std::move(packet)}), "failed to send compressed packet to decoder node");
    }
}

struct DecodedCounts final {
    std::size_t video_frames{0};
    std::size_t audio_blocks{0};
    std::size_t audio_samples{0};
    bool video_eos{false};
    bool audio_eos{false};
    bool video_reset{false};
    bool audio_reset{false};
    std::optional<std::int64_t> last_video_pts_us;
};

template <typename LinkType, typename Handler>
void PollLink(const std::shared_ptr<LinkType>& link, Handler&& handler) {
    if (auto item = link->ReceiveFor(1ms)) {
        handler(std::move(*item));
    }
}

void ConsumeVideoItem(VideoFrameStreamItem item, std::uint64_t expected_epoch, bool expect_reset,
                      bool allow_stale_before_reset, DecodedCounts& counts) {
    if (auto* frame = std::get_if<VideoFrame>(&item)) {
        Require(frame->IsValid(), "decoder node emitted an invalid video frame");
        if (frame->epoch_ != expected_epoch) {
            Require(allow_stale_before_reset && !counts.video_reset && frame->epoch_ < expected_epoch,
                    "video frame from an old epoch crossed the StreamReset boundary");
            return;
        }
        if (frame->pts_us_.has_value()) {
            if (counts.last_video_pts_us.has_value()) {
                Require(*frame->pts_us_ >= *counts.last_video_pts_us, "decoded B-frame output is not in PTS order");
            }
            counts.last_video_pts_us = frame->pts_us_;
        }
        ++counts.video_frames;
        return;
    }
    if (const auto* reset = std::get_if<StreamReset>(&item)) {
        Require(expect_reset, "unexpected video StreamReset");
        Require(reset->epoch_ == expected_epoch, "video StreamReset used the wrong epoch");
        counts.video_reset = true;
        return;
    }
    const auto* eos = std::get_if<EndOfStream>(&item);
    Require(eos != nullptr, "video output contained an unknown stream item");
    Require(eos->epoch_ == expected_epoch, "video EOS used the wrong epoch");
    counts.video_eos = true;
}

void ConsumeAudioItem(PcmFrameStreamItem item, std::uint64_t expected_epoch, bool expect_reset,
                      bool allow_stale_before_reset, DecodedCounts& counts) {
    if (auto* frame = std::get_if<PcmFrame>(&item)) {
        Require(frame->IsValid(), "decoder node emitted invalid PCM");
        if (frame->epoch_ != expected_epoch) {
            Require(allow_stale_before_reset && !counts.audio_reset && frame->epoch_ < expected_epoch,
                    "PCM from an old epoch crossed the StreamReset boundary");
            return;
        }
        ++counts.audio_blocks;
        counts.audio_samples += frame->FrameCount();
        return;
    }
    if (const auto* reset = std::get_if<StreamReset>(&item)) {
        Require(expect_reset, "unexpected audio StreamReset");
        Require(reset->epoch_ == expected_epoch, "audio StreamReset used the wrong epoch");
        counts.audio_reset = true;
        return;
    }
    const auto* eos = std::get_if<EndOfStream>(&item);
    Require(eos != nullptr, "audio output contained an unknown stream item");
    Require(eos->epoch_ == expected_epoch, "audio EOS used the wrong epoch");
    counts.audio_eos = true;
}

struct DecoderGraph final {
    explicit DecoderGraph(std::unique_ptr<FfmpegVideoDecoder> video_decoder,
                          std::unique_ptr<FfmpegAudioDecoder> audio_decoder)
        : packet_video(pipeline.CreateLink<PacketStreamItem>(256)),
          packet_audio(pipeline.CreateLink<PacketStreamItem>(256)),
          video_output(pipeline.CreateLink<VideoFrameStreamItem>(1024)),
          audio_output(pipeline.CreateLink<PcmFrameStreamItem>(1024)),
          video_node(pipeline.EmplaceNode<FfmpegVideoDecoderNode>(std::move(video_decoder))),
          audio_node(pipeline.EmplaceNode<FfmpegAudioDecoderNode>(std::move(audio_decoder))) {
        Require(video_node.ConnectInput(packet_video), "failed to connect video decoder input");
        Require(video_node.ConnectOutput(video_output), "failed to connect video decoder output");
        Require(audio_node.ConnectInput(packet_audio), "failed to connect audio decoder input");
        Require(audio_node.ConnectOutput(audio_output), "failed to connect audio decoder output");
        Require(pipeline.Start() == mediacore::PipelineStartResult::kStarted, "failed to start decoder pipeline");
        Require(pipeline.Resume(), "failed to resume decoder pipeline");
    }

    mediacore::Pipeline pipeline;
    std::shared_ptr<mediacore::Link<PacketStreamItem>> packet_video;
    std::shared_ptr<mediacore::Link<PacketStreamItem>> packet_audio;
    std::shared_ptr<mediacore::Link<VideoFrameStreamItem>> video_output;
    std::shared_ptr<mediacore::Link<PcmFrameStreamItem>> audio_output;
    FfmpegVideoDecoderNode& video_node;
    FfmpegAudioDecoderNode& audio_node;
};

void DrainOutputs(DecoderGraph& graph, std::uint64_t epoch, bool expect_reset,
                  bool allow_stale_before_reset, DecodedCounts& counts) {
    const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
    while ((!counts.video_eos || !counts.audio_eos) && std::chrono::steady_clock::now() < deadline) {
        PollLink(graph.video_output, [&](VideoFrameStreamItem item) {
            ConsumeVideoItem(std::move(item), epoch, expect_reset, allow_stale_before_reset, counts);
        });
        PollLink(graph.audio_output, [&](PcmFrameStreamItem item) {
            ConsumeAudioItem(std::move(item), epoch, expect_reset, allow_stale_before_reset, counts);
        });
    }
    Require(counts.video_eos && counts.audio_eos, "decoder nodes did not both drain and forward EOS");
}

void CompressedAudioVideoDecodeDrainsBFramesAndEos(const std::filesystem::path& media_path) {
    auto opened = OpenMedia(media_path);
    const auto& info = opened.demuxer->GetMediaInfo();
    auto video_decoder = OpenVideoDecoder(std::move(opened.video_parameters), info.video_stream_->time_base_);
    auto audio_decoder = OpenAudioDecoder(std::move(opened.audio_parameters), info.audio_stream_->time_base_);
    PacketBatch packets = ReadAllPackets(*opened.demuxer);
    Require(packets.video.size() >= static_cast<std::size_t>(kVideoFrameCount),
            "compressed fixture did not contain the expected video packets");
    Require(!packets.audio.empty(), "compressed fixture did not contain AAC packets");

    DecoderGraph graph(std::move(video_decoder), std::move(audio_decoder));
    PushPackets(packets.video, kInitialEpoch, graph.packet_video);
    PushPackets(packets.audio, kInitialEpoch, graph.packet_audio);
    Require(graph.packet_video->Send(PacketStreamItem{EndOfStream{kInitialEpoch}}), "failed to send video EOS");
    Require(graph.packet_audio->Send(PacketStreamItem{EndOfStream{kInitialEpoch}}), "failed to send audio EOS");

    DecodedCounts counts;
    DrainOutputs(graph, kInitialEpoch, false, false, counts);
    Require(counts.video_frames == static_cast<std::size_t>(kVideoFrameCount),
            "video Drain did not emit every encoded frame");
    Require(counts.audio_blocks > 0 && counts.audio_samples > 0, "audio Decode/Drain emitted no PCM");
    Require(graph.pipeline.Stop(), "failed to stop fully drained decoder pipeline");
}

void ResetFlushesDecodersAndRejectsOldEpochPackets(const std::filesystem::path& media_path) {
    auto opened = OpenMedia(media_path);
    const auto& info = opened.demuxer->GetMediaInfo();
    auto video_decoder = OpenVideoDecoder(std::move(opened.video_parameters), info.video_stream_->time_base_);
    auto audio_decoder = OpenAudioDecoder(std::move(opened.audio_parameters), info.audio_stream_->time_base_);

    // Frame-threaded decoders can buffer more than eight packets before the
    // first output. Feed enough of this 30-frame fixture to warm those workers
    // while retaining a partial stream for the subsequent Flush/Epoch check.
    PacketBatch prefix = ReadPacketPrefix(*opened.demuxer, 24);
    Require(opened.demuxer->Seek(500000).IsSucceeded(), "demuxer seek for epoch test failed");
    PacketBatch tail = ReadAllPackets(*opened.demuxer);
    Require(!tail.video.empty() && !tail.audio.empty(), "seek did not produce both compressed tracks");
    Packet stale_video = ClonePacket(prefix.video.front());
    Packet stale_audio = ClonePacket(prefix.audio.front());

    DecoderGraph graph(std::move(video_decoder), std::move(audio_decoder));
    PushPackets(prefix.video, kInitialEpoch, graph.packet_video);
    PushPackets(prefix.audio, kInitialEpoch, graph.packet_audio);

    DecodedCounts initial_counts;
    const auto first_frame_deadline = std::chrono::steady_clock::now() + kTestTimeout;
    while ((initial_counts.video_frames == 0 || initial_counts.audio_blocks == 0) &&
           std::chrono::steady_clock::now() < first_frame_deadline) {
        PollLink(graph.video_output, [&](VideoFrameStreamItem item) {
            ConsumeVideoItem(std::move(item), kInitialEpoch, false, false, initial_counts);
        });
        PollLink(graph.audio_output, [&](PcmFrameStreamItem item) {
            ConsumeAudioItem(std::move(item), kInitialEpoch, false, false, initial_counts);
        });
    }
    Require(initial_counts.video_frames > 0 && initial_counts.audio_blocks > 0,
            "initial epoch did not produce both video and audio before Seek");

    const auto new_epoch = graph.pipeline.BeginNewEpoch();
    Require(new_epoch > kInitialEpoch, "Pipeline did not advance the media epoch");
    Require(graph.packet_video->Send(PacketStreamItem{StreamReset{new_epoch, 500000}}),
            "failed to send video StreamReset");
    Require(graph.packet_audio->Send(PacketStreamItem{StreamReset{new_epoch, 500000}}),
            "failed to send audio StreamReset");

    stale_video.SetEpoch(kInitialEpoch);
    stale_audio.SetEpoch(kInitialEpoch);
    Require(graph.packet_video->Send(PacketStreamItem{std::move(stale_video)}), "failed to send stale video packet");
    Require(graph.packet_audio->Send(PacketStreamItem{std::move(stale_audio)}), "failed to send stale audio packet");
    PushPackets(tail.video, new_epoch, graph.packet_video);
    PushPackets(tail.audio, new_epoch, graph.packet_audio);
    Require(graph.packet_video->Send(PacketStreamItem{EndOfStream{new_epoch}}), "failed to send seek video EOS");
    Require(graph.packet_audio->Send(PacketStreamItem{EndOfStream{new_epoch}}), "failed to send seek audio EOS");

    DecodedCounts counts;
    DrainOutputs(graph, new_epoch, true, true, counts);
    Require(counts.video_reset && counts.audio_reset, "both decoder nodes must forward the new StreamReset");
    Require(counts.video_frames > 0 && counts.audio_blocks > 0,
            "post-seek epoch did not decode both video and audio");
    Require(graph.pipeline.Stop(), "failed to stop decoder pipeline after seek EOS");
}

}  // namespace

int main() {
    try {
        TemporaryMediaDirectory temporary_media;
        bool saw_b_frames = false;
        WriteFixture(temporary_media.Path(), saw_b_frames);
        Require(saw_b_frames, "generated MPEG-4 stream did not contain B-frame reordering");

        CompressedAudioVideoDecodeDrainsBFramesAndEos(temporary_media.Path());
        ResetFlushesDecodersAndRejectsOldEpochPackets(temporary_media.Path());
    } catch (const std::exception& error) {
        std::cerr << "ffmpeg_decode_integration_test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "ffmpeg_decode_integration_test passed\n";
    return 0;
}

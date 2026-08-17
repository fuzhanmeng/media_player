/**
 * @file ffmpeg_demuxer_node_test.cpp
 * @brief FFmpeg 解复用模块的媒体信息转换测试
 * @details
 * 测试直接构造 AVFormatContext、AVStream 和 AVCodecParameters，
 * 用来验证转换函数能不能读出视频流、音频流和整体媒体信息。
 *
 * 这个测试不读取真实媒体文件，也不启动解复用线程。
 * 这样可以先把“FFmpeg 字段到项目 StreamInfo 字段”的转换规则测清楚。
 */

#include "demux/ffmpeg_demuxer_node.h"

#include <iostream>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace {

/**
 * @class TestFailure
 * @brief 测试断言失败时抛出的异常
 */
class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

/**
 * @brief 要求条件成立，否则终止当前测试
 */
void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

/**
 * @brief 创建一条带视频编码参数的测试流
 * @param format_context 测试用的 AVFormatContext，avformat_new_stream() 会把新流挂到它下面
 * @return 新创建的视频流，失败时返回 nullptr
 */
AVStream* AddVideoStream(AVFormatContext* format_context) {
    AVStream* stream = avformat_new_stream(format_context, nullptr);
    if (stream == nullptr) {
        return nullptr;
    }

    stream->time_base = AVRational{1, 90000};
    stream->avg_frame_rate = AVRational{30000, 1001};
    stream->duration = 450000;
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_H264;
    stream->codecpar->format = AV_PIX_FMT_YUV420P;
    stream->codecpar->width = 1920;
    stream->codecpar->height = 1080;
    return stream;
}

/**
 * @brief 创建一条带音频编码参数的测试流
 * @param format_context 测试用的 AVFormatContext，avformat_new_stream() 会把新流挂到它下面
 * @return 新创建的音频流，失败时返回 nullptr
 */
AVStream* AddAudioStream(AVFormatContext* format_context) {
    AVStream* stream = avformat_new_stream(format_context, nullptr);
    if (stream == nullptr) {
        return nullptr;
    }

    stream->time_base = AVRational{1, 48000};
    stream->duration = 240000;
    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    stream->codecpar->codec_id = AV_CODEC_ID_AAC;
    stream->codecpar->format = AV_SAMPLE_FMT_FLTP;
    stream->codecpar->sample_rate = 48000;

    AVChannelLayout stereo_layout = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&stream->codecpar->ch_layout, &stereo_layout);
    return stream;
}

/**
 * @brief 创建一条当前项目暂不处理的字幕流
 * @param format_context 测试用的 AVFormatContext
 * @return 新创建的字幕流，失败时返回 nullptr
 */
AVStream* AddSubtitleStream(AVFormatContext* format_context) {
    AVStream* stream = avformat_new_stream(format_context, nullptr);
    if (stream == nullptr) {
        return nullptr;
    }

    stream->time_base = AVRational{1, 1000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    stream->codecpar->codec_id = AV_CODEC_ID_ASS;
    return stream;
}

/**
 * @brief 创建一条字段不完整的视频流
 * @param format_context 测试用的 AVFormatContext
 * @return 新创建的视频流，失败时返回 nullptr
 */
AVStream* AddInvalidVideoStream(AVFormatContext* format_context) {
    AVStream* stream = AddVideoStream(format_context);
    if (stream == nullptr) {
        return nullptr;
    }

    stream->codecpar->width = 0;
    return stream;
}

/**
 * @brief 验证视频流字段能够正确转换
 */
void ConvertsVideoStream() {
    AVFormatContext* format_context = avformat_alloc_context();
    Require(format_context != nullptr, "failed to allocate format context");

    AVStream* stream = AddVideoStream(format_context);
    Require(stream != nullptr, "failed to create video stream");

    mediacore::VideoStreamInfo video_info;
    const mediacore::MediaError error = mediacore::ConvertVideoStreamInfo(stream, video_info);

    Require(!error.HasError(), "video stream conversion failed: " + error.ToString());
    Require(video_info.stream_index_ == stream->index, "unexpected video stream index");
    Require(video_info.codec_id_ == AV_CODEC_ID_H264, "unexpected video codec id");
    Require(video_info.width_ == 1920 && video_info.height_ == 1080, "unexpected video size");
    Require(video_info.pixel_format_ == mediacore::VideoPixelFormat::kYuv420p,
            "unexpected video pixel format");
    Require(video_info.frame_rate_.numerator_ == 30000 && video_info.frame_rate_.denominator_ == 1001,
            "unexpected video frame rate");
    Require(video_info.time_base_.numerator_ == 1 && video_info.time_base_.denominator_ == 90000,
            "unexpected video time base");
    Require(video_info.duration_us_.has_value() && video_info.duration_us_.value() == 5000000,
            "unexpected video duration");

    avformat_free_context(format_context);
}

/**
 * @brief 验证音频流字段能够正确转换
 */
void ConvertsAudioStream() {
    AVFormatContext* format_context = avformat_alloc_context();
    Require(format_context != nullptr, "failed to allocate format context");

    AVStream* stream = AddAudioStream(format_context);
    Require(stream != nullptr, "failed to create audio stream");

    mediacore::AudioStreamInfo audio_info;
    const mediacore::MediaError error = mediacore::ConvertAudioStreamInfo(stream, audio_info);

    Require(!error.HasError(), "audio stream conversion failed: " + error.ToString());
    Require(audio_info.stream_index_ == stream->index, "unexpected audio stream index");
    Require(audio_info.codec_id_ == AV_CODEC_ID_AAC, "unexpected audio codec id");
    Require(audio_info.sample_rate_hz_ == 48000, "unexpected audio sample rate");
    Require(audio_info.channel_count_ == 2, "unexpected audio channel count");
    Require(audio_info.sample_format_ == mediacore::AudioSampleFormat::kFloatPlanar,
            "unexpected audio sample format");
    Require(audio_info.time_base_.numerator_ == 1 && audio_info.time_base_.denominator_ == 48000,
            "unexpected audio time base");
    Require(audio_info.duration_us_.has_value() && audio_info.duration_us_.value() == 5000000,
            "unexpected audio duration");

    avformat_free_context(format_context);
}

/**
 * @brief 验证 MediaInfo 能同时保存视频和音频流
 */
void ConvertsMediaInfo() {
    AVFormatContext* format_context = avformat_alloc_context();
    Require(format_context != nullptr, "failed to allocate format context");

    format_context->duration = 5 * AV_TIME_BASE;
    Require(AddVideoStream(format_context) != nullptr, "failed to create video stream");
    Require(AddAudioStream(format_context) != nullptr, "failed to create audio stream");

    mediacore::MediaInfo media_info;
    const mediacore::MediaError error = mediacore::ConvertMediaInfo(format_context, media_info);

    Require(!error.HasError(), "media info conversion failed: " + error.ToString());
    Require(media_info.IsValid(), "converted media info should be valid");
    Require(media_info.video_stream_.has_value(), "video stream is missing");
    Require(media_info.audio_stream_.has_value(), "audio stream is missing");
    Require(media_info.duration_us_.has_value() && media_info.duration_us_.value() == 5000000,
            "unexpected media duration");
    Require(!media_info.can_seek_, "test format context should not be seekable");

    avformat_free_context(format_context);
}

/**
 * @brief 验证暂不支持的流不会阻止后面的音视频流继续转换
 */
void SkipsUnsupportedStreamBeforeMediaStreams() {
    AVFormatContext* format_context = avformat_alloc_context();
    Require(format_context != nullptr, "failed to allocate format context");

    Require(AddSubtitleStream(format_context) != nullptr, "failed to create subtitle stream");
    Require(AddVideoStream(format_context) != nullptr, "failed to create video stream");
    Require(AddAudioStream(format_context) != nullptr, "failed to create audio stream");

    mediacore::MediaInfo media_info;
    const mediacore::MediaError error = mediacore::ConvertMediaInfo(format_context, media_info);

    Require(!error.HasError(), "media info conversion should skip subtitle stream: " + error.ToString());
    Require(media_info.video_stream_.has_value(), "video stream should be found after subtitle stream");
    Require(media_info.audio_stream_.has_value(), "audio stream should be found after subtitle stream");

    avformat_free_context(format_context);
}

/**
 * @brief 验证前面的坏视频流不会阻止后面的可用音频流
 */
void UsesLaterValidAudioAfterInvalidVideo() {
    AVFormatContext* format_context = avformat_alloc_context();
    Require(format_context != nullptr, "failed to allocate format context");

    Require(AddInvalidVideoStream(format_context) != nullptr, "failed to create invalid video stream");
    Require(AddAudioStream(format_context) != nullptr, "failed to create audio stream");

    mediacore::MediaInfo media_info;
    const mediacore::MediaError error = mediacore::ConvertMediaInfo(format_context, media_info);

    Require(!error.HasError(), "valid audio stream should keep media info usable: " + error.ToString());
    Require(!media_info.video_stream_.has_value(), "invalid video stream should not be saved");
    Require(media_info.audio_stream_.has_value(), "valid audio stream should be saved");

    avformat_free_context(format_context);
}

/**
 * @brief 验证空指针和没有有效流时会返回 MediaError
 */
void RejectsInvalidInput() {
    mediacore::VideoStreamInfo video_info;
    mediacore::MediaError error = mediacore::ConvertVideoStreamInfo(nullptr, video_info);
    Require(error.HasError(), "null video stream should fail");
    Require(error.GetCategory() == mediacore::MediaErrorCategory::kInternal,
            "null video stream should be an internal error");

    mediacore::MediaInfo media_info;
    error = mediacore::ConvertMediaInfo(nullptr, media_info);
    Require(error.HasError(), "null format context should fail");
    Require(error.GetCategory() == mediacore::MediaErrorCategory::kInternal,
            "null format context should be an internal error");

    AVFormatContext* empty_context = avformat_alloc_context();
    Require(empty_context != nullptr, "failed to allocate empty format context");
    error = mediacore::ConvertMediaInfo(empty_context, media_info);
    Require(error.HasError(), "empty format context should fail");
    Require(error.GetCategory() == mediacore::MediaErrorCategory::kFfmpeg,
            "empty format context should be an FFmpeg metadata error");
    avformat_free_context(empty_context);
}

}  // namespace

int main() {
    try {
        ConvertsVideoStream();
        ConvertsAudioStream();
        ConvertsMediaInfo();
        SkipsUnsupportedStreamBeforeMediaStreams();
        UsesLaterValidAudioAfterInvalidVideo();
        RejectsInvalidInput();
    } catch (const std::exception& error) {
        std::cerr << "ffmpeg_demuxer_node_test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "ffmpeg_demuxer_node_test passed\n";
    return 0;
}

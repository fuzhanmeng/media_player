/**
 * @file muxer.cpp
 * @brief 复用类实现
 * @note 数据流转：
 *   AAC Packet (音频编码器输出)  ──→ WriterAudioPacket() ──→ 音频轨 (index=1)
 *   H.264 Packet (视频编码器输出) ──→ WriterVideoPacket() ──→ 视频轨 (index=0)
 *                                                              ↓
 *                                                        av_interleaved_write_frame
 *                                                              ↓
 *                                                          MP4 文件（按PTS交错排列）
 *
 * Init 顺序：
 *   alloc_output_context → new_stream(video) → new_stream(audio)
 *   → avio_open → write_header（此后 stream->time_base 才最终确定）
 *
 * Close 顺序：
 *   WriterTrailer → avio_closep → avformat_free_context
 */

#include "muxer.h"

#include "define/av_packet.h"
#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace av {

/**
 * @brief 创建复用实例
 */
std::unique_ptr<IMuxer> IMuxer::Create(){
    return std::make_unique<Muxer>();
}

/**
 * @brief 构造函数
 */
Muxer::Muxer() {}

/**
 * @brief 析构函数
 */
Muxer::~Muxer() { Close(); }

/**
 * @brief 初始化复用器
 * @note AVCodecContext 仅用于读取编码参数（宽高/SPS/PPS/采样率等）写入MP4文件头
 *       以及保存 time_base 供写包时做时间基换算，Init后不再持有编码器引用
 */
bool Muxer::Init(const std::string& path, AVCodecContext* audio_ctx, AVCodecContext* video_ctx) {
    // 1. 根据文件扩展名(.mp4)自动选择格式，创建空白容器
    if (avformat_alloc_output_context2(&fmt_ctx_, nullptr, nullptr, path.c_str()) < 0) {
        LOG_ERROR("avformat_alloc_output_context2 failed");
        return false;
    }

    // 2. 创建视频轨（index=0），将H.264编码参数（宽高/SPS/PPS）写入流描述
    video_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!video_stream_) {
        LOG_ERROR("avformat_new_stream video failed");
        return false;
    }
    if (avcodec_parameters_from_context(video_stream_->codecpar, video_ctx) < 0) {
        LOG_ERROR("avcodec_parameters_from_context video failed");
        return false;
    }
    // 保存视频编码器时间基，WriteVideoPacket时换算PTS用
    // 编码器time_base={1,fps}，MP4视频流time_base通常={1,90000}，需要转换
    video_encoder_time_base_ = video_ctx->time_base;

    // 3. 创建音频轨（index=1），将AAC编码参数（采样率/声道/AudioSpecificConfig）写入流描述
    audio_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!audio_stream_) {
        LOG_ERROR("avformat_new_stream audio failed");
        return false;
    }
    if (avcodec_parameters_from_context(audio_stream_->codecpar, audio_ctx) < 0) {
        LOG_ERROR("avcodec_parameters_from_context audio failed");
        return false;
    }
    // 保存音频编码器时间基，WriteAudioPacket时换算PTS用
    audio_encoder_time_base_ = audio_ctx->time_base;

    // 4. 打开磁盘文件（avio_open之后才有文件句柄可以写入）
    if (avio_open(&fmt_ctx_->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
        LOG_ERROR("avio_open failed: {}", path);
        return false;
    }

    // 5. 写MP4文件头（此调用后 stream->time_base 被FFmpeg最终确定，之后才能正确换算PTS）
    if (avformat_write_header(fmt_ctx_, nullptr) < 0) {
        LOG_ERROR("avformat_write_header failed");
        return false;
    }

    return true;
}

/**
 * @brief 将AAC音频包写入音频轨
 * @note av_packet_rescale_ts：从音频编码器时间基换算到MP4音频流时间基
 *       av_interleaved_write_frame：内部按PTS排序，保证音视频交错
 */
void Muxer::WriterAudioPacket(std::shared_ptr<Packet> pkt) {
    AVPacket* raw_pkt = pkt->GetRawPacket();

    // 从音频编码器 time_base 换算到 MP4 音频流 time_base
    av_packet_rescale_ts(raw_pkt, audio_encoder_time_base_, audio_stream_->time_base);
    raw_pkt->stream_index = audio_stream_->index;  // 指定写入音频轨
    av_interleaved_write_frame(fmt_ctx_, raw_pkt);
}

/**
 * @brief 将H.264视频包写入视频轨
 * @note 视频编码器time_base={1,fps}，MP4视频流time_base={1,90000}，
 *       rescale后 pts 数值会变大（如 fps=30 时 pts×3000）
 */
void Muxer::WriterVideoPacket(std::shared_ptr<Packet> pkt) {
    AVPacket* raw_pkt = pkt->GetRawPacket();

    // 从视频编码器 time_base 换算到 MP4 视频流 time_base（通常是 1/90000）
    av_packet_rescale_ts(raw_pkt, video_encoder_time_base_, video_stream_->time_base);
    raw_pkt->stream_index = video_stream_->index;  // 指定写入视频轨
    av_interleaved_write_frame(fmt_ctx_, raw_pkt);
}

/**
 * @brief 写MP4文件尾（录制结束时必须调用）
 * @note av_write_trailer 做三件事：
 *       1. 刷出 av_interleaved_write_frame 内部缓冲的剩余包
 *       2. 写MP4索引表（chunk offset/stbl），播放器靠此跳转进度
 *       3. 完成 moov 盒子的写入
 *       不调用此函数：文件损坏，无法拖动进度条，部分播放器拒绝播放
 */
void Muxer::WriterTrailer() {
    if (fmt_ctx_) {
        av_write_trailer(fmt_ctx_);
    }
}

/**
 * @brief 关闭复用器，释放资源
 * @note 顺序严格：WriterTrailer → avio_closep（关文件句柄）→ avformat_free_context
 *       AVStream* 是 fmt_ctx_ 的子对象，avformat_free_context 会自动释放，不需要单独释放
 */
void Muxer::Close() {
    if (fmt_ctx_) {
        // 关闭文件句柄，将内核缓冲刷到磁盘（必须在 free_context 之前）
        avio_closep(&fmt_ctx_->pb);
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    // AVStream 由 fmt_ctx_ 管理，free_context 后只需置空防悬空访问
    audio_stream_ = nullptr;
    video_stream_ = nullptr;
}

} // namespace av

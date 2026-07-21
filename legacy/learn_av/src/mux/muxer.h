/**
 * @file muxer.h
 * @brief 复用器：将音频(AAC)和视频(H.264)包交错写入MP4文件
 */
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "define/av_packet.h"
#include "mux/interface/imuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace av {

class Muxer : public IMuxer {
public:
    /**
     * @brief 构造函数
     */
    Muxer();

    /**
     * @brief 析构函数
     */
    ~Muxer();

    /**
     * @brief 初始化复用器：创建MP4容器、配置音视频轨道参数、写MP4文件头
     */
    bool Init(const std::string& path, AVCodecContext* audio_ctx, AVCodecContext* video_ctx) override;

    /**
     * @brief 将AAC音频包写入MP4音频轨道
     */
    void WriterAudioPacket(std::shared_ptr<Packet> pkt) override;

    /**
     * @brief 将H.264视频包写入MP4视频轨道
     */
    void WriterVideoPacket(std::shared_ptr<Packet> pkt) override;

    /**
     * @brief 写MP4文件尾：刷出剩余缓冲包并写索引表，录制结束时必须调用
     */
    void WriterTrailer() override;

    /**
     * @brief 关闭文件句柄并释放资源
     */
    void Close() override;

private:
    /// 输出格式上下文：管理MP4容器、所有流及文件IO句柄
    AVFormatContext* fmt_ctx_{nullptr};

    /// 音频轨道（AAC），index由avformat_new_stream自动分配
    AVStream* audio_stream_{nullptr};

    /// 视频轨道（H.264），index由avformat_new_stream自动分配
    AVStream* video_stream_{nullptr};

    /// 音频编码器时间基（如{1,44100}），WriteAudioPacket时换算PTS用
    AVRational audio_encoder_time_base_{0, 1};

    /// 视频编码器时间基（如{1,30}），WriteVideoPacket时换算至MP4流时间基用
    AVRational video_encoder_time_base_{0, 1};
};
} // namespace av

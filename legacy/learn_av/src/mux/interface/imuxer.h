/**
 * @file imuxer.h
 * @brief 复用器纯虚接口
 */
#pragma once

#include <memory>
#include <string>

#include "define/av_packet.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace av {

class IMuxer {
public:
    /**
     * @brief 创建复用实例
     */
    static std::unique_ptr<IMuxer> Create();

    /**
     * @brief 构造函数
     */
    IMuxer() = default;

    /**
     * @brief 析构函数
     */
    virtual ~IMuxer() = default;

    /**
     * @brief 初始化复用器：创建MP4容器、配置音视频轨道、写文件头
     */
    virtual bool Init(const std::string& path, AVCodecContext* audio_ctx, AVCodecContext* video_ctx) = 0;

    /**
     * @brief 将编码后的AAC包写入音频轨道
     */
    virtual void WriterAudioPacket(std::shared_ptr<Packet> pkt) = 0;

    /**
     * @brief 将编码后的H.264包写入视频轨道
     */
    virtual void WriterVideoPacket(std::shared_ptr<Packet> pkt) = 0;

    /**
     * @brief 刷出剩余缓冲包并写MP4索引表，录制结束时必须调用
     */
    virtual void WriterTrailer() = 0;

    /**
     * @brief 关闭文件句柄并释放资源
     */
    virtual void Close() = 0;
};
} // namespace av

/**
 * @file ffmpeg_demuxer_node.h
 * @brief FFmpeg Demuxer Node
 * @details
 * 此模块实现 MediaInfo 以及 音视频流信息的提取和解复用
 */

#pragma once

#include "mediacore/av_types/stream_info.h"
#include "mediacore/media_error.h"

struct AVFormatContext;
struct AVStream;

namespace mediacore {

/**
 * @brief 把 FFmpeg 探测到的视频流信息复制到 VideoStreamInfo
 * @param stream 使用 avformat_find_stream_info() 获取到的 AVStream
 * @param output 成功后写入 VideoStreamInfo 的流编号，编码格式、长度、宽度、帧率、时间基、时长等信息
 * 如果失败，output 会被清空
 * @return 如果成功，返回默认的 MediaError；如果失败，返回具体的错误原因
 */
MediaError ConvertVideoStreamInfo(const AVStream* stream, VideoStreamInfo& output);

/**
 * @brief 把 FFmpeg 探测到的音频流信息复制到 AudioStreamInfo
 * @param stream 使用 avformat_find_ stream_info() 获取到的 AVStream
 * @param output 成功后写入 AudioStreamInfo 的流编号，编码格式，采样率、声道数、声道布局、样本格式、时间基
 * 和时长，失败是 output 会被清空
 * @return 如果成功，返回默认的 MediaError；如果失败，返回具体的错误原因
 */
MediaError ConvertAudioStreamInfo(const AVStream* stream, AudioStreamInfo& output);

/**
 * @brief 把 FFmpeg 探测到的媒体信息复制到 MediaInfo
 * @param format_context avformat_open_input() 和 avformat_find_stream_info() 获取到的
 * AVFormatContext
 * @param output 成功后会写入容器名、总时长、是否可 seek 和第一条可用的视频流和音频流，失败时 output 被清空
 * @return 成功时返回默认的 MediaError，失败时返回具体的错误信息
 */
MediaError ConvertMediaInfo(const AVFormatContext* format_context, MediaInfo& output);
}  // namespace mediacore

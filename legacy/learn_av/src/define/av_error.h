/**
 * @file av_error.h
 * @brief 播放器全局错误码定义
 * @details 采用分段设计，通过首位数字可以快速定位错误发生的模块
 */
#pragma once

namespace av {

// 采用 Google C++ Style 命名规范 (kCamelCase)
enum AVErrorCode {
    kErrorCodeSuccess = 0, // 成功
    kErrorCodeUnknown = -1, // 未知错误

    // 1000 段：IO 与 网络错误 (Network & IO)
    kErrorCodeIoFileNotFound = 1001, // 文件不存在
    kErrorCodeIoNetworkTimeout = 1002, // 网络连接超时
    kErrorCodeIoReadFailed = 1003, // 物理读取失败

    // 2000 段：解封装错误 (Demuxer)
    kErrorCodeDemuxerOpenFailed = 2001, // 探测格式或打开流失败
    kErrorCodeDemuxerNoStream = 2002, // 文件中找不到任何有效的音视频流
    kErrorCodeDemuxerReadEof = 2003, // 读取到文件末尾

    // 3000 段：视频解码错误 (Video Decoder)
    kErrorCodeVideoInitFailed = 3001, // 视频解码器初始化失败
    kErrorCodeVideoDecodeFailed = 3002, // 视频解码过程发生内部错误
    kErrorCodeVideoHwAccelFailed = 3003, // 硬件解码加速启动失败

    // 4000 段：音频解码错误 (Audio Decoder)
    kErrorCodeAudioInitFailed = 4001, // 音频解码器初始化失败
    kErrorCodeAudioDecodeFailed = 4002, // 音频解码过程发生内部错误
    kErrorCodeAudioResampleFailed = 4003, // 重采样器初始化或处理失败

    // 5000 段：渲染与同步错误 (Render & Sync)
    kErrorCodeRenderVideoFailed = 5001, // OpenGL 渲染失败
    kErrorCodeRenderAudioFailed = 5002, // 音频设备 (如 OpenAL/SDL) 打开失败
};

} // namespace av
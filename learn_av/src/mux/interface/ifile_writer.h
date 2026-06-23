/**
 * @file ifile_writer.h
 * @brief 录制器接口：接收原始 PCM/YUV420P 帧，内部驱动编码与封装，输出 MP4 文件
 * @note 调用方只需调用 Init/PushAudioFrame/PushVideoFrame/Stop，
 *       编码参数、Muxer 细节对上层完全透明 
 */

#pragma once

#include <memory>
#include <string>

#include "define/av_frame.h"
#include "video_frame.h"
#include "mux/interface/fileWriterParameters.h"

namespace av {
class IFileWriter {
public:

    /**
     * @brief 创建实例
     */
    static std::unique_ptr<IFileWriter> Create();

    /**
     * @brief 构造函数
     */
    IFileWriter() = default;

    /**
     * @brief 析构函数
     */
    virtual ~IFileWriter() = default;

    /**
     * @brief 初始化
     */
    virtual bool Init(const std::string& path, FileWriterParameters& paramters) = 0;

    /**
     * @brief 接收 Player 模块的音频帧并入队
     */
    virtual void PushAudioFrame(std::shared_ptr<Frame> audio_frame) = 0;

    /**
     * @brief 接收 Player 模块的视频帧并入队
     */
    virtual void PushVideoFrame(std::shared_ptr<VideoFrame> video_frame) = 0;

    /**
     * @brief 启动录制线程
     */
    virtual void Start() = 0;

    /**
     * @brief 停止录制线程
     */
    virtual void Stop() = 0;
};
} // namespace av
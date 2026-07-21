/**
 * @file file_writer.h
 * @brief
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "define/av_frame.h"
#include "encode/interface/iaudio_encoder.h"
#include "encode/interface/ivideo_encoder.h"
#include "mux/interface/fileWriterParameters.h"
#include "mux/interface/ifile_writer.h"
#include "mux/interface/imuxer.h"
#include "utils/thread_safe_queue.h"

namespace av {

class FileWriter : public IFileWriter, public IAudioEncoder::Listener, public IVideoEncoder::Listener {
public:
    /**
     * @brief 构造函数
     */
    FileWriter();

    /**
     * @brief 析构函数
     */
    ~FileWriter();

    /**
     * @brief 初始化
     */
    bool Init(const std::string& path, FileWriterParameters& paramters) override;

    /**
     * @brief 接收 Player 的音频帧，并入队
     */
    void PushAudioFrame(std::shared_ptr<Frame> audio_frame) override;

    /**
     * @brief 接收 Player 的视频帧，并入队
     */
    void PushVideoFrame(std::shared_ptr<VideoFrame> video_frame) override;

    /**
     * @brief 启动录制线程，由 Init() 内部调用
     */
    void Start() override;

    /**
     * @brief 停止线程
     */
    void Stop() override;

private:
    /**
     * @brief 接收编码后的音频
     */
    void OnAudioEncoderNotifyPacket(std::shared_ptr<Packet> audio_packet) override;

    /**
     * @brief 音频编码已经结束
     */
    void OnAudioEncoderNotifyFinished() override;

    /**
     * @brief 接收编码后的视频
     */
    void OnVideoEncoderNotifyPacket(std::shared_ptr<Packet> video_packet) override;

    /**
     * @brief 视频编码已经结束
     */
    void OnVideoEncoderNotifyFinished() override;

private:
    /**
     * @brief 录制线程
     */
    void ThreadLoop();

private:
    std::unique_ptr<IMuxer> muxer_;
    std::unique_ptr<IAudioEncoder> audio_encoder_;
    std::unique_ptr<IVideoEncoder> video_encoder_;

    ThreadSafeQueue<std::shared_ptr<Frame>> audio_queue_{30};
    ThreadSafeQueue<std::shared_ptr<VideoFrame>> video_queue_{10};


    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread encode_thread_;
    std::atomic<bool> is_stoping_{false};
    std::atomic<bool> is_initialized_{false}; // Init()成功后置true，Stop()用来防止未初始化时调Flush
};
} // namespace av
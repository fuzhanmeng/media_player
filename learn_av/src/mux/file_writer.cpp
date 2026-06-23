/**
 * @file file_writer.cpp
 * @brief
 */

#include "file_writer.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "encode/interface/iaudio_encoder.h"
#include "encode/interface/ivideo_encoder.h"
#include "logger.h"
#include "mux/interface/imuxer.h"

namespace av {

/**
 * @brief 实例创建
 */
std::unique_ptr<IFileWriter> IFileWriter::Create() {
    return std::make_unique<FileWriter>();
}

/**
 * @brief 构造函数
 */
FileWriter::FileWriter() {
    // 创建复用器
    muxer_ = std::unique_ptr<IMuxer>(IMuxer::Create());

    // 创建音频编码实例
    audio_encoder_ = std::unique_ptr<IAudioEncoder>(IAudioEncoder::Create());

    // 创建视频实例
    video_encoder_ = std::unique_ptr<IVideoEncoder>(IVideoEncoder::Create());

    // 建立数据回调路由通道
    audio_encoder_->SetListener(this);
    video_encoder_->SetListener(this);
}

/**
 * @brief 析构函数
 */
FileWriter::~FileWriter() { Stop(); }

/**
 * @brief 初始化
 */
bool FileWriter::Init(const std::string& path, FileWriterParameters& paramters) {
    // 1. 初始化音频
    if (!audio_encoder_->Init(paramters.sample_rate_, paramters.channels_, paramters.audio_bit_rate_)) {
        return false;
    }

    // 2. 初始化视频
    if (!video_encoder_->Init(paramters.width_, paramters.height_, paramters.fps_, paramters.video_bit_rate_)) {
        return false;
    }

    // 3. 初始化复用
    if (!muxer_->Init(path, audio_encoder_->GetCodecContext(), video_encoder_->GetCodecContext())) {
        return false;
    }

    Start();
    is_initialized_ = true;
    return true;
}

/**
 * @brief 接收 Player 的音频帧并入队
 * @param audio_frame 原始 PCM 音频帧，空帧被丢弃
 * @note 使用 TryPush 非阻塞入队：队列满时丢弃当前帧，绝不阻塞调用方。
 *       本函数运行在音频解码线程上，若改用阻塞 Push，录制 I/O 变慢会反压解码线程，
 *       进而耗尽声卡队列、拖停实时播放。
 */
void FileWriter::PushAudioFrame(std::shared_ptr<Frame> audio_frame) {
    if (!audio_frame) {
        LOG_ERROR("PushAudioFrame received null frame");
        return;
    }

    if (!audio_queue_.TryPush(audio_frame)) {
        LOG_WARN("record audio queue full, drop one frame");
    }

    // 先获取 mutex_ 再 notify：与 ThreadLoop 中 cv_.wait 谓词判定串行化，消除丢失唤醒
    {
        std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_one();
}

/**
 * @brief 接收 Player 的视频帧并入队
 * @param video_frame 原始 YUV420P 视频帧，空帧被丢弃
 * @note TryPush 非阻塞入队：队列满时丢帧而非阻塞渲染/解码线程
 */
void FileWriter::PushVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    if (!video_frame) {
        LOG_ERROR("PushVideoFrame received null frame");
        return;
    }

    if (!video_queue_.TryPush(video_frame)) {
        LOG_WARN("record video queue full, drop one frame");
    }

    // 先获取 mutex_ 再 notify：消除丢失唤醒（与 PushAudioFrame 同理）
    {
        std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_one();
}

/**
 * @brief 启动录制线程
 */
void FileWriter::Start() {
    is_stoping_.store(false);

    encode_thread_ = std::thread(&FileWriter::ThreadLoop, this);
}

/**
 * @brief 停止录制：通知编码线程退出，等待排空队列后 Flush 编码器并写 MP4 文件尾
 */
void FileWriter::Stop() {
    is_stoping_.store(true);
    cv_.notify_all();

    if (encode_thread_.joinable()) {
        encode_thread_.join();  // 等待编码线程排空队列后退出
    }

    // 仅在 Init() 成功后才执行 Flush，防止编码器未初始化时 crash
    if (is_initialized_) {
        audio_encoder_->Flush();   // drain 模式：输出编码器内部缓冲的剩余 AAC 包
        video_encoder_->Flush();   // drain 模式：输出编码器内部缓冲的剩余 H264 包
        muxer_->WriterTrailer();   // 写 MP4 索引表（moov），不调则文件损坏
        muxer_->Close();           // 关闭文件句柄，刷内核缓冲到磁盘
        is_initialized_ = false;
    }
}

/**
 * @brief 接收编码后的音频
 */
void FileWriter::OnAudioEncoderNotifyPacket(std::shared_ptr<Packet> audio_packet) {
    if (muxer_) {
        muxer_->WriterAudioPacket(std::move(audio_packet));
    }
}

/**
 * @brief 音频编码已经结束
 */
void FileWriter::OnAudioEncoderNotifyFinished() {}

/**
 * @brief 接收编码后的视频
 */
void FileWriter::OnVideoEncoderNotifyPacket(std::shared_ptr<Packet> video_packet) {
    if (muxer_) {
        muxer_->WriterVideoPacket(std::move(video_packet));
    }
}

/**
 * @brief 视频编码已经结束
 */
void FileWriter::OnVideoEncoderNotifyFinished() {}

/**
 * @brief 编码线程主循环：等待帧数据 → 编码 → 退出时排空队列
 */
void FileWriter::ThreadLoop() {
    while (!is_stoping_) {
        // 1. 等待唤醒：有数据入队或 Stop() 被调用时唤醒
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return is_stoping_.load() || audio_queue_.Size() > 0 || video_queue_.Size() > 0;
            });
        }

        // 2. 被 Stop() 唤醒则退出主循环，进入排空阶段
        if (is_stoping_.load()) {
            break;
        }

        // 3. 取出并编码一帧音频（TryPop：非阻塞，空则跳过）
        std::shared_ptr<Frame> audio_frame;
        if (audio_queue_.TryPop(audio_frame)) {
            audio_encoder_->Encoder(std::move(audio_frame));
        }

        // 4. 取出并编码一帧视频
        std::shared_ptr<VideoFrame> video_frame;
        if (video_queue_.TryPop(video_frame)) {
            video_encoder_->Encoder(std::move(video_frame));
        }
    }

    // 5. Stop() 触发退出后，排空队列中残留的帧，保证录制内容完整
    //    不排空则 Stop() 之前推入但未编码的帧会丢失，导致录制末尾截断
    {
        std::shared_ptr<Frame> audio_frame;
        while (audio_queue_.TryPop(audio_frame)) {
            audio_encoder_->Encoder(std::move(audio_frame));
        }
    }
    {
        std::shared_ptr<VideoFrame> video_frame;
        while (video_queue_.TryPop(video_frame)) {
            video_encoder_->Encoder(std::move(video_frame));
        }
    }
}

} // namespace av
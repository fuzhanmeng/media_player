/**
 * @file file_reader.cpp
 * @brief 负责解复用与解码模块的生命周期统筹、数据路由及状态机维护
 */
#include "file_reader.h"

#include <memory>
#include <mutex>
#include <string>

#include "logger.h"

namespace av {

std::unique_ptr<IFileReader> IFileReader::Create() {
    LOG_INFO("FileReader Create...");
    return std::make_unique<FileReader>();
}

/**
 * @brief 构造函数
 */
FileReader::FileReader() {
    // 创建解封装器
    demuxer_ = std::unique_ptr<IDemuxer>(IDemuxer::Create());

    // 音视频解码器
    audio_decoder_ = std::unique_ptr<IAudioDecoder>(IAudioDecoder::Create());
    video_decoder_ = std::unique_ptr<IVideoDecoder>(IVideoDecoder::Create());

    // 建立数据回调路由通道
    demuxer_->SetListener(this);
    audio_decoder_->SetListener(this);
    video_decoder_->SetListener(this);
}

/**
 * @brief 析构函数
 */
FileReader::~FileReader() {
    Stop();
    listener_ = nullptr;
}

//
void FileReader::SetListener(IFileReader::Listener* listener) { listener_ = listener; }

/**
 * @brief 打开媒体文件并执行流探测
 */
bool FileReader::Open(const std::string& url) {
    if (!demuxer_) {
        return false;
    }
    return demuxer_->Open(url);
}

/**
 * @brief 启动底层流水线，严格遵循消费者先于生产者启动原则
 */
void FileReader::Start(std::shared_ptr<SyncContext> sync_ctx) {
    if (audio_decoder_) {
        audio_decoder_->Start(sync_ctx);
    }

    if (video_decoder_) {
        video_decoder_->Start(sync_ctx);
    }

    if (demuxer_) {
        demuxer_->Start(sync_ctx);
    }
}

/**
 * @brief 停止所有底层线程并重置状态机
 */

void FileReader::Stop() {
    if (demuxer_) {
        demuxer_->Stop();
    }

    if (audio_decoder_) {
        audio_decoder_->Stop();
    }

    if (video_decoder_) {
        video_decoder_->Stop();
    }
}

/**
 * @brief  触发绝对时间轴跳转，保证多线程状态同步
 */

void FileReader::SeekTo(double pts_seconds) {

    if (demuxer_) {
        demuxer_->SeekTo(pts_seconds);
    }

    //  重置 EOF 状态机
    {
        std::lock_guard<std::mutex> lock(eof_mutex_);
        demuxer_eof_received_ = false;
        audio_eof_received_ = false;
        video_eof_received_ = false;
    }
}

/**
 * @brief 查询媒体总时长
 */
double FileReader::GetDuration() const { return demuxer_ ? demuxer_->GetDuration() : 0; }

/**
 * @brief 获取已缓存的视频宽度
 */
int FileReader::GetVideoWidth() const { return video_width_; }

/**
 * @brief 获取已缓存的视频高度
 */
int FileReader::GetVideoHeight() const { return video_height_; }

// ================= 解复用器数据上行回调 (Demuxer -> FileReader) =================

/**
 * @brief 接收视频流元数据，并初始化下游视频解码器
 */
void FileReader::OnNotifyVideoStream(const StreamInfo& info) {
    video_width_ = info.width_;
    video_height_ = info.height_;

    if (video_decoder_) {
        // 发送流参数至视频解码器进行初始化
        video_decoder_->Init(info);
    }
}

/**
 * @brief 接收音频流元数据，并初始化下游音频解码器
 */
void FileReader::OnNotifyAudioStream(const StreamInfo& info) {
    if (audio_decoder_) {
        // 发送流参数至音频解码器进行初始化
        audio_decoder_->Init(info);
    }
}

/**
 * @brief 接收压缩视频包，并路由至视频解码队列
 */
void FileReader::OnNotifyVideoPacket(std::shared_ptr<Packet> video_packet) {
    if (video_decoder_) {
        // 路由至视频解码管线
        video_decoder_->PushPacket(std::move(video_packet));
    }
}

/**
 * @brief 接收压缩音频包，并路由至音频解码队列
 */
void FileReader::OnNotifyAudioPacket(std::shared_ptr<Packet> audio_packet) {
    if (audio_decoder_) {
        // 路由至音频解码管线
        audio_decoder_->PushPacket(std::move(audio_packet));
    }
}

/**
 * @brief 解复用引擎读取至文件末尾，触发解码器冲刷信令
 */
void FileReader::OnNotifyDemuxerEOF() {
    std::lock_guard<std::mutex> lock(eof_mutex_);
    demuxer_eof_received_ = true; 

    // 投递终止信令至解码器，触发其内部缓冲区排空机制
    if (video_decoder_) {
        video_decoder_->SendEOF();
    }

    if (audio_decoder_) {
        audio_decoder_->SendEOF();
    }
}

// ================= 解码器数据上行回调 (Decoder -> FileReader -> Player) =================

/**
 * @brief 接收解码后的纯净 PCM 音频帧，并透传至上层总控
 */

void FileReader::OnNotifyAudioFrame(std::shared_ptr<Frame> audio_frame) {
    if (listener_) {
        // 将解码后的PCM交给FileReader模块
        listener_->OnFileReaderNotifyAudioFrame(std::move(audio_frame));
    }
}

/**
 * @brief 音频解码流彻底结束，向上层总控抛出终极信令
 */
void FileReader::OnNotifyAudioEOF() {
    std::lock_guard<std::mutex> lock(eof_mutex_);

    audio_eof_received_ = true;

    if (listener_) {
        listener_->OnFileReaderNotifyAudioFinished();
    }
}

/**
 * @brief 接收解码后的纯净 YUV 视频帧，并透传至上层总控
 */
void FileReader::OnNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    if (listener_) {
        // 将YUV图传给FileReader模块
        listener_->OnFileReaderNotifyVideoFrame(std::move(video_frame));
    }
}

/**
 * @brief 视频解码流彻底结束，向上层总控抛出终极信令
 */
void FileReader::OnNotifyVideoEOF() {
    std::lock_guard<std::mutex> lock(eof_mutex_);

    video_eof_received_ = true;

    if (listener_) {
        listener_->OnFileReaderNotifyVideoFinished();
    }
}

} // namespace av
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

std::unique_ptr<IFileReader> IFileReader::Create() { return std::make_unique<FileReader>(); }

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
    RequestStop();
    WaitForStop();
    listener_ = nullptr;
}

/**
 * @brief 设置下游回调监听
 */
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
 * @brief 启动底层流水线，消费者先于生产者启动
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
 * @brief 停止所有底层线程并重置状态机 -- 非阻塞
 */
void FileReader::RequestStop() {
    if (audio_decoder_) {
        audio_decoder_->RequestStop();
    }

    if (video_decoder_) {
        video_decoder_->RequestStop();
    }

    if (demuxer_) {
        demuxer_->RequestStop();
    }
    LOG_INFO("========== FileReader::RequestStop ==========");
}

/**
 * @brief 阶段二：同步等待回收线程 -- 阻塞
 */
void FileReader::WaitForStop() {
    if (demuxer_) {
        demuxer_->WaitForStop();
    }

    if (audio_decoder_) {
        audio_decoder_->WaitForStop();
    }

    if (video_decoder_) {
        video_decoder_->WaitForStop();
    }
    LOG_INFO("========== FileReader::WaitForStop ==========");
}

/**
 * @brief  触发绝对时间轴跳转，保证多线程状态同步
 */
void FileReader::SeekTo(int64_t target_time_us) {
    if (demuxer_) {
        demuxer_->SeekTo(target_time_us);
    }
}

/**
 * @brief 强制中断解码器内部的 Packet 队列阻塞，并清空遗留数据
 */
void FileReader::AbortAndClearDecoderQueues() {
    if (audio_decoder_) {
        audio_decoder_->AbortAndClearQueue();
    }

    if (video_decoder_) {
        video_decoder_->AbortAndClearQueue();
    }
}

/**
 * @brief 恢复解码器内部 Packet 队列的入队/出队能力
 */
void FileReader::StartDecoderQueues() {
    if (audio_decoder_) {
        audio_decoder_->StartQueue();
    }

    if (video_decoder_) {
        video_decoder_->StartQueue();
    }
}

/**
 * @brief 查询媒体总时长
 */
int64_t FileReader::GetDuration() const { return demuxer_ ? demuxer_->GetDuration() : 0; }

/**
 * @brief 获取已缓存的视频宽度
 */
int FileReader::GetVideoWidth() const { return video_width_; }

/**
 * @brief 获取已缓存的视频高度
 */
int FileReader::GetVideoHeight() const { return video_height_; }

/**
 * @brief 是否含有音频轨道
 */
bool FileReader::HasAudio() const { return demuxer_ ? demuxer_->HasAudio() : false; }

/**
 * @brief 是否含有视频轨道
 */
bool FileReader::HasVideo() const { return demuxer_ ? demuxer_->HasVideo() : false; }

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

        if (listener_) {
            listener_->OnFileReaderAudioInfo(audio_decoder_->GetSample(), audio_decoder_->GetChannels());
        }
    }
}

/**
 * @brief 接收压缩视频包，并路由至视频解码队列
 */
bool FileReader::OnNotifyVideoPacket(std::shared_ptr<Packet> video_packet) {
    if (video_decoder_) {
        // 路由至视频解码管线
        video_decoder_->PushPacket(std::move(video_packet));
        return true;
    } else {
        return false;
    }
}
/**
 * @brief 接收压缩音频包，并路由至音频解码队列
 */
bool FileReader::OnNotifyAudioPacket(std::shared_ptr<Packet> audio_packet) {
    if (audio_decoder_) {
        // 路由至音频解码管线
        audio_decoder_->PushPacket(std::move(audio_packet));
        return true;
    } else {
        return false;
    }
}

/**
 * @brief 解复用引擎读取至文件末尾，触发解码器冲刷信令
 */
void FileReader::OnNotifyDemuxerEOF() {
    // 投递终止信令至解码器，触发其内部缓冲区排空机制
    if (video_decoder_) {
        video_decoder_->SendEOF();
    }

    if (audio_decoder_) {
        audio_decoder_->SendEOF();
    }
}

/**
 * @brief 实现 IDemuxer::Listener 接口
 * @details 接收底层 Demuxer 寻址成功信号，并向顶层 Player 透传
 */
void FileReader::OnSeekResetQueues() {
    LOG_INFO("FileReader received OnSeekResetQueues from Demuxer. Forwarding to Player.");
    if (listener_) {
        listener_->OnFileReaderSeekResetQueues();
    }
}

/**
 * @brief 实现 IDemuxer::Listener 接口
 * @details 接收底层 Demuxer 状态恢复信号，并向顶层 Player 透传
 */
void FileReader::OnSeekCompleteResumePlay() {
    LOG_INFO("FileReader received OnSeekCompleteResumePlay from Demuxer. Forwarding to Player.");
    if (listener_) {
        // 调用 IFileReader::Listener 的接口，透传给 Player
        listener_->OnFileReaderSeekCompleteResumePlay();
    }
}

// ================= 解码器数据上行回调 (Decoder -> FileReader -> Player) =================

/**
 * @brief 接收解码后的纯净 PCM 音频帧，传给Player
 */
void FileReader::OnNotifyAudioFrame(std::shared_ptr<Frame> audio_frame) {
    if (listener_) {
        // 将解码后的 PCM 交给Player模块
        listener_->OnFileReaderNotifyAudioFrame(std::move(audio_frame));
    }
}

/**
 * @brief 接收解码后的纯净 YUV 视频帧，传给Player
 */
void FileReader::OnNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) {
    if (listener_) {
        // 将YUV图传给Player模块
        listener_->OnFileReaderNotifyVideoFrame(std::move(video_frame));
    }
}

} // namespace av
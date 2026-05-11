/**
 * @file player.cpp
 * @brief 播放器核心调度中枢的实现
 * @details 整个播放器架构的唯一控制中心，负责协调解封装、解码、渲染等所有底层模块
 *          所有底层模块的事件都汇聚到这里，由Player统一调度并向UI层暴露标准接口
 */
#include "player.h"

#include <atomic>
#include <utility>

#include "data/av_frame.h"
#include "data/av_packet.h"
#include "iplayer.h"
#include "logger.h"

namespace av {

// IPlayer 工厂方法实现
std::shared_ptr<av::IPlayer> IPlayer::Create(std::shared_ptr<IGLContext> shared_gl_context) {
    LOG_INFO("Create IPlayer instance");
    return std::make_shared<Player>(std::move(shared_gl_context));
}

// 构造与析构
Player::Player(std::shared_ptr<IGLContext> gl_context) : gl_context_(std::move(gl_context)) {
    LOG_INFO("Player constructor called");

    // 创建解封装器并设置监听器
    demuxer_ = IDemuxer::Create();
    demuxer_->SetListener(this);

    LOG_INFO("Player created successfully");
}

Player::~Player() {
    demuxer_->Stop();

    if (debug_pcm_file_.is_open()) {
        debug_pcm_file_.close();
    }
    LOG_INFO("Player destroyed successfully");
}

// UI相关接口实现

void Player::AttachDisplayView(void* view_handle) {
    view_handle_ = view_handle;
    LOG_INFO("Display view attached. Handle: {}", view_handle_);
}

void Player::DetachDisplayView(void* view_handle) {
    // 安全校验：仅当传入的句柄与当前绑定的句柄一致时，才执行解绑
    if (view_handle_ == view_handle) {
        view_handle_ = nullptr;
    }
    LOG_INFO("Display view detached");
}

// 播放控制接口实现

bool Player::Open(const std::string& url) {
    LOG_INFO("Open media file: {}", url);

    // 打开本地文件
    debug_pcm_file_.open("debug_output.pcm", std::ios_base::binary | std::ios_base::trunc);
    if (debug_pcm_file_.is_open()) {
        LOG_WARN("Failed to open debug_output.pcm for writing");
    }

    return demuxer_->Open(url);
}

void Player::Play() {
    LOG_INFO("Start playback");

    is_playing_.store(true, std::memory_order_release);

    // 启动解复用器
    if (demuxer_) {
        demuxer_->Start();
    }

    // 启动/恢复音频解码器
    if (audio_decoder_) {
        audio_decoder_->Start();
    }

    LOG_INFO("Playback started successfully");
}

void Player::Pause() {
    LOG_INFO("Pause playback");

    is_playing_.store(false, std::memory_order_release);

    // 暂停解复用器
    if (demuxer_) {
        demuxer_->Pause();
    }

    // 暂停音频解码器
    if (audio_decoder_) {
        audio_decoder_->Pause();
    }

    LOG_INFO("Playback paused successfully");
}

bool Player::IsPlaying() const { return is_playing_.load(std::memory_order_acquire); }

void Player::SetPlaybackListener(std::shared_ptr<IPlaybackListener> listener) {
    ui_listener_ = std::move(listener);
    LOG_INFO("Playback listener set");
}

// IDemuxerListener 接口实现,所有方法都由解复用线程调用
void Player::OnNotifyVideoStream(AVStream* stream) {
    LOG_INFO("Received video stream info. Index: {}", stream->index);
    LOG_INFO("Video decoder not implemented yet, video stream will be ignored");
}

void Player::OnNotifyAudioStream(AVStream* stream) {
    LOG_INFO("Received audio stream info. Index: {}", stream->index);

    // 创建音频解码器
    audio_decoder_ = IAudioDecoder::Create();

    // 将Player自身设置为解码器的监听器
    audio_decoder_->SetListener(this);

    // 初始化解码器
    if (audio_decoder_->Init(stream)) {
        LOG_INFO("Audio decoder initialized successfully");

        // 如果用户已经点击了播放，直接启动解码器
        if (is_playing_.load(std::memory_order_acquire)) {
            audio_decoder_->Start();
        }
    } else {
        LOG_ERROR("Failed to initialize audio decoder");
    }
}

void Player::OnNotifyVideoPacket(std::shared_ptr<Packet> packet) {
    // 调试专用：打印视频包信息
    DumpPacketInfo("Video", packet);
    LOG_TRACE("Video packet ignored, video decoder not implemented");
}

void Player::OnNotifyAudioPacket(std::shared_ptr<Packet> packet) {
    // 调试专用：打印音频包信息（上线时关闭）
    DumpPacketInfo("Audio", packet);

    // 将音频包推送到解码器队列
    if (audio_decoder_) {
        audio_decoder_->PushPacket(std::move(packet));
    }
}

void Player::OnNotifyEOF() {
    LOG_INFO("=====> Demuxer reached end of file");

    // 更新播放器状态
    is_playing_.store(false, std::memory_order_release);

    // 通知所有解码器输入结束
    if (audio_decoder_) {
        audio_decoder_->SendEOF();
    }

    // 暂停解复用器，停止读取新包
    if (demuxer_) {
        demuxer_->Pause();
    }

    LOG_INFO("Demuxer stopped successfully");
}

// IAudioDecoder::Listener 接口实现
// 所有方法都由音频解码线程调用
void Player::OnNotifyAudioFrame(std::shared_ptr<Frame> frame) {
    if (!frame)
        return;

    AVFrame* raw_frame = frame->GetRawFrame();
    if (!raw_frame)
        return;

    // A. 打印调试信息（使用静态计数器降频打印，防止刷屏导致性能卡顿）
    static int frame_count = 0;
    if (frame_count++ % 100 == 0) {
        LOG_INFO("[Audio Frame Dump] PTS: {}, Samples: {}, Format: {}", raw_frame->pts, raw_frame->nb_samples,
                 raw_frame->format); // 预期 format 应该是 1 (AV_SAMPLE_FMT_S16)
    }

    // B. 计算当前帧的数据字节大小
    // 计算公式: 样本数 * 声道数 * 每个样本的字节数(S16=2字节)
    // 使用 FFmpeg 官方 API 
    int data_size = av_samples_get_buffer_size(nullptr,
                                               raw_frame->channels, // 声道数
                                               raw_frame->nb_samples, // 单通道样本数
                                               static_cast<AVSampleFormat>(raw_frame->format), // 采样格式
                                               1 // 内存对齐 (1 表示不对齐，严格按实际大小)
    );

    // C. 写入文件
    if (debug_pcm_file_.is_open() && data_size > 0 && raw_frame->data[0] != nullptr) {
        // 直接将 data[0] 的内存块强制转换为 const char* 写入文件
        debug_pcm_file_.write(reinterpret_cast<const char*>(raw_frame->data[0]), data_size);
    }
}

void Player::OnNotifyAudioEOF() {
    LOG_INFO("=====> Audio decoder reached end of file");

    // 现在所有音频都已经解码完成，可以通知UI播放结束了
    if (ui_listener_) {
        ui_listener_->OnPlaybackCompleted();
    }

}

// 调试工具方法实现

void Player::DumpPacketInfo(const std::string& stream_type, const std::shared_ptr<Packet>& packet) {
    // 1. 检查智能指针是否为空
    if (!packet) {
        LOG_WARN("Warning: [{}] packet is null!", stream_type);
        return;
    }

    // 2. 检查是否是 Flush 标志包
    if (packet->HasFlag(av::AVFrameFlag::kFlush)) {
        LOG_INFO("=====> [{}] Received Flush packet!", stream_type);
        return;
    }

    // 3. 拿到最底层的 FFmpeg 原生包
    AVPacket* raw_pkt = packet->GetRawPacket();
    if (!raw_pkt) {
        LOG_ERROR("Warning: [{}] raw AVPacket is null!", stream_type);
        return;
    }

    // 4. 打印核心数据
    bool is_keyframe = (raw_pkt->flags & AV_PKT_FLAG_KEY);

    LOG_TRACE("[{} Packet] Size: {:>6} bytes | PTS: {:>6} | DTS: {:>6} | KeyFrame: {}", stream_type, raw_pkt->size,
              raw_pkt->pts, raw_pkt->dts, is_keyframe ? "Yes" : "No");
}

} // namespace av
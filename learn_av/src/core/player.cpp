/**
 * @file player.cpp
 * @brief 播放器核心调度中枢的实现
 */
#include "player.h"

#include <atomic>

#include "data/av_packet.h"
#include "iplayer.h"
#include "logger.h"

namespace av {

std::shared_ptr<av::IPlayer> IPlayer::Create(std::shared_ptr<IGLContext> shared_gl_context) {
    LOG_INFO("Create IPlayer");
    return std::make_shared<Player>(std::move(shared_gl_context));
}

Player::Player(std::shared_ptr<IGLContext> gl_context) : gl_context_(std::move(gl_context)) {
    LOG_INFO("Created Player successfully.");

    // 将 Player 自身注册为 Demuxer 的数据监听器
    demuxer_.SetListener(this);
}

Player::~Player() {
    demuxer_.Stop();
    LOG_INFO("Destroyed.");
}

void Player::AttachDisplayView(void* view_handle) {
    view_handle_ = view_handle;
    LOG_INFO("Display view attached. Handle: {}", view_handle_);
}

void Player::DetachDisplayView(void* view_handle) {
    // 安全校验：仅当传入的句柄与当前绑定的句柄一致时，才执行解绑
    if (view_handle_ == view_handle) {
        view_handle_ = nullptr;
    }
    LOG_INFO("Display view detached.");
}

bool Player::Open(const std::string& url) {
    LOG_INFO("Try to open URL: {}", url);
    return demuxer_.Open(url);
}

void Player::Play() {
    is_playing_ = true;
    demuxer_.Start();
    LOG_INFO("Playback started.");
}

void Player::Pause() {
    is_playing_ = false;
    demuxer_.Pause();
    LOG_INFO("Playback paused.");
}

bool Player::IsPlaying() const { return is_playing_; }

void Player::SetPlaybackListener(std::shared_ptr<IPlaybackListener> listener) { ui_listener_ = listener; }

void Player::OnNotifyAudioPacket(std::shared_ptr<IAVPacket> packet) {
    DumpPacketInfo("Audio", packet);
    // 这里非常频繁，如果不需要深究，建议用 LOG_TRACE，或者注释掉，避免刷屏
    LOG_TRACE("Received Audio packet. Decoder missing.");
}

void Player::OnNotifyVideoPacket(std::shared_ptr<IAVPacket> packet) {
    DumpPacketInfo("Video", packet);
    // 同理，高频日志用 TRACE 级别
    LOG_TRACE("Received Video packet. Decoder missing.");
}

void Player::OnNotifyAudioStream(AVStream* stream) { LOG_INFO("Received Audio stream info. Decoder missing."); }

void Player::OnNotifyVideoStream(AVStream* stream) { LOG_INFO("Received Video stream info. Decoder missing."); }

void Player::DumpPacketInfo(const std::string& stream_type, const std::shared_ptr<IAVPacket>& packet) {
    // 1. 检查智能指针是否为空
    if (!packet) {
        LOG_WARN("Warning: [{}] packet is null!", stream_type);
        return;
    }

    // 2. 检查是否是 Flush 标志包
    if (packet->flags_ & AVFrameFlag::kFlush) {
        LOG_INFO("=====> [{}] Received Flush packet!", stream_type);
        return;
    }

    // 3. 拿到最底层的 FFmpeg 原生包
    AVPacket* raw_pkt = packet->packet_;
    if (!raw_pkt) {
        LOG_ERROR("Warning: [{}] raw AVPacket is null!", stream_type);
        return;
    }

    // 4. 打印核心数据
    bool is_keyframe = (raw_pkt->flags & AV_PKT_FLAG_KEY);

    // 【魔法格式化】：直接抛弃 std::setw 和冗长的 <<
    LOG_TRACE("[{} Packet] Size: {:>6} bytes | PTS: {:>6} | DTS: {:>6} | KeyFrame: {}", stream_type, raw_pkt->size,
              raw_pkt->pts, raw_pkt->dts, is_keyframe ? "Yes" : "No");
}

} // namespace av
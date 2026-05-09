/**
 * @file player.cpp
 * @brief 播放器核心调度中枢的实现
 */
#include "player.h"

#include <atomic>
#include <iomanip>
#include <iostream>

#include "data/av_packet.h"
#include "iplayer.h"

namespace av {

std::shared_ptr<av::IPlayer> IPlayer::Create(std::shared_ptr<IGLContext> shared_gl_context) {
    return std::make_shared<Player>(std::move(shared_gl_context));
}

Player::Player(std::shared_ptr<IGLContext> gl_context) : gl_context_(std::move(gl_context)) {
    std::cout << "[Player] Created successfully." << std::endl;

    // 将 Player 自身注册为 Demuxer 的数据监听器
    demuxer_.SetListener(this);
}

Player::~Player() {
    demuxer_.Stop();
    std::cout << "[Player] Destroyed." << std::endl;
}

void Player::AttachDisplayView(void* view_handle) {
    view_handle_ = view_handle;
    std::cout << "[Player] Display view attached." << std::endl;
}

void Player::DetachDisplayView(void* view_handle) {
    // 安全校验：仅当传入的句柄与当前绑定的句柄一致时，才执行解绑
    if (view_handle_ == view_handle) {
        view_handle_ = nullptr;
    }
    std::cout << "[Player] Display view detached." << std::endl;
}

bool Player::Open(const std::string& url) {
    // 调用解复用器打开媒体源
    return demuxer_.Open(url);
}

void Player::Play() {
    is_playing_ = true;
    demuxer_.Start();
    std::cout << "[Player] Playback started." << std::endl;
}

void Player::Pause() {
    is_playing_ = false;
    demuxer_.Pause();
    std::cout << "[Player] Playback paused." << std::endl;
}

bool Player::IsPlaying() const { return is_playing_; }

void Player::SetPlaybackListener(std::shared_ptr<IPlaybackListener> listener) { ui_listener_ = listener; }

void Player::OnNotifyAudioPacket(std::shared_ptr<IAVPacket> packet) {
    DumpPacketInfo("Audio", packet);
    std::cout << "[Player] Received Audio packet. Decoder missing." << std::endl;
}

void Player::OnNotifyVideoPacket(std::shared_ptr<IAVPacket> packet) {
    DumpPacketInfo("Video", packet);
    std::cout << "[Player] Received Video packet. Decoder missing." << std::endl;
}

void Player::OnNotifyAudioStream(AVStream* stream) {
    std::cout << "[Player] Received Audio stream info. Decoder missing." << std::endl;
}

void Player::OnNotifyVideoStream(AVStream* stream) {
    std::cout << "[Player] Received Video stream info. Decoder missing." << std::endl;
}

void Player::DumpPacketInfo(const std::string& stream_type, const std::shared_ptr<IAVPacket>& packet) {
    // 1. 检查智能指针是否为空
    if (!packet) {
        std::cout << "[Player] Warning: [" << stream_type << "] packet is null!" << std::endl;
        return;
    }

    // 2. 检查是否是 Flush 标志包 (Seek 操作时用于清洗管线的指令包)
    if (packet->flags_ & AVFrameFlag::kFlush) {
        std::cout << "[Player] =====> [" << stream_type << "] Received Flush packet!" << std::endl;
        return;
    }

    // 3. 拿到最底层的 FFmpeg 原生包
    AVPacket* raw_pkt = packet->packet_;
    if (!raw_pkt) {
        std::cout << "[Player] Warning: [" << stream_type << "] raw AVPacket is null!" << std::endl;
        return;
    }

    // 4. 打印核心数据
    // raw_pkt->pts: 显示时间戳
    // raw_pkt->dts: 解码时间戳
    // raw_pkt->size: 包的大小（字节）
    // raw_pkt->flags & AV_PKT_FLAG_KEY: 是否是关键帧 (I帧)
    bool is_keyframe = (raw_pkt->flags & AV_PKT_FLAG_KEY);

    std::cout << "[Player] [" << stream_type << " Packet] " << "Size: " << std::setw(6) << raw_pkt->size << " bytes | "
              << "PTS: " << std::setw(6) << raw_pkt->pts << " | " << "DTS: " << std::setw(6) << raw_pkt->dts << " | "
              << "KeyFrame: " << (is_keyframe ? "Yes" : "No") << std::endl;
}

} // namespace av
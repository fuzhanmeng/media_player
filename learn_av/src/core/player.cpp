/**
 * @file player.cpp
 * @brief 真正的播放器中枢实现类
 */
#include "player.h"
#include "../include/iplayer.h"

#include <atomic>
#include <iomanip>
#include <iostream>

#include "../data/av_packet.h"

namespace av {

std::shared_ptr<av::IPlayer> IPlayer::Create(std::shared_ptr<IGLContext> shared_gl_context) {
    return std::make_shared<Player>(std::move(shared_gl_context));
}

Player::Player(std::shared_ptr<IGLContext> gl_context) : gl_context_(std::move(gl_context)) {
    std::cout << "create Player success" << std::endl;

    // 让 Player 成为 Demuxer 的合法收件人
    demuxer_.SetListener(this);
}

Player::~Player() { demuxer_.Stop(); }

void Player::AttachDisplayView(void* view_handle) {
    view_handle_ = view_handle;
    std::cout << "AttachDisplayView" << std::endl;
}

void Player::DetachDisplayView(void* view_handle) {
    // 安全校验：如果要拔的确实是当前插着的这根线，才把它置空
    if (view_handle_ == view_handle) {
        view_handle_ = nullptr;
    }
    std::cout << "DetachDisplayView" << std::endl;
}

bool Player::Open(const std::string& url) {
    // // 1. 停止之前的动作
    // demuxer_.Stop();

    // 2. 调用解复用器的open
    return demuxer_.Open(url);
}

void Player::Play() {
    // 点击播放
    is_playing_ = true;

    // 开启解复用
    demuxer_.Start();
}

void Player::Pause() {
    is_playing_ = false;
    demuxer_.Pause();
}

bool Player::IsPlaying() const { return is_playing_; }

void Player::SetPlaybackListener(std::shared_ptr<IPlaybackListener> listener) { ui_listener_ = listener; }

void Player::OnNotifyAudioPacket(std::shared_ptr<IAVPacket> packet) {
    DumpPacketInfo("Audio", packet);
    std::cout << "接到了来自解复用器的Audio packet,需要传给解码器，解码器现在还没有创建" << std::endl;
}

void Player::OnNotifyVideoPacket(std::shared_ptr<IAVPacket> packet) {
    DumpPacketInfo("Video", packet);
    std::cout << "接到了来自解复用器的Video packet,需要传给解码器，解码器现在还没有创建" << std::endl;
}

void Player::OnNotifyAudioStream(AVStream* stream) {
    std::cout << "接到了来自解复用器的 audio stream info,需要传给解码器，解码器现在还没有创建" << std::endl;
}

void Player::OnNotifyVideoStream(AVStream* stream) {
    std::cout << "接到了来自解复用器的 video stream info,需要传给解码器，解码器现在还没有创建" << std::endl;
}

void Player::DumpPacketInfo(const std::string& stream_type, const std::shared_ptr<IAVPacket>& packet) {
    // 1. 检查智能指针是否为空
    if (!packet) {
        std::cout << "[" << stream_type << "] 收到空指针!" << std::endl;
        return;
    }

    // 2. 检查是否是 Flush 标志包 (Seek 操作时发送的空炸弹)
    if (packet->flags_ & AVFrameFlag::kFlush) {
        std::cout << "=====> [" << stream_type << "] 收到 Flush 清理包！" << std::endl;
        return;
    }

    // 3. 拿到最底层的 FFmpeg 原生包
    AVPacket* raw_pkt = packet->packet_;
    if (!raw_pkt) {
        std::cout << "[" << stream_type << "] 原生 AVPacket 为空!" << std::endl;
        return;
    }

    // 4. 打印核心数据
    // raw_pkt->pts: 显示时间戳
    // raw_pkt->dts: 解码时间戳
    // raw_pkt->size: 包的大小（字节）
    // raw_pkt->flags & AV_PKT_FLAG_KEY: 是否是关键帧 (I帧)
    bool is_keyframe = (raw_pkt->flags & AV_PKT_FLAG_KEY);

    std::cout << "[" << stream_type << " Packet] " << "Size: " << std::setw(6) << raw_pkt->size << " bytes | "
              << "PTS: " << std::setw(6) << raw_pkt->pts << " | " << "DTS: " << std::setw(6) << raw_pkt->dts << " | "
              << "KeyFrame: " << (is_keyframe ? "Yes" : "No") << std::endl;
}
} // namespace av

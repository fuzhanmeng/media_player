/**
 * @file player.h
 * @brief 真正的播放器中枢实现类
 */
#pragma once

#include <atomic>

#include "core/demuxer.h"
#include "interface/idemuxer_listener.h"
#include "iplayer.h"

namespace av {

class Player : public IPlayer, public IDemuxerListener {
public:
    explicit Player(std::shared_ptr<IGLContext> gl_context);
    ~Player() override;

    // UI
    void AttachDisplayView(void* view_handle) override;
    void DetachDisplayView(void* view_handle) override;

    bool Open(const std::string& ulr) override;
    void Play() override;
    void Pause() override;
    bool IsPlaying() const override;
    void SetPlaybackListener(std::shared_ptr<IPlaybackListener> listener) override;

    // IDemuxerListener
    void OnNotifyVideoStream(AVStream* stream) override;
    void OnNotifyAudioStream(AVStream* stream) override;
    void OnNotifyVideoPacket(std::shared_ptr<IAVPacket> packet) override;
    void OnNotifyAudioPacket(std::shared_ptr<IAVPacket> packet) override;

    /**
     * @brief 调试专用：打印数据包的详细信息
     * @param stream_type 标明是 "Video" 还是 "Audio"
     * @param packet 要打印的数据包
     */
    void DumpPacketInfo(const std::string& stream_type, const std::shared_ptr<IAVPacket>& packet);

private:
    std::shared_ptr<IGLContext> gl_context_{nullptr};
    std::atomic<bool> is_playing_{false};
    std::shared_ptr<IPlaybackListener> ui_listener_{nullptr};
    void* view_handle_{nullptr};
    Demuxer demuxer_;
};
} // namespace av
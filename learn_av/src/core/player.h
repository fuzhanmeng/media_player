/**
 * @file player.h
 * @brief 播放器核心调度中枢实现类
 * @details 整个播放器架构的唯一控制中心，负责协调解封装、解码、渲染等所有底层模块
 *          所有底层模块的事件都汇聚到这里，由Player统一调度并向UI层暴露标准接口
 *          所有公共方法都是线程安全的，可以在任意线程中调用
 */
#pragma once

#include <atomic>
#include <fstream> // 新增头文件

#include "interface/iaudio_decoder.h"
#include "interface/idemuxer.h"
#include "interface/idemuxer_listener.h"
#include "iplayer.h"

namespace av {

class Frame;
class Packet;

/**
 * @class Player
 * @brief 播放器核心实现类
 * @inherit IPlayer 面向UI层的播放器标准接口
 * @inherit IDemuxerListener 解封装器事件监听器，接收解封装器的所有事件
 * @inherit IAudioDecoder::Listener 音频解码器事件监听器，接收音频解码器的所有事件
 */
class Player : public IPlayer, public IDemuxerListener, public IAudioDecoder::Listener {
public:
    explicit Player(std::shared_ptr<IGLContext> gl_context);

    ~Player() override;

    // UI相关接口

    /**
     * @brief 绑定显示视图
     * @param view_handle 平台相关的视图句柄
     * @note 必须在主线程中调用
     */
    void AttachDisplayView(void* view_handle) override;

    /**
     * @brief 解绑显示视图
     * @param view_handle 要解绑的视图句柄
     * @note 只有传入的句柄与当前绑定的句柄一致时才会执行解绑
     */
    void DetachDisplayView(void* view_handle) override;

    // 播放控制接口

    /**
     * @brief 打开媒体文件
     * @param url 媒体文件路径或网络流地址
     * @return true 打开成功；false 打开失败
     * @note 该方法是同步阻塞的，会执行文件IO和流解析操作
     */
    bool Open(const std::string& url) override;

    /**
     * @brief 开始或恢复播放
     * @note 如果已经在播放状态，调用此方法无效果
     */
    void Play() override;

    /**
     * @brief 暂停播放
     * @note 如果已经在暂停状态，调用此方法无效果
     */
    void Pause() override;

    /**
     * @brief 查询当前是否正在播放
     * @return true 正在播放；false 已暂停或已停止
     */
    bool IsPlaying() const override;

    /**
     * @brief 设置播放事件监听器
     * @param listener 监听器指针，接收播放状态变化事件
     * @note 所有监听器回调都保证在主线程中调用
     */
    void SetPlaybackListener(std::shared_ptr<IPlaybackListener> listener) override;

    // IDemuxerListener 接口实现, 所有方法都由解复用线程调用
    /**
     * @brief 通知发现视频流
     * @param stream FFmpeg视频流对象
     * @note 由解复用线程调用
     */
    void OnNotifyVideoStream(AVStream* stream) override;

    /**
     * @brief 通知发现音频流
     * @param stream FFmpeg音频流对象
     * @note 由解复用线程调用
     */
    void OnNotifyAudioStream(AVStream* stream) override;

    /**
     * @brief 通知收到视频数据包
     * @param packet 视频数据包
     * @note 由解复用线程调用
     */
    void OnNotifyVideoPacket(std::shared_ptr<Packet> packet) override;

    /**
     * @brief 通知收到音频数据包
     * @param packet 音频数据包
     * @note 由解复用线程调用
     */
    void OnNotifyAudioPacket(std::shared_ptr<Packet> packet) override;

    /**
     * @brief 通知解封装器到达文件末尾
     * @details 表示解封装器已经读完了所有数据包
     * @note 由解复用线程调用
     */
    void OnNotifyEOF() override;

    // IAudioDecoder::Listener 接口实现,所有方法都由音频解码线程调用
    /**
     * @brief 通知收到解码后的音频帧
     * @param frame 解码后的原始音频帧
     * @note 由音频解码线程调用
     */
    void OnNotifyAudioFrame(std::shared_ptr<Frame> frame) override;

    /**
     * @brief 通知音频解码器到达末尾
     * @details 表示音频解码器已经输出了所有剩余帧
     * @note 由音频解码线程调用
     */
    void OnNotifyAudioEOF() override;

    // 调试工具方法
    /**
     * @brief 调试专用：打印数据包的详细信息
     * @param stream_type 标明是 "Video" 还是 "Audio"
     * @param packet 要打印的数据包
     */
    void DumpPacketInfo(const std::string& stream_type, const std::shared_ptr<Packet>& packet);

private:
    // 渲染相关
    /// 共享的OpenGL上下文，用于视频渲染
    std::shared_ptr<IGLContext> gl_context_{nullptr};
    /// 平台相关的显示视图句柄
    void* view_handle_{nullptr};

    // 状态与监听器
    std::atomic<bool> is_playing_{false};
    /// 面向UI层的播放事件监听器
    std::shared_ptr<IPlaybackListener> ui_listener_{nullptr};

    // 底层模块
    /// 解封装器实例，负责读取媒体文件并拆解为音视频数据包
    std::unique_ptr<IDemuxer> demuxer_{nullptr};
    /// 音频解码器实例，负责解码音频数据包为原始音频帧
    std::unique_ptr<IAudioDecoder> audio_decoder_{nullptr};

    std::ofstream debug_pcm_file_;
};

} // namespace av
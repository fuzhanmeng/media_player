/**
 * @file file_reader.h
 * @brief 媒体数据源外观实现类 (Facade)
 * @details 作为纯粹的数据路由器和管线生命周期管理者，自身不维护播放状态，完全服从全局状态机调度
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "interface/ifile_reader.h"
#include "interface/iaudio_decoder.h"
#include "interface/idemuxer.h"
#include "interface/ivideo_decoder.h"

namespace av {
struct SyncContext;

class FileReader : public IFileReader,
                   public IDemuxer::Listener,
                   public IAudioDecoder::Listener,
                   public IVideoDecoder::Listener {
public:
    FileReader();
    ~FileReader() override;

    /**
     * @brief 绑定上层总控监听器 (即 Player)
     */
    void SetListener(IFileReader::Listener* listener) override;

    /**
     * @brief 打开媒体文件并探测流信息
     * @param url 文件路径或网络流地址
     * @return true 探测并初始化成功；false 探测失败
     * @note 仅建立上下文与参数探测，不启动任何活动线程
     */
    bool Open(const std::string& url) override;

    /**
     * @brief 物理启动所有底层工作线程
     * @details 仅在线程首次创建（或 Stop 后重新 Open）时调用。
     * 线程启动后，将自主监听全局 PlaybackState 进行运转或休眠。
     */
    void Start(std::shared_ptr<SyncContext> sync_ctx) override;


    /**
     * @brief 物理销毁底层所有工作线程并释放资源
     * @details 执行标准的拆除流程：发送退出信号 -> 唤醒所有死锁线程 -> Join 线程 -> 释放上下文
     */
    void Stop() override;

    /**
     * @brief 触发绝对时间轴跳转
     * @param pts_seconds 目标绝对时间（单位：秒）
     * @note 内部自动协调 Demuxer 的文件指针偏移与 Decoder 的脏数据冲刷 (Flush)
     */
    void SeekTo(double pts_seconds) override;

    /**
     * @brief 查询媒体总时长
     */
    double GetDuration() const override;

    /**
     * @brief 获取图像物理宽度
     */
    int GetVideoWidth() const override;

    /**
     * @brief 获取图像物理高度
     */
    int GetVideoHeight() const override;

private:
    // ================= IDemuxer::Listener =================
    /**
     * @brief 响应解封装器：发现视频流元数据
     */
    void OnNotifyVideoStream(const StreamInfo& info) override;

    /**
     * @brief 响应解封装器：发现音频流元数据
     */
    void OnNotifyAudioStream(const StreamInfo& info) override;

    /**
     * @brief 响应解封装器：收到压缩视频包，路由至视频解码管线
     */
    void OnNotifyVideoPacket(std::shared_ptr<Packet> packet) override;

    /**
     * @brief 响应解封装器：收到压缩音频包，路由至音频解码管线
     */
    void OnNotifyAudioPacket(std::shared_ptr<Packet> packet) override;

    /**
     * @brief 响应解封装器：文件物理读取完毕 (EOF)
     */
    void OnNotifyDemuxerEOF() override;

    // ================= IAudioDecoder::Listener =================
    /**
     * @brief 响应音频解码器：输出纯净 PCM 帧，向上透传给 Player
     */
    void OnNotifyAudioFrame(std::shared_ptr<Frame> frame) override;

    /**
     * @brief 响应音频解码器：音频缓存全部排空 (Audio EOF)
     */
    void OnNotifyAudioEOF() override;

    // ================= IVideoDecoder::Listener  =================
    /**
     * @brief 响应视频解码器：输出纯净 YUV/RGB 帧，向上透传给 Player
     */
    void OnNotifyVideoFrame(std::shared_ptr<av::VideoFrame> frame) override;

    /**
     * @brief 响应视频解码器：视频缓存全部排空 (Video EOF)
     */
    void OnNotifyVideoEOF() override;

private:
    // 向上层抛数据的通讯管道
    IFileReader::Listener* listener_{nullptr};

    // 解复用器
    std::unique_ptr<IDemuxer> demuxer_{nullptr};

    // 音频解码 / 视频解码
    std::unique_ptr<IAudioDecoder> audio_decoder_{nullptr};
    std::unique_ptr<IVideoDecoder> video_decoder_{nullptr};

    // 媒体元数据缓存
    int video_width_{0};
    int video_height_{0};
    double duration_{0.0};

    // EOF 跟踪状态机：精确控制什么时候才算真正播放完毕
    std::mutex eof_mutex_;
    bool demuxer_eof_received_{false};
    bool audio_eof_received_{false};
    bool video_eof_received_{false};
};

} // namespace av
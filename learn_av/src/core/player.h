/**
 * @file player.h
 * @brief 播放器核心调度中枢实现类
 * @details 负责统筹底层数据路由、管线调度，并维护全局有限状态机 (FSM)
 */
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "interface/iav_synchronizer.h"
#include "interface/ifile_reader.h"
#include "iplayer.h"
#include "output/sdl_audio_speaker.h"
#include "playback_state.h"

namespace av {

// 对象前置声明
class Frame;
class VideoFrame;

/**
 * @class Player
 * @brief 播放器核心实现类
 * @inherit IPlayer 面向 UI 层的标准播放控制契约
 * @inherit IFileReader::Listener 底层音视频纯净帧数据的唯一合法接收通道
 */
class Player : public IPlayer, public IFileReader::Listener, public IAVSynchronizer::Listener {
public:
    explicit Player(std::shared_ptr<IGLContext> gl_context);
    ~Player() override;

    // ================= 渲染管道控制 =================

    /**
     * @brief 绑定视频渲染槽
     * @param sink 实现了 IVideoSink 接口的视频帧接收器
     */
    void AttachDisplayView(IVideoSink* sink) override;

    /**
     * @brief 解绑视频渲染槽
     * @param sink 要解绑的视频帧接收器
     */
    void DetachDisplayView(IVideoSink* sink) override;

    // ================= 播放状态机控制指令 =================

    /**
     * @brief 打开媒体流并建立底层数据路由
     * @param url 媒体文件路径或网络流地址
     * @return true 探测成功；false 打开失败
     * @note 该方法会触发底层 Demuxer 的同步阻塞探测逻辑
     */
    bool Open(const std::string& url) override;

    /**
     * @brief 触发播放状态机流转 (kIdle/kPaused -> kPlaying)
     */
    void Play() override;

    /**
     * @brief 触发播放状态机流转 (kPlaying -> kPaused)，挂起全链路线程
     */
    void Pause() override;

    /**
     * @brief 终止播放状态机，安全释放底层的解码与解复用引擎
     */
    void Stop() override;

    /**
     * @brief 触发媒体物理时间轴跳转 (Seek)
     * @param target_time_sec 目标绝对时间（单位：秒）
     * @note Pause -> Flush -> Seek -> 状态恢复 的多线程安全时序
     */
    void SeekTo(double target_time_sec) override;

    // ================= 状态查询与事件订阅 =================

    /**
     * @brief 获取当前媒体全局总时长
     * @return 总时长（单位：秒）。若是直播流或未就绪，返回 0.0
     */
    double GetDuration() const override;

    /**
     * @brief 获取图像物理宽度
     * @note 仅在 Open 成功解析流信息后有效
     */
    int GetVideoWidth() const override;

    /**
     * @brief 获取图像物理高度
     * @note 仅在 Open 成功解析流信息后有效
     */
    int GetVideoHeight() const override;

    /**
     * @brief 查询当前播放器状态机的原子状态
     * @return 当前所处的 PlaybackState 枚举值
     */
    PlaybackState GetState() const override;

    void SetListener(IPlayer::Listener* listener) override;

private:
    // ================= IFileReader::Listener 数据上行接口实现 =================

    /**
     * @brief 接收底层解码输出的纯净 PCM 音频帧
     * @param audio_frame 包含采样率、声道数等物理参数的音频数据 DTO
     */
    void OnFileReaderNotifyAudioFrame(std::shared_ptr<Frame> audio_frame) override;

    /**
     * @brief 接收底层解码输出的纯净 YUV/RGB 视频帧
     * @param video_frame 包含分辨率、色彩空间等物理参数的视频数据 DTO
     */
    void OnFileReaderNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) override;

    /**
     * @brief 将接收的YUV420P传给音画同步模块
     */
    void OnSyncNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) override;

    /**
     * @brief 音频物理管线彻底枯竭 (Audio EOF)
     * @details 底层音频解码器内部残留帧已全部排空
     */
    void OnFileReaderNotifyAudioFinished() override;

    /**
     * @brief 视频物理管线彻底枯竭 (Video EOF)
     * @details 底层视频解码器内部残留帧已全部排空
     */
    void OnFileReaderNotifyVideoFinished() override;

    /**
     * @brief 通知上层重置所有相关数据队列的状态标志
     */
    void OnFileReaderSeekResetQueues() override;

    /**
     * @brief 通知上层底层 Seek 操作已完成，申请恢复播放状态
     */
    void OnFileReaderSeekCompleteResumePlay() override;

    // ================= 内部控制逻辑 =================

    /**
     * @brief 内部状态机跃迁辅助函数
     * @param new_state 目标原子状态
     * @details 负责执行 CAS 状态锁更新，并异步派发事件至 UI 层
     */
    void ChangeState(PlaybackState new_state);

private:
    // ================= 核心基础设施 =================

    /// 面向 UI 层的事件监听器 (采用裸指针，严格防止循环依赖)
    IPlayer::Listener* listener_{nullptr};

    /// 硬件 OpenGL 上下文句柄 (用于后续初始化 VideoPipeline 渲染管线)
    std::shared_ptr<IGLContext> gl_context_{nullptr};

    /// 底层数据源路由总管
    std::unique_ptr<IFileReader> file_reader_{nullptr};

    // ================= 视频渲染分发 =================
    // 保存注册上来的 OpenGLView 等接收端
    std::vector<IVideoSink*> video_sinks_;

    // 保护 video_sinks_ 读写的互斥锁，防止 UI 销毁组件时底层还在刷帧
    std::mutex sinks_mutex_;

    // ================= 多线程状态同步原语 (The Source of Truth) =================

    // 全局同步上下文的持久化句柄
    std::shared_ptr<SyncContext> sync_ctx_;

    // ================= 调试工具 =================

    /// 音频播放实例
    std::unique_ptr<IAudioSpeaker> audio_speaker_{nullptr};

    /// 音画同步实例
    std::unique_ptr<IAVSynchronizer> av_sync_{nullptr};
};

} // namespace av
/**
 * @file iav_synchronizer.h
 * @brief 音画同步模块接口定义
 * @details 以音频物理时钟为基准，通过精准休眠或丢帧策略,实现视频画面的 A/V Sync
 */
#pragma once

#include <memory>

namespace av {
class VideoFrame;
class IAudioSpeaker;
struct SyncContext;

/**
 * @class IAVSynchronizer
 * @brief 音画同步器抽象接口类
 */
class IAVSynchronizer {
public:
    /**
     * @class Listener
     * @brief 同步器事件回调接口
     * @details 下游渲染模块（Player）需实现此接口以接收同步好的视频帧
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * @brief 接收已完成时间轴对齐、可立即提交 GPU 渲染的视频帧
         * @param video_frame 视频帧数据 DTO
         */
        virtual void OnSyncNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) = 0;
    
        /**
         * @brief 通知Player收到了EOF包
         */
        virtual void OnSyncCompleted() = 0;
    };

    /**
     * @brief 静态工厂方法，实例化具体的同步器
     * @param sync_ctx 播放器全局状态机上下文
     * @param audio_speaker 指向音频物理播放器的指针，用于索取主时钟
     * @return 独占智能指针包裹的同步器实例
     */
    static std::unique_ptr<IAVSynchronizer> Create(std::shared_ptr<SyncContext> sync_ctx, IAudioSpeaker* audio_speaker);

    virtual ~IAVSynchronizer() = default;

    /**
     * @brief 注册下游监听器
     * @param listener 实现了 Listener 接口的接收方指针
     */
    virtual void SetListener(Listener* listener) = 0;

    /**
     * @brief 启动同步器内部的工作线程
     */
    virtual void Start() = 0;

    /**
     * @brief 阶段一：下发异步终止信令 -- 非阻塞
     */
    virtual void RequestStop() = 0;

    /**
     * @brief 阶段二：同步等待回收线程 -- 阻塞
     */
    virtual void WaitForStop() = 0;

    /**
     * @brief 刷新跳转后的队列
     */
    virtual void Flush() = 0;

    /**
     * @brief 清空队列，唤醒阻塞
     */
    virtual void AbortAndClearQueue() = 0;

    /**
     * @brief 启动队列
     */
    virtual void StartQueue() = 0;

    /**
     * @brief 接收来自上游解码器的原始视频帧
     * @param video_frame 刚解码出的视频帧数据
     * @details 该方法将帧压入同步器内部队列，非阻塞立即返回
     */
    virtual void PushVideoFrame(std::shared_ptr<VideoFrame> video_frame) = 0;
};

} // namespace av
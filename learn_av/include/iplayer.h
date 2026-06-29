/**
 * @file iplayer.h
 * @brief 播放器控制接口，采用纯虚函数设计，隔离具体实现
 */
#pragma once

#include <memory>
#include <string>

#include "igl_context.h"
#include "playback_state.h"

namespace av {

// 前置声明，解耦 UI 渲染层
class IVideoSink;

class IPlayer {
public:
    /**
     * @brief 播放状态事件监听器接口
     * @details 所有需要接收播放器状态变化的 UI 类都需要实现这个接口
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * @brief 状态流转通知
         * @details 彻底替代繁琐的 Started/Paused/Resumed/Stopped 回调,当底层 PlaybackState 发生原子跃迁时，触发此回调
         * @param state 跃迁后的最新状态
         */
        virtual void OnPlaybackStateChanged(PlaybackState state) = 0;

        /**
         * @brief 播放流结束
         * @details 当媒体文件读取完毕，且所有帧渲染出列时触发
         */
        virtual void OnPlaybackCompleted() = 0;

        /**
         * @brief 播放错误
         * @param error_code 错误码
         * @param error_msg 错误信息
         */
        virtual void OnPlaybackError(int error_code, const std::string& error_msg) = 0;

        /**
         * @brief 播放进度更新
         * @param progress      当前播放进度，绝对比例范围 [0.0f, 1.0f]
         * @param current_us    当前播放的绝对物理时间（单位：微秒）
         * @param total_us      媒体全局总时长（单位：微秒）
         */
        virtual void OnPlaybackProgress(float progress, int64_t current_us, int64_t total_us) = 0;
    };

public:
    IPlayer() = default;
    virtual ~IPlayer() = default;

    /**
     * @brief 播放器工厂方法：创建一个播放器实例
     * @param shared_gl_context 共享的 OpenGL 上下文句柄
     */
    static std::shared_ptr<av::IPlayer> Create(std::shared_ptr<IGLContext> shared_gl_context);

    /**
     * @brief 绑定视频渲染槽，将底层的视频帧输出推送到指定的接收端上
     * @param sink 实现了 IVideoSink 接口的视频帧接收器（UI 层的 OpenGLView）
     */
    virtual void AttachDisplayView(IVideoSink* sink) = 0;

    /**
     * @brief 解绑视频渲染槽，停止向该接收端OpenGLView推送视频帧，防止野指针崩溃
     * @param sink 之前绑定的视频帧接收器
     */
    virtual void DetachDisplayView(IVideoSink* sink) = 0;

    /**
     * @brief 打开媒体文件
     * @param url 文件路径或流媒体网络地址
     * @return true 探测成功，false 打开失败
     */
    virtual bool Open(const std::string& url) = 0;

    /**
     * @brief 播放请求
     * @details 内部将触发状态机向 PlaybackState::kPlaying 跃迁
     */
    virtual void Play() = 0;

    /**
     * @brief 挂起播放请求
     * @details 内部将触发状态机向 PlaybackState::kPaused 跃迁
     */
    virtual void Pause() = 0;

    /**
     * @brief 停止播放并彻底释放所有底层资源
     * @details 内部将触发状态机向 PlaybackState::kStopped 跃迁
     * 调用此方法后，播放器将执行以下原子拆除流程：
     * 1. 发送退出信号，唤醒并安全销毁（Join）解封装与解码等所有工作线程
     * 2. 清空并释放所有的 Packet 和 Frame 缓存队列
     * 3. 关闭解封装器与解码器上下文，断开文件或网络句柄
     * 4. 渲染器复位，状态机重置，准备接受下一次 Open() 调用
     */
    virtual void Stop() = 0;

    /**
     * @brief 触发媒体物理时间轴跳转 (Seek)
     * @param target_time_us 目标绝对时间（单位：微秒）
     * @note 内部将自动执行 Pause -> Flush(清空旧缓存) -> Seek -> 恢复状态 的组合原子操作
     */
    virtual void SeekTo(int64_t target_time_us) = 0;

    /**
     * @brief 获取当前媒体全局总时长
     * @return 总时长（单位：微秒，AV_TIME_BASE 时基）；直播流或未就绪返回 0
     */
    virtual int64_t GetDuration() const = 0;

    /**
     * @brief 获取图像物理宽度
     * @note 仅在 Open 成功解析流信息后有效
     */
    virtual int GetVideoWidth() const = 0;

    /**
     * @brief 获取图像物理高度
     * @note 仅在 Open 成功解析流信息后有效
     */
    virtual int GetVideoHeight() const = 0;

    /**
     * @brief 开始录制，将播放管线中的 PCM/YUV 编码并封装为 MP4 文件
     * @param path 输出文件路径（由 UI 层在录制开始时提供）
     */
    virtual void StartRecord(const std::string& path) = 0;

    /**
     * @brief 停止录制：排空编码队列、Flush 编码器、写 MP4 索引表并关闭文件
     */
    virtual void StopRecord() = 0;

    /**
     * @brief 查询当前播放器状态机的原子状态
     * @return 当前所处的 PlaybackState 枚举值
     */
    virtual PlaybackState GetState() const = 0;

    /**
     * @brief 设置播放状态监听器
     * @param listener 实现了 Listener 的监听器对象
     */
    virtual void SetListener(Listener* listener) = 0;
};

} // namespace av
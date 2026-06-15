/**
 * @file av_synchronizer.h
 * @brief 音画同步模块实现类
 * @details 继承自 IAVSynchronizer 接口，内部维护独立的高优先级线程与线程安全队列
 * 负责拦截视频解码产物，索取音频硬件主时钟，并执行精确到毫秒级的 Sleep 或 Drop 策略
 */
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "interface/iaudio_speaker.h"
#include "interface/iav_synchronizer.h"
#include "playback_sync.h"
#include "utils/thread_safe_queue.h"
#include "video_frame.h"

namespace av {

/**
 * @class AVSynchronizer
 * @brief 音画同步器核心工作类
 */
class AVSynchronizer : public IAVSynchronizer {
public:
    /**
     * @brief 构造函数
     * @param sync_ctx 全局状态机上下文（用于感知播放、暂停、跳转指令）
     * @param audio_speaker 音频发声器接口指针（用于获取物理主时钟基准）
     */
    AVSynchronizer(std::shared_ptr<SyncContext> sync_ctx, IAudioSpeaker* audio_speaker);

    /**
     * @brief 析构函数
     * @details 自动触发 Stop()，确保后台线程安全被 join 回收
     */
    ~AVSynchronizer() override;

    /**
     * @brief 注册下游监听器
     * @param listener 接收同步完成帧的目标对象（如 Player 或 VideoPipeline）
     * @note 线程安全，内部由 listener_mutex_ 保护
     */
    void SetListener(Listener* listener) override;

    /**
     * @brief 启动音画同步模块
     * @details 激活内部安全队列，并拉起后台同步线程 (ThreadLoop)
     */
    void Start() override;

    /**
     * @brief 阶段一：下发异步终止信令 -- 非阻塞
     */
    void RequestStop() override;

    /**
     * @brief 阶段二：同步等待回收并清理资源 -- 阻塞
     */
    void WaitForStop() override;

    /**
     * @brief 刷新跳转后的队列
     */
    void Flush() override;

    /**
     * @brief 清空队列，唤醒阻塞
     */
    void AbortAndClearQueue() override;

    /**
     * @brief 启动队列
     */
    void StartQueue() override;

    /**
     * @brief 接收上游视频帧并压入队列
     * @param video_frame 上游解码器产出的纯净视频帧 DTO
     */
    void PushVideoFrame(std::shared_ptr<VideoFrame> video_frame) override;

private:
    /**
     * @brief 内部核心工作线程
     * @details 死循环提取视频帧 -> 获取音频主时钟 -> 计算时间差 -> 执行 放行/休眠/丢弃
     */
    void ThreadLoop();

private:
    /// 全局状态机
    std::shared_ptr<SyncContext> sync_ctx_{nullptr};

    /// 物理音频设备的指针,用于获取时间基
    IAudioSpeaker* audio_speaker_{nullptr};

    /// 下游数据接收方
    Listener* listener_{nullptr};

    /// 保护 listener_ 指针的互斥锁，防数据竞争
    std::mutex listener_mutex_;

    /// 缓存从解码器送来的视频帧的并发队列
    ThreadSafeQueue<std::shared_ptr<VideoFrame>> video_queue_{15};

    /// 同步器工作线程
    std::thread sync_thread_;

    /// 线程物理死亡标志位
    std::atomic<bool> is_abort_{false};

    /// 强制渲染单帧，解决暂停态拖动无画面的问题
    std::atomic<bool> force_render_{false};

    /// 同步阈值：25ms = 25000 微秒
    const int64_t kSyncThresholdUs = 25000;

    /// 替代 ThreadLoop 内的 static 变量，这样每次 Start() 时可以重置
    std::chrono::steady_clock::time_point last_render_log_time_;
};

} // namespace av
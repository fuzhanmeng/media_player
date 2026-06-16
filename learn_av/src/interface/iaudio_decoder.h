/**
 * @file iaudio_decoder.h
 * @brief 音频解码器纯虚接口定义
 * @details 定义音频解码模块的标准抽象接口，完全隔离底层解码实现与上层调度逻辑
 * 支持多线程异步解码、基于全局状态机的条件挂起/唤醒、缓存冲刷与 EOF 信令处理
 */

#pragma once

#include <memory>

namespace av {

// 内部领域对象前置声明
class Packet;
class Frame;
struct StreamInfo;
struct SyncContext; // 全局同步上下文

/**
 * @class IAudioDecoder
 * @brief 音频解码器纯虚接口
 */
class IAudioDecoder {
public:
    /**
     * @brief 解码器输出事件监听器接口
     * @details 采用观察者模式，将解码后的原始音频帧异步推送给上层调度器 (FileReader)
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * @brief 通知已解码完成的PCM音频帧
         * @param frame 解码后的 PCM 音频帧，包含采样率、声道数、位深等元数据
         * @note 由底层的解码工作线程异步触发，接收方需保证数据路由的线程安全性
         */
        virtual void OnNotifyAudioFrame(std::shared_ptr<Frame> frame) = 0;

        /**
         * @brief 通知音频解码流彻底枯竭 (Audio EOF)
         * @details 表示解码器已接收到 EOF 信令，且内部缓存队列中的所有残留帧已全部排空并输出完毕
         */
        virtual void OnNotifyAudioEOF() = 0;
    };

    // ================= 静态工厂方法 =================

    /**
     * @brief 创建音频解码器实例
     * @return 解码器智能指针，由调用方管理生命周期
     */
    static std::unique_ptr<IAudioDecoder> Create();

    // ================= 构造与析构 =================

    virtual ~IAudioDecoder() = default;

    /**
     * @brief 注册解码输出事件监听器
     * @param listener 监听器指针，接收解码后的音频帧与 EOF 事件
     * @note 必须在 Init() 之前调用，否则极易丢失首帧数据
     */
    virtual void SetListener(Listener* listener) = 0;

    /**
     * @brief 初始化解码器上下文
     * @param info 纯净音频流元数据 DTO，包含编码格式、采样率、声道数等核心参数
     * @return true 初始化成功；false 初始化失败
     * @note 接口层彻底去 FFmpeg 化，实现了与物理容器解析的硬解耦
     */
    virtual bool Init(const StreamInfo& info) = 0;

    /**
     * @brief 推送压缩音频数据包 (Packet) 到解码输入队列
     * @param packet 包含 AAC/MP3 等压缩数据的数据包
     * @note 内部应实现隐式背压 (Backpressure) 机制，当输入队列满时，此方法将阻塞 Demuxer 线程
     */
    virtual void PushPacket(std::shared_ptr<Packet> packet) = 0;

    /**
     * @brief 发送流结束信令 (EOF 信令)
     * @details 向解码队列尾部投递 EOF 标记，触发解码器进入排空 (Drain) 模式
     * 当所有帧排空后，触发 Listener::OnNotifyAudioEOF()
     */
    virtual void SendEOF() = 0;

    // ================= 并发线程控制接口 =================

    /**
     * @brief 启动异步解码工作线程
     * @param sync_ctx 注入全局状态同步上下文
     * @note 线程启动后，将自主监听 SyncContext 中的 PlaybackState 原语以决定执行或休眠，不再暴露独立的 Pause 接口
     */
    virtual void Start(std::shared_ptr<SyncContext> sync_ctx) = 0;

    /**
     * @brief 阶段一：下发异步终止信令 -- 非阻塞
     */
    virtual void RequestStop() = 0;

    /**
     * @brief 阶段二：同步等待回收线程 -- 阻塞
     */
    virtual void WaitForStop() = 0;

    /**
     * @brief Flush解码器内部缓存
     * @details 清空输入/输出队列与解码器底层的参考帧缓冲区，重置解码状态机
     * 主要配合时间轴跳转 (Seek) 使用，防止残留的脏数据导致杂音或音画不同步
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
};

} // namespace av
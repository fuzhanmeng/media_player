/**
 * @file demuxer.h
 * @brief 媒体解复用引擎核心头文件
 * @details 负责将底层容器格式拆解为独立的音视频 Packet 数据流，
 * 支持基于条件变量的异步调度、背压限流机制与绝对时间轴 Seek 操作
 */
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "interface/idemuxer.h"

struct AVFormatContext;

namespace av {

class Demuxer : public IDemuxer {
public:
    Demuxer();
    ~Demuxer() override;

    /**
     * @brief 注册事件监听器
     * @param listener 注册监听器
     */
    void SetListener(Listener* listener) override;

    /**
     * @brief 执行容器解析与流元数据探测
     * @param url 本地文件路径或网络流媒体 URI
     * @return 探测并构建物理流映射是否成功
     * @note 阻塞式同步 I/O 操作，涉及 FFmpeg 的 avformat_open_input 与 avformat_find_stream_info
     */
    bool Open(const std::string& url) override;

    /**
     * @brief 启动异步解复用工作线程
     */
    void Start(std::shared_ptr<SyncContext> sync_ctx) override;

    /**
     * @brief 清空解复用器内部状态并向下游发送Flush信令
     * @note 主要用于Seek操作后清除旧数据，防止花屏
     */
    void Flush() override;

    /**
     * @brief 阶段一：异步发送终止信令 -- 非阻塞
     */
    void RequestStop() override;

    /**
     * @brief 同步等待线程退出 -- 阻塞
     */
    void WaitForStop() override;

    /**
     * @brief 触发绝对时间轴跳转
     * @param target_time_us 目标绝对时间（单位：微秒）
     */
    void SeekTo(int64_t target_time_us) override;

    /**
     * @brief 获取当前媒体容器的全局总时长
     * @return 总时长（单位：秒）
     */
    int64_t GetDuration() const override;

    /**
     * @brief 探测当前媒体是否包含视频流
     * @return 存在视频流返回 true，否则返回 false
     */
    bool HasVideo() const override;

    /**
     * @brief 探测当前媒体是否包含音频流
     * @return 存在音频流返回 true，否则返回 false
     */
    bool HasAudio() const override;

private:
    /**
     * @brief 内部方法：彻底关闭媒体并释放所有物理资源
     * @note 仅供 Open() 和析构函数调用，对上层业务屏蔽
     */
    void Close();

    /**
     * @brief 异步 I/O 线程的事件循环 (Event Loop)
     * @details 维护三级优先级调度：P0(中断/跳转响应) > P1(背压限流阻塞) > P2(物理分包读取)
     */
    void ThreadLoop();

    /**
     * @brief 响应并执行跳帧逻辑
     * @note 必须在 I/O 线程上下文中执行，防止与 av_read_frame 发生跨线程上下文撕裂
     * 操作完成后向数据总线注入 kFlush 带内信令包
     */
    void ProcessSeek();

    /**
     * @brief 单次读包结果
     */
    enum class ReadResult {
        kOk,        ///< 成功读取并派发一个 Packet（或被下游 Abort 丢弃），循环继续
        kEof,       ///< 物理到达文件末尾，进入 EOF 挂起
        kTransient, ///< 可恢复的临时读取错误，短暂退避后重试，不应误判为 EOF
    };

    /**
     * @brief 执行底层裸包提取与跨模块派发
     * @return ReadResult 读取结果状态码
     */
    ReadResult ReadAndSendPacket();

private:
    // 下游总线监听器
    Listener* listener_{nullptr};

    // FFmpeg 底层句柄与同步锁
    AVFormatContext* avformat_ctx_{nullptr};

    // 声明为 mutable，允许在 GetDuration() const 函数内部加锁
    mutable std::mutex avformat_mutex_;

    // 记录 FFmpeg 解析出的物理流索引，用于发包时的路由路由
    int audio_stream_index_{-1};
    int video_stream_index_{-1};

    // 线程与并发控制原语
    std::thread demuxer_thread_;
    std::atomic<bool> is_abort_requested_{true}; ///< 线程生命周期终结信令

    // 保存来自Player的的同步上下文
    std::shared_ptr<SyncContext> sync_ctx_;

    // 当前是否有待处理的跳转请求
    std::atomic<bool> is_seek_requested_{false}; ///< Seek 触发信令

    std::atomic<bool> is_eof_{false}; ///< 标记是否已经到达文件末尾

    // 跳转目标位置
    std::atomic<int64_t> target_seek_time_{0};
};

} // namespace av
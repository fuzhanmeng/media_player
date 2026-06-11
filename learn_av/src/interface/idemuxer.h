/**
 * @file idemuxer.h
 * @brief 解复用引擎控制接口
 */
#pragma once

#include <memory>
#include <string>

#include "define/stream_info.h"

namespace av {

// 前置声明 Packet DTO
class Packet;
struct SyncContext;
class IDemuxer {
public:
    /**
     * @brief 解封装器事件监听器接口
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * @brief 传递 流信息 到 视频解码
         * @param info 经过抽象后的视频流参数
         */
        virtual void OnNotifyVideoStream(const StreamInfo& info) = 0;

        /**
         * @brief 传递 流信息 到 音频解码
         * @param info 经过抽象后的音频流参数
         */
        virtual void OnNotifyAudioStream(const StreamInfo& info) = 0;

        /**
         * @brief 通知收到视频数据包
         */
        virtual bool OnNotifyVideoPacket(std::shared_ptr<Packet> packet) = 0;

        /**
         * @brief 通知收到音频数据包
         */
        virtual bool OnNotifyAudioPacket(std::shared_ptr<Packet> packet) = 0;

        /**
         * @brief 通知到达文件末尾
         */
        virtual void OnNotifyDemuxerEOF() = 0;

        /**
         * @brief 通知监听器（FileReader）重置下游所有的阻塞队列状态
         */
        virtual void OnSeekResetQueues() = 0;

        /**
         * @brief 通知监听器（FileReader）底层寻址与上下文冲刷已完成，申请恢复播放
         */
        virtual void OnSeekCompleteResumePlay() = 0;
    };

public:
    /**
     * @brief 获取解复用器实例
     */
    static std::unique_ptr<IDemuxer> Create();

    virtual ~IDemuxer() = default;

    /**
     * @brief 注册监听器
     * 必须在 Open 之前调用
     */
    virtual void SetListener(Listener* listener) = 0;

    /**
     * @brief 打开媒体文件并解析流信息
     * @param url 文件本地路径或网络流地址
     * @return 成功返回 true，失败返回 false
     */
    virtual bool Open(const std::string& url) = 0;

    /**
     * @brief 启动解复用后台线程，通过sync_ctx状态来判断是第一次Play还是恢复Play
     */
    virtual void Start(std::shared_ptr<SyncContext> sync_ctx) = 0;

    /**
     * @note 阶段一：异步发送终止信令 -- 非阻塞
     */
    virtual void RequestStop() = 0;

    /**
     * @file idemuxer.h
     * @brief 阶段二：同步等待线程退出 -- 阻塞
     */
    virtual void WaitForStop() = 0;

    /**
     * @brief 清空解复用器内部缓存
     * @note 主要用于Seek操作后清除旧数据
     */
    virtual void Flush() = 0;

    /**
     * @brief 触发绝对时间轴跳转
     * @param target_time_us 目标绝对时间（单位：微秒）
     */
    virtual void SeekTo(int64_t target_time_us) = 0;

    /**
     * @brief 获取媒体总时长
     * @return 总时长（单位：微秒）
     */
    virtual int64_t GetDuration() const = 0;

    /**
     * @brief 探测当前媒体是否包含视频流
     * @return 存在视频流返回 true，否则返回 false
     */
    virtual bool HasVideo() const = 0;

    /**
     * @brief 探测当前媒体是否包含音频流
     * @return 存在音频流返回 true，否则返回 false
     */
    virtual bool HasAudio() const = 0;
};

} // namespace av
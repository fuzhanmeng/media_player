/**
 * @file idemuxer.h
 * @brief 解复用引擎控制接口
 */
#pragma once

#include <memory>
#include <string>

#include "define/stream_info.h" // 引入公共的流信息 DTO

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
         * @brief 通知发现视频流
         * @param info 经过抽象后的视频流参数
         */
        virtual void OnNotifyVideoStream(const StreamInfo& info) = 0;

        /**
         * @brief 通知发现音频流
         * @param info 经过抽象后的音频流参数
         */
        virtual void OnNotifyAudioStream(const StreamInfo& info) = 0;

        /**
         * @brief 通知收到视频数据包
         */
        virtual void OnNotifyVideoPacket(std::shared_ptr<Packet> packet) = 0;

        /**
         * @brief 通知收到音频数据包
         */
        virtual void OnNotifyAudioPacket(std::shared_ptr<Packet> packet) = 0;

        /**
         * @brief 通知到达文件末尾
         */
        virtual void OnNotifyDemuxerEOF() = 0;
    };

public:
    /**
     * @brief 获取解复用器实例
     */
    static std::unique_ptr<IDemuxer> Create();

    virtual ~IDemuxer() = default;

    /**
     * @brief 注册监听器
     * 必须在 Open 之前调用，否则漏掉流初始化信息
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
     * @brief 停止解复用线程并释放资源
     * @note 调用后需要重新Open才能再次使用
     */
    virtual void Stop() = 0;

    /**
     * @brief 清空解复用器内部缓存
     * @note 主要用于Seek操作后清除旧数据
     */
    virtual void Flush() = 0;

    /**
     * @brief 进度跳转
     * @param target_time_sec 目标绝对时间（单位：秒）
     * @note 由上层 UI 负责计算好绝对秒数后传入，以支持直播流和时长异常的文件
     */
    virtual void SeekTo(double target_time_sec) = 0;

    /**
     * @brief 获取媒体总时长
     * @return 总时长（单位：秒）
     * @note 增加 const 约束，保证查询操作的安全性；返回值改为 double 提升计算精度
     */
    virtual double GetDuration() const = 0;
};

} // namespace av
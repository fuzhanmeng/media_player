/**
 * @file ivideo_decoder.h
 * @brief 视频解码器纯虚接口定义
 * @details 定义视频解码模块的标准抽象接口，彻底隔离底层解码实现与上层调度逻辑
 * 支持多线程异步解码、基于全局状态机的条件挂起/唤醒、缓存冲刷与 EOF 信令处理
 */

#pragma once

#include <memory>

namespace av {

// 内部领域对象的前置声明
class Packet;
struct VideoFrame;
struct StreamInfo;
struct SyncContext;

/**
 * @class IVideoDecoder
 * @brief 视频解码器纯虚接口
 * @note 所有方法均为线程安全设计，支持跨管线的多线程异步调用
 */
class IVideoDecoder {
public:
    /**
     * @brief 视频解码监听器
     * @details 采用观察者模式，将解码后的原始视频帧异步推送给上层调度器 (如 FileReader)
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * @brief 接收解码并色彩转换完成后的纯净视频帧 (DTO)
         * @param frame 包含 YUV/RGBA 连续内存与物理参数的智能指针
         */
        virtual void OnNotifyVideoFrame(std::shared_ptr<VideoFrame> frame) = 0;

        /**
         * @brief 视频流彻底解码结束的回调
         */
        virtual void OnNotifyVideoEOF() = 0;
    };

    /**
     * @brief 创建并返回视频解码器实例
     */
    static std::unique_ptr<IVideoDecoder> Create();

    virtual ~IVideoDecoder() = default;

    /**
     * @brief 设置下游监听器
     */
    virtual void SetListener(Listener* listener) = 0;

    /**
     * @brief 初始化解码器上下文
     * @param info 纯净视频流元数据 DTO，包含编码格式、分辨率、帧率等核心参数
     * @return true 初始化成功；false 初始化失败
     */
    virtual bool Init(const StreamInfo& info) = 0;

    /**
     * @brief 接收来自 Demuxer 的视频压缩数据包
     * @param packet 包含 H264/HEVC 等压缩数据的智能指针
     * @note 内部应实现隐式背压机制，队列满时会阻塞调用线程
     */
    virtual void PushPacket(std::shared_ptr<Packet> packet) = 0;

    /**
     * @brief 通知解码器输入已结束
     * @details 投递 EOF 标记，触发解码器排空内部残余的 B 帧/P 帧
     */
    virtual void SendEOF() = 0;

    /**
     * @brief 启动异步解码工作线程
     * @param sync_ctx 注入全局状态同步上下文
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
     * @brief 清洗解码器内部参考帧缓存
     * @details 在 Seek 操作时调用，防止解码出花屏或马赛克 (因 I/P/B 帧时序依赖断裂)
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
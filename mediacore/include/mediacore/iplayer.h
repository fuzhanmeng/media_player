/**
 * @file iplayer.h
 * @brief 播放器核心控制抽象接口定义
 * @details
 * UI 表现层与 mediacore 核心引擎之间唯一的高层边界契约：
 * 1. 架构解耦：上层应用（如 Qt/ImGui 界面、CLI 或远程 RPC 桥接）仅依赖此纯虚接口下发命令、查询状态，
 *    完全不感知底层是基于 DAG Pipeline、多线程 Link 还是 FFmpeg 软硬解码实现的细节；
 * 2. 异步命令驱动：所有控制方法（Open、Play、Pause、Stop、Seek）均为异步非阻塞设计，
 *    命令立即推入控制线程的 Mailbox 执行，调用线程（UI 线程）不会被阻塞；
 * 3. 动态视图绑定：支持在播放中途动态绑定或解除绑定 IVideoSink 显示端，
 *    Detach 返回后确保无后台线程向该视图投递帧，可安全销毁视图。
 */

#pragma once

#include <cstdint>
#include <string>

#include "mediacore/playback_state.h"

namespace mediacore {

class IVideoSink;

/**
 * @class IPlayer
 * @brief 播放器核心控制器纯虚接口
 * @details 统筹定义打开媒体、起播、暂停、定位、停止及状态查询等上层可见的全部操作能力。
 */
class IPlayer {
public:
    /**
     * @brief 默认构造函数
     */
    IPlayer() = default;

    /**
     * @brief 虚析构函数，保证派生类安全销毁
     * @note 析构时将同步等待管线完全停止并释放所有后台线程与媒体资源。
     */
    virtual ~IPlayer() = default;

    // 严禁拷贝与移动，防止抽象接口产生对象切片（Object Slicing）
    IPlayer(const IPlayer&) = delete;
    IPlayer& operator=(const IPlayer&) = delete;
    IPlayer(IPlayer&&) = delete;
    IPlayer& operator=(IPlayer&&) = delete;

    /**
     * @brief 动态挂载视频渲染显示端
     * @param sink 实现了 IVideoSink 接口的非拥有实例指针，不可为空
     * @note 内部通过 VideoSinkDispatcher 进行弱引用代理分发，调用方负责维持 sink 对象的有效生命周期。
     */
    virtual void AttachDisplayView(IVideoSink* sink) = 0;

    /**
     * @brief 动态解绑视频渲染显示端
     * @param sink 待解绑的显示端指针；若与当前绑定的对象不匹配则安全忽略
     * @note
     * 本函数为阻塞安全调用：返回时保证所有正在执行的渲染回调均已安全退出（In-flight Tracking），
     * 返回后外部即可安全释放该 sink 内存；解绑不会中断正在运行的解码流水线。
     */
    virtual void DetachDisplayView(IVideoSink* sink) = 0;

    /**
     * @brief 异步打开指定的媒体资源 URI
     * @param url 本地文件路径、网络流媒体地址（如 HTTP/RTMP/RTSP）或其他受支持的 URI
     * @note 触发后播放器状态迁移至 kOpening，内部异步执行格式解封装探测与管线拓扑构建。
     */
    virtual void Open(const std::string& url) = 0;

    /**
     * @brief 异步开始或恢复播放
     * @note 将播放器状态切换为 kPlaying，解除各节点暂停门控，时钟开始正常向前驱动。
     */
    virtual void Play() = 0;

    /**
     * @brief 异步暂停媒体播放
     * @note 将播放器状态切换为 kPaused，各工作线程在完成当前处理后在门控处挂起，保留内部队列已排队数据。
     */
    virtual void Pause() = 0;

    /**
     * @brief 异步停止播放器并释放当前全部媒体资源与解码管线
     * @note 触发五阶段安全停止协议，停止完毕后状态转为 kStopped，可重新调用 Open 打开新媒体。
     */
    virtual void Stop() = 0;

    /**
     * @brief 异步跳转到指定的目标时间戳（Seek）
     * @param timestamp_us 目标时间点（微秒，通常 >= 0）
     * @note
     * 1. 触发后状态迁移至 kSeeking，内部原子递增媒体世代（Epoch）并排空旧数据；
     * 2. 找到首张匹配的关键帧/目标帧上屏呈现并完成定格后，恢复此前的播放/暂停状态。
     */
    virtual void Seek(std::int64_t timestamp_us) = 0;

    /**
     * @brief 线程安全地获取当前播放器所处的全局状态快照
     * @return 最新的 PlaybackState 状态枚举值
     */
    virtual PlaybackState GetState() const = 0;

    /**
     * @brief 线程安全地获取当前媒体流的总时长
     * @return 媒体总时长（单位：微秒）；若媒体未打开、属于无界直播流或时长未知时返回 -1
     */
    virtual std::int64_t GetDurationUs() const = 0;
};

}  // namespace mediacore
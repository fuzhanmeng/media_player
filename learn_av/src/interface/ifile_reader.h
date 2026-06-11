/**
 * @file ifile_reader.h
 * @brief 媒体数据源外观实现类，负责解复用与解码模块的生命周期统筹、数据路由及状态机维护
 */

#pragma once

#include <memory>
#include <string>

namespace av {
struct VideoFrame; // YUV
class Frame; // PCM
struct SyncContext;

class IFileReader {
public:
    /**
     * @brief 异步数据输出监听器
     */
    class Listener {
    public:
        virtual ~Listener() = default;

        /**
         * @brief 接收解码后的纯净音频 PCM
         */
        virtual void OnFileReaderNotifyAudioFrame(std::shared_ptr<Frame> audio_frmae) = 0;

        /**
         * @brief 接收解码后的纯净视频 YUV420P
         */
        virtual void OnFileReaderNotifyVideoFrame(std::shared_ptr<VideoFrame> video_frame) = 0;

        /**
         * @brief 通知上层Player，重置所有相关数据队列的状态标志
         */
        virtual void OnFileReaderSeekResetQueues() = 0;

        /**
         * @brief 通知上层Player，底层 Seek 操作已完成，申请恢复播放状态
         */
        virtual void OnFileReaderSeekCompleteResumePlay() = 0;

        /**
         * @file ifile_reader.h
         * @brief 通知上层Player，传递获取到的采样率/声道数传递
         */
        virtual void OnFileReaderAudioInfo(int sample_rate, int channels) = 0;
    };

    /**
     * @brief 创建FileReader实例
     */
    static std::unique_ptr<IFileReader> Create();

    virtual ~IFileReader() = default;

    /**
     * @brief 设置回调监听器（通知Play模块）
     */
    virtual void SetListener(Listener* listener) = 0;

    /**
     * @brief 打开媒体文件并探测流信息
     * @param url 文件路径或网络流地址
     * @return true 探测成功并已初始化解码器；false 探测失败
     */
    virtual bool Open(const std::string& url) = 0;

    /**
     * @brief 启动底层解复用与解码流水线
     * @param sync_ctx 包含全局状态机锁与条件变量的同步上下文指针
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
     * @brief 触发绝对时间轴跳转
     * @param target_time_us 目标绝对时间（单位：微秒）
     */
    virtual void SeekTo(int64_t target_time_us) = 0;

    /**
     * @brief 强制中断解码器内部的 Packet 队列阻塞，并清空遗留数据
     */
    virtual void AbortAndClearDecoderQueues() = 0;

    /**
     * @brief 恢复解码器内部 Packet 队列的入队/出队能力
     */
    virtual void StartDecoderQueues() = 0;

    /**
     * @brief 查询媒体总时长
     */
    virtual int64_t GetDuration() const = 0;

    /**
     * @brief 获取图像宽度
     */
    virtual int GetVideoWidth() const = 0;

    /**
     * @brief 获取图像高度
     */
    virtual int GetVideoHeight() const = 0;

    /**
     * @brief 是否含有音频轨道
     */
    virtual bool HasAduio() const = 0;

    /**
     * @brief 是否含有视频轨道
     */
    virtual bool HasVideo() const = 0;
};
} // namespace av
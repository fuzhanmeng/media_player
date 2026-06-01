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
         * @brief Audio 读到了末尾
         */
        virtual void OnFileReaderNotifyAudioFinished() = 0;

        /**
         * @brief Video 读到了末尾
         */
        virtual void OnFileReaderNotifyVideoFinished() = 0;
    };

    /**
     * @file ifile_reader.h
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
     * @brief 挂起底层工作线程
     */
    virtual void Stop() = 0;

    /**
     * @brief 触发绝对时间轴跳转
     * @param time_sec 目标绝对时间（单位：秒）
     * @note 上层需保证调用此接口前，并发管线已处于 kPaused 挂起状态
     */
    virtual void SeekTo(double time_sec) = 0;

    /**
     * @brief 查询媒体总时长
     */
    virtual double GetDuration() const = 0;

    /**
     * @brief 获取图像宽度
     */
    virtual int GetVideoWidth() const = 0;

    /**
     * @brief 获取图像高度
     */
    virtual int GetVideoHeight() const = 0;
};
} // namespace av
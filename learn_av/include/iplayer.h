/**
 * @brief 播放器控制接口,采用纯虚函数设计，隔离具体实现
 */
#pragma once
#include <memory>

namespace av {

// 前置声明，用于接收播放器状态回调
class IPlaybackListener;

// 
class IPlayer {
public:
    IPlayer() = default;
    virtual ~IPlayer() = default;

    /**
     * @brief 播放器工厂方法：创建一个播放器实例
     * @param shared_gl_context 共享的 OpenGL 上下文句柄
     * @return std::shared_ptr<IPlayer> 返回创建好的播放器引擎智能指针
     */
    static std::shared_ptr<av::IPlayer> Create(void* shared_gl_context);

    /**
     * @brief 绑定视频渲染视图
     * 将底层的视频帧输出推送到指定的视图上
     * @param view_handle 渲染视图的句柄标识
     */
    virtual void AttachDisplayView(void* view_handle) = 0;

    /**
     * @brief 解绑视频渲染视图
     * 停止向该视图推送视频帧，通常在窗口销毁前调用，防止野指针崩溃。
     * @param view_handle 之前绑定的视图句柄标识
     */
    virtual void DetachDisplayView(void* view_handle) = 0;

    // 控制接口
    virtual void Play() = 0;
    virtual void Pause() = 0;

    // 获取当前播放状态
    virtual bool IsPlaying() const = 0;

    /**
     * @brief 设置播放状态监听器
     * @param listener 实现了 IPlaybackListener 的监听器对象
     */
    virtual void SetPlaybackListener(std::shared_ptr<IPlaybackListener> listener) = 0;
};

/**
 * @brief 播放器回调监听接口
 * 用于底层向 UI 层同步播放进度和状态，UI 层需继承此接口并实现逻辑。
 */
class IPlaybackListener {
public:
    virtual ~IPlaybackListener() = default;

    /**
     * @brief 播放进度更新回调
     * @param current_pos 当前播放位置 (单位：毫秒)
     * @param total_duration 视频总时长 (单位：毫秒)
     */
    virtual void OnProgress(int current_pos, int total_duration) = 0;
};
} // namespace av
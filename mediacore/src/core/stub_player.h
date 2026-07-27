/**
 * @file stub_player.h
 * @brief 在真实 DAG 解码管线尚未落地前，打通 UI → IPlayer 接口 → IVideoSink → OpenGLView 渲染
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "mediacore/iplayer.h"
#include "mediacore/playback_state.h"

namespace mediacore {

class IVideoSink;

/**
 * @brief 生成渐变测试帧的 IPlayer 桩实现
 * @details 继承 IPlayer，实现全部契约方法。视频帧不来自解码，而由 GenerateFrame
 * 合成，随时间推移色彩渐变，肉眼即可确认"帧在动、回环通了
 */
class StubPlayer : public IPlayer {
public:
    /**
     * @brief 构造函数，仅初始化成员，不启动线程
     */
    StubPlayer();

    /**
     * @brief 析构函数
     */
    ~StubPlayer() override;

    /// 禁止拷贝 / 移动： 持有 std::thread 与同步原语，语义上不可赋值

    // ================= IPlayer：渲染出口注入 =================

    /**
     * @brief 绑定视频帧接收端
     * @param sink 非拥有观察者指针
     */
    void AttachDisplayView(IVideoSink* sink) override;

    /**
     * @brief 解绑视频帧接收端
     * @param 之前绑定的接收器，调用后后台线程不再向其推帧
     */
    void DetachDisplayView(IVideoSink* sink) override;

    // ================= IPlayer：播放控制 =================

    /**
     * @brief 打开媒体源（桩：仅记录 url 并跃迁到 kOpening → 就绪）。
     * @param url 媒体地址（桩阶段不实际解析）。
     */
    void Open(const std::string& url) override;

    /**
     * @brief 开始 / 恢复播放：跃迁到 kPlaying，拉起（或唤醒）后台造帧线程
     */
    void Play() override;

    /**
     * @brief 暂停：跃迁到 kPaused，后台线程在同步点挂起（不退出）
     */
    void Pause() override;

    /**
     * @brief 停止：跃迁到 kStopped，两阶段停止后台线程并 join
     */
    void Stop() override;

    /**
     * @brief 跳转（桩：仅打日志占位，不涉及真实时间轴）
     * @param timestampUs 目标时间戳（微秒）
     */
    void Seek(int64_t timestampUs) override;

    // ================= IPlayer：状态查询 =================

    /**
     * @brief 查询当前播放状态。
     * @return 原子读取的 PlaybackState。
     */
    PlaybackState GetState() const override;

private:
    /**
     * @brief 后台造帧循环（运行在 renderThread_ 上）
     * @details 三态调度：kPlaying 造帧推帧、kPaused 挂起等待、kStopped 退出。
     *          每帧调用 GenerateFrame 合成渐变，经 mutex_ 取 sink_ 后推送，再按帧率休眠
     * @note 用 condition_variable 等待，避免暂停态忙等空转 CPU
     */
    void RenderLoop();

    /**
     * @brief 合成一帧渐变测试图（YUV420P）
     * @param frameIndex 帧序号，用于让色彩随时间平移，产生"动"的观感。
     * @note 在后台线程内分配 VideoFrame（shared_ptr 托管），填充 Y/U/V 后返回。
     */
    // 返回类型放在 .cpp 里用 std::shared_ptr<VideoFrame>；此处仅声明（避免头文件强依赖完整 VideoFrame）

    /**
     * @brief 内部状态跃迁（原子写 + 唤醒等待线程）
     * @param newState 目标状态
     */
    void ChangeState(PlaybackState newState);

private:
    // ================= 渲染出口 =================

    /// 视频帧接收端
    IVideoSink* sink_{nullptr};

    /// 保护 sink_ 读写，后台线程推帧, UI 线程 Attach/Detach
    std::mutex sinkMutex_;

    // ================= 状态机与线程同步 =================

    /// 全局播放状态
    std::atomic<PlaybackState> state_{PlaybackState::kIdle};

    /// 后台造帧线程
    std::thread renderThread_;

    /// 线程物理退出标志： true 表示 RenderLoop 跳出循环
    std::atomic<bool> abort_{false};

    /// 状态等待用互斥量
    std::mutex stateMutex_;

    /// 状态变更条件变量：Play/Pause/Stop 唤醒挂起的 RenderLoop，避免忙等
    std::condition_variable stateCv_;

    // ================= 桩帧参数 =================

    /// 测试帧宽度（像素）。
    static constexpr int kFrameWidth = 1920;

    /// 测试帧高度（像素）。
    static constexpr int kFrameHeight = 1080;

    /// 造帧帧率（帧 / 秒），决定 RenderLoop 每帧休眠时长
    static constexpr int kFps = 25;

    /// 已生成帧计数，驱动渐变色随时间平移。
    uint64_t frameIndex_{0};
};

} // namespace mediacore

#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "audio_decoder/ffmpeg_audio_decoder.h"
#include "audio_decoder/iaudio_sink.h"
#include "demux/ffmpeg_demuxer.h"
#include "mediacore/iplayer.h"
#include "mediacore/media_error.h"
#include "mediacore/playback_state.h"
#include "mediacore/player_config.h"
#include "video_decoder/ffmpeg_video_decoder.h"

namespace mediacore {

class IVideoSink;
class PlayerJoinTask;
class PlayerThreadReaper;
class FfmpegVideoDecoder;
class FfmpegAudioDecoder;

/**
 * @class OpenTask
 * @brief 管理一次异步媒体打开任务
 * @details
 * 后台线程负责打开媒体源、探测音视频流信息，并预先打开对应的解码器。
 * 内部持有 DemuxerInterruptHandle，允许外部请求中断 FFmpeg 的阻塞 I/O
 * 任务完成后保存打开结果与解码器，并通过回调通知 Player 控制线程
 *
 * 所有权约定：打开结果与解码器都必须在线程 Join 之后，通过 Take* 系列函数取出；
 * 取出前不应在其他线程读取这些成员。
 */
class OpenTask {
public:
    /**
     * @brief 启动后台媒体打开任务
     * @param media_uri 媒体文件路径或网络流 URL
     * @param open_sequence 本次 Open 操作的编号，用于识别过期任务
     * @param on_finished 打开任务结束后的通知回调，在后台线程中执行
     */
    OpenTask(std::string media_uri, std::uint64_t open_sequence, std::function<void()> on_finished);

    /**
     * @brief 请求中断后台 I/O，并等待打开线程退出
     *
     * 线程退出前可能仍在访问本对象的成员，因此必须回收线程后才能销毁对象
     */
    ~OpenTask();

    OpenTask(const OpenTask&) = delete;
    OpenTask& operator=(const OpenTask&) = delete;

    /**
     * @brief 获取本次 Open 操作的编号
     */
    std::uint64_t GetSequence() const noexcept { return open_sequence_; }

    /**
     * @brief 请求中断后台打开任务中的 FFmpeg I/O
     * @note 这是中断请求，不代表线程已经退出；需要调用 Join() 等待线程结束
     */
    void RequestStopOpen() noexcept { interrupt_handle_.RequestStop(); }

    /**
     * @brief 判断后台线程是否仍持有可 Join 的线程句柄
     * @return true 表示线程尚未 Join 或 Detach，不代表线程仍在执行
     */
    bool IsJoinable() const noexcept { return open_thread_.joinable(); }

    /**
     * @brief 检查后台打开任务是否已经完成
     * @return true 表示打开结果及错误信息已经写入
     *
     * acquire 与后台线程发布完成标志时的 release 配合，
     * 保证完成标志之前写入的结果对当前线程可见
     */
    bool IsFinished() const noexcept { return is_finished_.load(std::memory_order_acquire); }

    /**
     * @brief 移交后台线程生成的解封装器打开结果
     * @return 包含 Demuxer 实例及流信息的打开结果
     * @note 必须先调用 Join()，确保后台线程不再访问该结果
     */
    FfmpegDemuxerOpenResult TakeOpenResult() noexcept { return std::move(open_result_); }

    /**
     * @brief 移交后台线程打开的视频解码器
     * @return 媒体不含视频流、解码器打开失败或已被取走时返回 nullptr
     * @note 必须先调用 Join()；移至控制线程后由 BuildPipeline 交给解码节点
     */
    std::unique_ptr<FfmpegVideoDecoder> TakeVideoDecoder() noexcept { return std::move(video_decoder_); }

    /**
     * @brief 移交后台线程打开的音频解码器
     * @return 媒体不含音频流、解码器打开失败或已被取走时返回 nullptr
     * @note 必须先调用 Join()
     */
    std::unique_ptr<FfmpegAudioDecoder> TakeAudioDecoder() noexcept { return std::move(audio_decoder_); }

    /**
     * @brief 获取后台打开过程中产生的结构化错误
     * @note 必须在 Join() 后调用
     */
    const MediaError& Error() const noexcept { return media_error_; }

    /**
     * @brief 重新抛出后台线程捕获的异常
     * @note 必须在 Join() 后调用，由控制线程的异常处理逻辑接管
     */
    void RethrowFailure() const {
        if (open_exception_) {
            std::rethrow_exception(open_exception_);
        }
    }

    /**
     * @brief 等待后台打开线程退出，并回收线程句柄
     * @note 调用后才能安全读取后台线程写入的结果和错误信息
     */
    void Join() {
        if (open_thread_.joinable()) {
            open_thread_.join();
        }
    }

private:
    std::string media_uri_;  ///< 待打开的媒体路径或 URL
    std::uint64_t open_sequence_{0};  ///< 本次 Open 操作的编号
    DemuxerInterruptHandle interrupt_handle_;  ///< 用于请求中断 FFmpeg 阻塞 I/O

    FfmpegDemuxerOpenResult open_result_;  ///< 后台线程生成的解封装器和流信息
    MediaError media_error_;  ///< 打开失败时的结构化错误信息
    std::exception_ptr open_exception_;  ///< 后台线程捕获的 C++ 异常

    std::thread open_thread_;  ///< 执行媒体打开和流信息探测的线程
    std::atomic<bool> is_finished_{false};  ///< 后台线程是否已发布执行结果

    std::unique_ptr<FfmpegVideoDecoder> video_decoder_;  ///< 后台线程打开的视频解码器
    std::unique_ptr<FfmpegAudioDecoder> audio_decoder_;  ///< 后台线程打开的音频解码器
};

/**
 * @class Player
 * @brief IPlayer 接口的实现，负责播放器控制和 Pipeline 生命周期管理
 * @details
 * 接收 Open、Play、Pause、Stop、Seek 等控制请求，
 * 通过内部控制线程管理媒体打开、Pipeline 创建、状态转换和资源回收
 * 播放状态通过原子变量发布，播放位置通过 PlaybackTimeline 提供线程安全快照
 */
class Player : public IPlayer {
public:
    /**
     * @brief 音频输出设备工厂类型
     *
     * 工厂在控制线程中创建 IAudioSink 对象
     * 对象所有权交给 AudioSinkNode，由其工作线程调用 Open() 并执行音频输出
     */
    using AudioSinkFactory = std::function<std::unique_ptr<IAudioSink>()>;

    /**
     * @brief 创建播放器并启动内部控制线程
     * @param config 播放器配置
     * @param audio_sink_factory 音频输出设备工厂；为空时使用默认实现
     */
    explicit Player(PlayerConfig config = PlayerConfig::Default(), AudioSinkFactory audio_sink_factory = {});

    /**
     * @brief 停止播放器并回收控制线程
     * @details
     * 普通外部线程中析构时，同步等待控制线程完成清理
     * 如果在节点回调或控制线程中析构，则将控制线程交给 Reaper 延后回收，
     * 避免当前线程与 Pipeline 停止过程互相等待
     *
     * 已经开始执行的视频显示回调仍需要正常返回
     * 外部显示对象应遵守 Attach/Detach 的生命周期约定
     */
    ~Player() override;

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;

    /**
     * @brief 绑定外部视频显示对象
     * @param sink 外部显示对象的非拥有指针，Player 不负责销毁该对象
     * @note 绑定过程可能等待旧显示对象的在途回调结束，不能在视频回调内部调用
     */
    void AttachDisplayView(IVideoSink* sink) override;

    /**
     * @brief 解绑指定的视频显示对象
     * @param sink 先前绑定的显示对象
     * @note 返回后，该显示对象不再有尚未结束的分发回调，可以由调用方销毁
     *       不能在该显示对象自己的回调中调用
     */
    void DetachDisplayView(IVideoSink* sink) override;

    /**
     * @brief 异步提交媒体打开请求
     * @param url 本地媒体路径或网络流 URL
     * @note 函数返回仅表示请求已提交，不代表媒体已经打开
     */
    void Open(const std::string& url) override;

    /**
     * @brief 异步请求开始或恢复播放
     */
    void Play() override;

    /**
     * @brief 异步请求暂停播放
     */
    void Pause() override;

    /**
     * @brief 异步请求停止播放并销毁当前 Pipeline
     * @note 停止当前媒体不会关闭 Player 的内部控制线程
     */
    void Stop() override;

    /**
     * @brief 异步请求跳转到指定媒体时间
     * @param target_us 目标媒体时间，单位为微秒
     */
    void Seek(std::int64_t target_us) override;

    /**
     * @brief 线程安全地读取当前播放状态
     * @return 当前发布的 PlaybackState
     */
    PlaybackState GetState() const override;

    /**
     * @brief 读取当前媒体的总时长
     * @return 媒体总时长，单位为微秒；未知或尚未成功打开时返回 -1
     */
    std::int64_t GetDurationUs() const override;

    /**
     * @brief 读取当前播放位置
     * @return 时间轴的位置快照，单位为微秒；时间轴尚未创建时返回 0
     * @note 可供 UI 定期查询，仅用于获取播放进度，不参与解码和音频输出
     */
    std::int64_t GetPositionUs() const override;

    /**
     * @brief 一次性读取播放状态、总时长与当前位置
     * @return 三者一致的快照
     */
    IPlayer::PlaybackSnapshot GetSnapshot() const override;

private:
    struct Control;

    std::shared_ptr<Control> control_;
    ///< Player 的内部控制状态；控制线程也持有 shared_ptr，保证清理期间对象存活

    std::unique_ptr<PlayerJoinTask> join_task_;
    ///< 预先分配的线程回收任务；析构时可直接移交给 Reaper

    PlayerThreadReaper* reaper_{nullptr};
    ///< 进程级控制线程回收服务，非拥有指针
};

}  // namespace mediacore

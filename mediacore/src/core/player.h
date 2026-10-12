#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "core/pipeline.h"
#include "demux/ffmpeg_demuxer.h"
#include "mediacore/iplayer.h"
#include "mediacore/media_error.h"
#include "mediacore/playback_state.h"

namespace mediacore {

class IVideoSink;
class VideoSinkDispatcher;
class VideoSinkNode;

/**
 * @class OpenTask
 * @brief 管理单次异步媒体打开与解封装探测的后台任务
 * @details 封装网络连接建立与 FFmpeg 流信息探测等高阻塞 I/O 操作。
 *          通过传入的 DemuxerInterruptHandle 支持从外部随时主动中断 I/O 并安全回收线程。
 */
class OpenTask {
public:
    /**
     * @brief 启动异步打开后台线程
     * @param uri 待打开的媒体地址（支持本地文件路径或网络流 URL）
     * @param sequence 本次打开操作的唯一自增序号，用于过滤被取消的历史任务
     * @param on_finished 打开操作完成（成功或失败）后在后台线程中触发的回调通知
     */
    OpenTask(std::string uri, std::uint64_t sequence, std::function<void()> on_finished);

    /**
     * @brief 析构函数，主动触发中断并回收后台线程，防止线程逃逸或崩溃
     */
    ~OpenTask();

    OpenTask(const OpenTask&) = delete;
    OpenTask& operator=(const OpenTask&) = delete;

    /**
     * @brief 获取本次 Open 任务的序号
     * @return 返回分配给本任务的 sequence_
     */
    std::uint64_t GetSequence() const noexcept { return sequence_; }

    /**
     * @brief 异步请求中断正在进行的底层 FFmpeg I/O 阻塞
     */
    void RequestStopOpen() noexcept { interrupt_.RequestStop(); }

    /**
     * @brief 检查后台线程是否仍可被 Join
     * @note 返回 true 仅代表线程句柄有效且未被回收，不代表任务一定未执行完
     */
    bool IsRunning() const noexcept { return open_thread_.joinable(); }

    /**
     * @brief 移交并获取解封装器的打开结果
     * @note 仅在 Join() 成功回收线程后方可安全调用，否则存在并发读写数据竞争
     * @return 包含解封装器实例及流元数据的结构体（右值移交）
     */
    FfmpegDemuxerOpenResult TakeOpenResult() noexcept { return std::move(open_result_); }

    /**
     * @brief 获取打开过程中产生的结构化错误信息
     * @note 仅在 Join() 成功回收线程后调用
     */
    const MediaError& Error() const noexcept { return media_error_; }

    /**
     * @brief 阻塞等待并回收后台打开线程
     */
    void Join() noexcept {
        if (open_thread_.joinable()) {
            open_thread_.join();
        }
    }

private:
    std::string uri_;  ///< 目标媒体的资源路径或 URL
    std::uint64_t sequence_{0};  ///< 任务的唯一自增序号，区分并发/连续的多次 Open
    DemuxerInterruptHandle interrupt_;  ///< 传递给 FFmpeg 用于打断阻塞网络/文件读取的打断句柄
    FfmpegDemuxerOpenResult open_result_;  ///< 底层解封装器初始化输出的结果（含 Demuxer 实例和参数）
    MediaError media_error_;  ///< 打开失败时的结构化错误描述
    std::thread open_thread_;  ///< 执行耗时打开操作的后台独立工作线程
};

/**
 * @class Player
 * @brief IPlayer 接口的完整实现类，管理播放器生命周期、状态机及底层 Pipeline
 * @details 负责处理外部控制输入、驱动 Pipeline 数据流动、维护音视频同步与精准 Seek 语义，
 *          对外提供非阻塞的异步调用接口以及线程安全的无锁状态查询。
 */
class Player : public IPlayer {
public:
    /**
     * @brief 构造播放器实例，初始化事件分发器并启动内部专属控制线程
     */
    Player();

    /**
     * @brief 析构播放器，安全停止 Pipeline、中断后台任务并回收控制线程
     */
    ~Player() override;

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    /**
     * @brief 动态挂载视频显示端（弱引用，不转移所有权）
     * @param sink 外部显示窗口实例指针
     * @warning 内部会阻塞等待先前显示端所有在途回调完全结束；严禁在视频帧回调内同步调用
     */
    void AttachDisplayView(IVideoSink* sink) override;

    /**
     * @brief 卸载当前绑定的视频显示端
     * @param sink 先前挂载的显示端指针；函数成功返回后外部可立即安全销毁该对象
     */
    void DetachDisplayView(IVideoSink* sink) override;

    /**
     * @brief 异步打开媒体源（压入命令队列，立即返回）
     * @param url 本地绝对路径或网络流播放地址
     */
    void Open(const std::string& url) override;

    /**
     * @brief 异步请求开始播放（或从结束状态重新从头播放）
     */
    void Play() override;

    /**
     * @brief 异步请求暂停播放（画面定格，挂起数据驱动）
     */
    void Pause() override;

    /**
     * @brief 异步请求停止播放并完全销毁当前 Pipeline 处理拓扑
     */
    void Stop() override;

    /**
     * @brief 异步请求时间跳转（精准 Seek）
     * @param target_us 目标跳转时间戳，单位微秒
     */
    void Seek(std::int64_t target_us) override;

    /**
     * @brief 线程安全地获取当前播放状态快照（无锁原子读取）
     * @return 当前所处的 PlaybackState（如 kPlaying, kPaused, kSeeking 等）
     */
    PlaybackState GetState() const override;

    /**
     * @brief 线程安全地获取当前媒体的总时长
     * @return 媒体总时长（微秒）；若流信息未知或尚未打开成功则返回 -1
     */
    std::int64_t GetDurationUs() const override;

private:
    /**
     * @enum MsgType
     * @brief 控制线程消息信箱支持的命令与事件类型枚举
     */
    enum class MsgType : std::uint8_t {
        kComOpen,  ///< 外部命令：打开媒体
        kComPlay,  ///< 外部命令：恢复/开始播放
        kComPause,  ///< 外部命令：暂停播放
        kComStop,  ///< 外部命令：停止播放
        kComSeek,  ///< 外部命令：执行时间跳转
        kComShutdown,  ///< 内部命令：销毁并退出控制线程
        kEvtOpenFinished,  ///< 内部事件：后台 OpenTask 执行结束通知
        kEvtMediaEnded,  ///< 管道事件：媒体数据已全部渲染完毕（到达流末尾）
        kEvtNodeError,  ///< 管道事件：底层某个工作节点触发了致命错误
        kEvtSeekCompleted,  ///< 管道事件：Seek 目标帧已成功呈现在显示端上
    };

    /**
     * @struct Message
     * @brief 投递给控制线程邮箱的标准统一消息载荷
     */
    struct Message {
        MsgType type_{MsgType::kComShutdown};  ///< 消息具体类型

        std::string uri_;  ///< Open 命令携带的资源地址
        std::int64_t target_us_{0};  ///< Seek 命令目标时间，或完成时实际交付的 PTS（微秒）
        std::uint64_t epoch_{0};  ///< 事件关联的 Seek 世代编号，用于丢弃老世代的乱序回调
        std::string node_name_;  ///< 上报错误或状态的底层节点名称
        std::string error_msg_;  ///< 人类可读的文本错误描述
        MediaError media_error_;  ///< 结构化媒体错误详情
        std::exception_ptr exception_{};  ///< 底层捕获并转发的 C++ 异常指针
        std::uint64_t open_seq_{0};  ///< 触发该事件的 Open 操作唯一编号
        bool is_reached_eof_{false};  ///< Seek 过程中是否触底直接到达文件末尾（EOF）
        std::uint64_t pipeline_gen_{0};  ///< 事件所属 Pipeline 实例的版本号，防止跨图污染

        Message() = default;
        explicit Message(MsgType type) : type_(type) {}
    };

    /**
     * @brief 控制线程的工作主循环：阻塞监听并从 mailbox 中逐条取出消息串行处理
     */
    void ControlLoop() noexcept;

    /**
     * @brief 消息分发器：根据消息类型路由至具体的业务处理函数
     * @param msg 待处理的消息结构体
     */
    void ProcessMessage(const Message& msg);

    /**
     * @brief 处理打开媒体命令：中断正在执行的任务，启动后台 OpenTask
     * @param uri 待打开的媒体地址
     */
    void HandleOpen(const std::string& uri);

    /**
     * @brief 处理后台打开任务完成事件：校验序列号并装配 Pipeline
     * @param open_seq 汇报完成的任务序号
     */
    void HandleOpenFinished(const std::uint64_t open_seq);

    /**
     * @brief 处理播放命令：解除节点挂起，或在播放结束后触发重新从头播放
     */
    void HandlePlay();

    /**
     * @brief 处理停止命令：清空意图标记，同步停止并销毁当前 Pipeline
     */
    void HandleStop();

    /**
     * @brief 处理暂停命令：挂起 Pipeline 数据流动，定格当前画面
     */
    void HandlePause();

    /**
     * @brief 处理时间跳转命令：过滤非法状态并调用 StartSeek
     * @param target_us 目标时间戳（微秒）
     */
    void HandleSeek(std::int64_t target_us);

    /**
     * @brief 处理底层节点上报的媒体播放结束事件（EOS）
     * @param pipeline_gen 事件来源的 Pipeline 版本号
     * @param epoch 事件所属的 Seek 世代号
     */
    void HandleMediaEnded(std::uint64_t pipeline_gen, std::uint64_t epoch);

    /**
     * @brief 处理底层节点上报的不可恢复性致命错误
     * @param pipeline_gen 事件来源的 Pipeline 版本号
     * @param epoch 发生错误时的 Seek 世代号
     * @param media_error 结构化错误对象
     * @param message 错误描述信息
     */
    void HandleNodeError(std::uint64_t pipeline_gen,
                         std::uint64_t epoch,
                         const MediaError& media_error,
                         const std::string& message);

    /**
     * @brief 处理渲染节点上报的 Seek 目标帧交付上屏事件
     * @param pipeline_gen 事件来源的 Pipeline 版本号
     * @param epoch 完成时的 Seek 世代号
     * @param actual_pos_us 实际定格呈现的媒体时间戳（微秒）
     * @param is_reached_eof 是否因目标超长直接依靠末尾帧兜底
     */
    void HandleSeekCompleted(std::uint64_t pipeline_gen,
                             std::uint64_t epoch,
                             std::int64_t actual_pos_us,
                             bool is_reached_eof);

    /**
     * @brief 发起一次底层 Seek 定位流程，并记录跳转完成后是继续播放还是保持定格暂停
     * @param target_us 跳转目标时间戳（微秒）
     * @param resume_playing 目标帧上屏后是否自动恢复播放状态
     * @return true 表示 Pipeline 成功接受并启动了该 Seek 流程
     */
    bool StartSeek(std::int64_t target_us, bool resume_playing);

    /**
     * @brief 检查最终视频输出节点是否已经消费并处理了当前世代的 EOS 标志
     * @return true 表示渲染节点已经彻底无新数据上屏
     */
    bool IsOutputAtEnd() const noexcept;

    /**
     * @brief 校验异步事件是否来源于当前依然活跃的 Pipeline 实例
     * @param pipeline_gen 事件所绑定的 Pipeline 实例生成编号
     * @return true 表示属于当前 Pipeline；false 表示属于已废弃/销毁的历史 Pipeline，应丢弃
     */
    bool IsCurrentPipelineGen(std::uint64_t pipeline_gen) const noexcept;

    /**
     * @brief 线程安全地向控制线程的消息信箱投递一条命令或事件
     * @param msg 待投递的消息载荷
     */
    void Post(Message msg);

    /**
     * @brief 核心装配函数：根据解封装结果创建 Node 拓扑图，建立 Link 管道并启动流水线
     * @param open_result 解封装器输出的上下文与流参数（所有权转移）
     * @return true 表示全图各节点连接并启动成功
     */
    bool BuildPipeline(FfmpegDemuxerOpenResult open_result);

    /**
     * @brief 构造并绑定流水线回调函数包，负责将 Node 线程的底层事件打包并 Post 到 mailbox
     * @param generation 当前装配的 Pipeline 所属的唯一版本编号
     * @return 包含 on_media_ended_、on_node_error_、on_seek_completed_ 的回调集合
     */
    PipelineEventCallbacks MakePipelineCallbacks(std::uint64_t generation);

    /**
     * @brief 同步停止各工作节点并完全释放当前的 Pipeline 实例内存
     */
    void StopCurrentPipeline();

    /**
     * @brief 原子更新播放器状态机，并在状态真正迁移时输出流转日志
     * @param next_state 目标流转的新状态
     */
    void SetState(PlaybackState next_state) {
        // exchange 原子写入新状态并返回原先的旧状态
        const PlaybackState previous = state_.exchange(next_state, std::memory_order_release);

        // 过滤重复设置相同状态的空操作，仅在状态真正发生流转时输出日志
        if (previous != next_state) {
            LOG_INFO("Player state {} -> {}", PlaybackStateToString(previous), PlaybackStateToString(next_state));
        }
    }

private:
    std::thread control_thread_;
    std::mutex mailbox_mutex_;
    std::condition_variable mailbox_cv_;  ///< 用于唤醒控制线程进行消息消费的条件变量
    std::queue<Message> mailbox_;  ///< FIFO 消息信箱队列
    bool shutting_down_{false};  ///< 播放器是否正在退出与销毁的退出标志位

    std::unique_ptr<OpenTask> open_task_;  ///< 当前正在后台执行或尚未回收的打开任务
    std::uint64_t next_open_sequence_{0};  ///< 用于分配给下一次 Open 的递增序号计数器

    std::unique_ptr<Pipeline> pipeline_;  ///< 当前媒体流的处理流水线拓扑实例（独占所有权）
    std::shared_ptr<VideoSinkDispatcher>
        video_sink_dispatcher_;  ///< 负责在渲染节点与外部 UI 显示端之间安全分发帧的中介
    VideoSinkNode* video_sink_node_{nullptr};  ///< 指向当前 Pipeline 内部渲染终端的观察裸指针（不拥有生命周期）

    std::atomic<PlaybackState> state_{PlaybackState::kIdle};  ///< 当前对外暴露的播放状态（无锁线程安全原子变量）
    bool seek_resume_playing_{false};  ///< 标志位：本次 Seek 目标帧呈现定格后，是否自动解除暂停继续播放
    bool play_when_opened_{false};  ///< 抢跑标志位：在后台媒体打开期间用户是否曾提前点击过 Play

    std::atomic<std::int64_t> durations_us_{-1};  ///< 对外提供的视频总时长缓存（微秒，支持无锁读取）
    std::uint64_t pipeline_generation_{0};  ///< Pipeline 拓扑实例代号（每次调用 BuildPipeline 递增）
};

}  // namespace mediacore
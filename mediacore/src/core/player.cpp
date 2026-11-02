/**
 * @file player.cpp
 * @brief Player 的命令处理、Pipeline 装配以及控制线程的退出与回收
 * @details
 * 外部线程投递 Open/Player/Pause/Seek/Stop。专属控制线程消费邮箱并操作 Pipeline
 * OpenTask 另开线程执行打开媒体的操作，每个 Node 再有 Pipeline 启动自己的工作线程
 * Player 对象、Control 对象和这些线程具有不同的生命周期，回调内销毁 Player 时，Control 仍须存活到节点停止
 */

#include "core/player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "audio_decoder/ffmpeg_audio_decoder.h"
#include "audio_decoder/ffmpeg_audio_decoder_node.h"
#include "av_sync/av_sync_node.h"
#include "core/inode.h"
#include "core/link.h"
#include "core/pipeline.h"
#include "demux/ffmpeg_demuxer_node.h"
#include "mediacore/ivideo_sink.h"
#include "mediacore/logger.h"
#include "mediacore/media_error.h"
#include "sink/audio_sink_node.h"
#include "sink/video_sink_dispatcher.h"
#include "sink/video_sink_node.h"
#include "video_decoder/ffmpeg_video_decoder_node.h"

namespace mediacore {

/**
 * @brief 创建一次后台媒体打开任务
 * @param media_uri 媒体文件路径或网络 URL；任务会保存自己的副本
 * @param open_sequence 本次 Open 的编号，用于过滤被后续 Open 替代的旧任务
 * @param on_open_finished 打开结束后的通知回调；只通知控制线程，不创建 Pipeline
 *
 * 工作线程写入打开结果或异常后，先以 release 语义发布 is_finished_，再投递完成通知
 * 控制线程通过 Join() 等待线程退出后读取结果，避免与后台线程并发访问这些成员
 */
OpenTask::OpenTask(std::string media_uri, std::uint64_t open_sequence, std::function<void()> on_open_finished)
    : media_uri_(std::move(media_uri)), open_sequence_(open_sequence) {
    // 打开操作可能长时间阻塞；放到独立线程后，控制线程仍能接收 Stop 或新的 Open
    open_thread_ = std::thread([this, on_open_finished = std::move(on_open_finished)] {
        try {
            // 此处只打开输入并探测流信息；播放期间的持续读包由 Pipeline 的 Demuxer 节点负责
            open_result_ = FfmpegDemuxer::Open(media_uri_, interrupt_handle_);
            media_error_ = open_result_.error_;
        } catch (...) {
            // 保存异常对象，留给控制线程处理；不要让异常逃出线程入口，否则程序会 terminate
            open_exception_ = std::current_exception();
        }

        is_finished_.store(true, std::memory_order_release);
        // 先发布完成标志，再发送通知；即使通知入队失败，控制循环也会定期检查 IsFinished()
        try {
            if (on_open_finished)
                on_open_finished();
        } catch (...) {
            // 线程入口不能让异常逃出去，否则会导致 std::terminate
        }
    });
}

/**
 * @brief 确保打开线程结束后，再释放 OpenTask 保存的资源
 *
 * 中断请求只是要求 FFmpeg 尽快停止 I/O，不代表线程已经结束；Join 才会等待线程退出
 * 线程仍可能访问本对象的 media_uri_、interrupt_handle_ 和结果成员，所以不能只发中断就销毁对象
 */
OpenTask::~OpenTask() {
    if (open_thread_.joinable()) {
        interrupt_handle_.RequestStop();
        open_thread_.join();
    }
}

/**
 * @brief 延后 Join 控制线程时使用的任务节点
 *
 * control_thread 保存待回收的控制线程句柄，next_task 将任务节点连接成单向链表
 * 每个 Player 在构造时就创建这个节点若析构发生在不能等待控制线程的线程中，
 * 只需把这个 unique_ptr 移交给 Reaper，不必在析构现场再分配一个队列节点
 * 这里不保存 Player 或 Control：控制线程入口的 lambda 已经通过 shared_ptr 持有 Control
 */
struct PlayerJoinTask {
    std::thread control_thread_;  ///< Player 控制线程句柄；这里只保存句柄，不负责请求线程退出
    std::unique_ptr<PlayerJoinTask> next_task_;  ///< 拥有链表中的下一个待回收任务
};

/**
 * @class PlayerThreadReaper
 * @brief 所有 Player 共用的控制线程回收服务
 *
 * 当 Player 的析构线程不能直接 Join 控制线程时，把线程句柄交给本服务
 * Reaper 自己的工作线程稍后 Join 该控制线程这样当前节点回调可以先返回，
 * 控制线程便能继续停止 Pipeline 并等待节点退出，不会和正在执行的回调互相等待
 *
 * 入队使用 Player 事先分配好的 PlayerJoinTask，因此 Adopt 只移动所有权，不创建新节点
 * 本服务不使用 detach；程序应在静态对象开始析构前释放 Player，避免退出阶段仍有媒体回调
 */
class PlayerThreadReaper final {
public:
    /**
     * @brief 取得进程内共享的回收服务
     * @return 返回静态 Reaper 对象的引用；调用者不拥有它，也不负责释放它
     *
     * 第一次调用时创建对象并启动一个回收线程；之后所有 Player 都复用这个对象和线程
     */
    static PlayerThreadReaper& Instance() {
        static PlayerThreadReaper instance;
        return instance;
    }
    /**
     * @brief 把一个待 Join 的控制线程交给 Reaper
     * @param join_task Player 预先创建的任务节点；调用后由 Reaper 接管所有权
     *
     * 此函数只把节点接到队尾并唤醒回收线程，不在调用者线程里 Join
     * 这点很重要：调用者可能正是 Pipeline 要等待其回调返回的工作线程
     * task_queue_mutex_ 只保护链表指针和 shutdown_requested_，不保护真正的 Join 等待过程
     */
    void Adopt(std::unique_ptr<PlayerJoinTask> join_task) noexcept {
        std::lock_guard<std::mutex> lock(task_queue_mutex_);
        auto* task_tail = join_task.get();
        if (task_queue_tail_) {
            task_queue_tail_->next_task_ = std::move(join_task);
        } else {
            task_queue_head_ = std::move(join_task);
        }
        task_queue_tail_ = task_tail;
        task_available_cv_.notify_one();
    }

private:
    /** @brief 启动共享回收线程；线程启动后负责执行 Run() 中的取队列和 Join 循环 */
    PlayerThreadReaper() : reaper_thread_([this] { Run(); }) {}
    /**
     * @brief 关闭回收服务，并等待回收线程完成剩余任务后退出
     *
     * shutdown_requested_ 表示服务不再继续空等新任务Run() 仍会处理队列里已有的任务，
     * 每个任务对应的控制线程结束后，服务线程才会退出
     * 这里不会强制终止卡住的控制线程，因此应用仍应在正常退出流程中先销毁 Player
     */
    ~PlayerThreadReaper() {
        {
            // 修改 shutdown_requested_ 时持有与 Run() 相同的锁，避免 Run 检查条件与进入等待之间漏掉通知
            std::lock_guard<std::mutex> lock(task_queue_mutex_);
            shutdown_requested_ = true;
        }
        task_available_cv_.notify_one();
        reaper_thread_.join();
    }
    /**
     * @brief 从任务队列取出控制线程句柄，并在队列锁外等待它结束
     *
     * 队列为空时等待新任务或关闭通知；关闭且队列为空时退出
     * Join 只等待线程结束，不会替该线程发送停止请求；停止工作由 ControlLoop 完成
     */
    void Run() noexcept {
        while (true) {
            std::unique_ptr<PlayerJoinTask> join_task;
            {
                std::unique_lock<std::mutex> lock(task_queue_mutex_);
                // wait 会在睡眠时释放锁，醒来后重新加锁并检查条件；即使虚假唤醒也不会误退出
                task_available_cv_.wait(lock, [this] { return shutdown_requested_ || task_queue_head_ != nullptr; });

                if (!task_queue_head_) {
                    return;
                }

                join_task = std::move(task_queue_head_);
                task_queue_head_ = std::move(join_task->next_task_);
                if (!task_queue_head_) {
                    task_queue_tail_ = nullptr;
                }
            }
            // Join 可能等待很久，所以先释放队列锁；否则其他 Player 连任务都无法加入队列
            if (join_task->control_thread_.joinable()) {
                join_task->control_thread_.join();
            }
        }
    }
    std::mutex task_queue_mutex_;  ///< 保护任务链表和 shutdown_requested_；不会在 Join 期间一直持有这把锁
    std::condition_variable task_available_cv_;  ///< 有新任务到来或服务开始关闭时，唤醒回收线程
    std::unique_ptr<PlayerJoinTask> task_queue_head_;  ///< 拥有第一个任务；后续任务由各节点的 next_task_ 拥有
    PlayerJoinTask* task_queue_tail_{nullptr};  ///< 记录最后一个节点的位置，不拥有该节点
    bool shutdown_requested_{false};  ///< 只在持有 task_queue_mutex_ 时读写；置位后仍需处理完队列中的任务
    std::thread reaper_thread_;  ///< 逐个 Join 已移交的控制线程；不负责停止 Pipeline
};

/**
 * @brief Player 的内部共享控制状态
 *
 * Control 保存 Player 的异步控制状态；ControlLoop 是唯一操作 Pipeline 生命周期的线程
 * 应用线程、OpenTask 线程和节点线程都通过 Post() 把命令或事件交给它处理
 *
 * 控制线程入口捕获 shared_ptr<Control>，因此 Player 外壳析构后 Control 仍能完成清理
 * 控制线程句柄由 Player 或 Reaper 持有，不放在 Control 内，避免控制线程在释放最后一个
 * Control 引用时尝试 Join 自己
 */
struct Player::Control {
    friend class Player;
    explicit Control(PlayerConfig config, AudioSinkFactory audio_sink_factory);
    void AttachDisplayView(IVideoSink* display_sink);
    void DetachDisplayView(IVideoSink* display_sink);
    void Open(const std::string& media_uri);
    void Play();
    void Pause();
    void Stop();
    void Seek(std::int64_t target_position_us);
    PlaybackState GetState() const;
    std::int64_t GetDurationUs() const;
    std::int64_t GetPositionUs() const;
    /**
     * @brief 取得当前发布的时间轴，并在本次查询期间持有它
     * @return 当前时间轴的 shared_ptr；尚未建好 Pipeline 或停止后可能为空
     *
     * atomic_load 保护的是 shared_ptr 句柄的并发读写时间轴对象内部数据的同步，
     * 仍由 PlaybackTimeline 自己负责
     */
    std::shared_ptr<PlaybackTimeline> LoadTimeline() const noexcept { return std::atomic_load(&timeline_); }
    /**
     * @brief 请求控制线程退出，并唤醒它
     *
     * 本函数不直接停止 Pipeline，也不等待任何线程ControlLoop 看到退出标志后会调用
     * HandleStop() 清理当前媒体，然后退出循环使用独立标志而不是投递消息，
     * 是为了避免退出请求还依赖一次可能分配内存的队列入队操作
     */
    void RequestShutdown() noexcept {
        // 和 ControlLoop 使用同一把锁，避免控制线程检查退出标志后、真正睡眠前漏掉唤醒
        std::lock_guard<std::mutex> lock(mailbox_mutex_);
        shutting_down_.store(true, std::memory_order_release);
        mailbox_cv_.notify_all();
    }

public:
    /**
     * @enum MsgType
     * @brief 消息队列中可以传递的外部命令和内部事件
     *
     * kCom 开头的是控制请求；kEvt 开头的是打开任务或 Pipeline 工作节点发出的通知
     */
    enum class MsgType : std::uint8_t {
        kComOpen,  ///< 外部命令：打开媒体
        kComPlay,  ///< 外部命令：恢复/开始播放
        kComPause,  ///< 外部命令：暂停播放
        kComStop,  ///< 外部命令：停止播放
        kComSeek,  ///< 外部命令：执行时间跳转
        kComShutdown,  ///< 内部命令：销毁并退出控制线程
        kEvtOpenFinished,  ///< 内部事件：后台 OpenTask 执行结束通知
        kEvtMediaEnded,  ///< 管道事件：所有实际存在的轨道均已登记结束
        kEvtNodeError,  ///< 管道事件：底层某个工作节点触发了致命错误
        kEvtSeekCompleted,  ///< 管道事件：所有活动轨道达到 Seek 就绪屏障后，由最后就绪轨道上报
    };

    /**
     * @struct Message
     * @brief 控制线程消息队列使用的统一消息结构
     *
     * 不同消息只填写自己需要的字段：Open 使用 uri_，Seek 命令使用 seek_target_us_，
     * Seek 完成事件使用 seek_actual_position_us_；Pipeline 事件使用 pipeline_generation_ 和 epoch_
     */
    struct Message {
        MsgType type_{MsgType::kComShutdown};  ///< 命令或事件的类型

        std::string uri_;  ///< Open 请求要打开的媒体路径或 URL
        std::int64_t seek_target_us_{0};  ///< kComSeek 的目标位置，单位微秒
        std::int64_t seek_actual_position_us_{0};  ///< kEvtSeekCompleted 报告的就绪位置，单位微秒
        std::uint64_t seek_epoch_{0};  ///< 事件所属的 Seek 轮次，用于过滤过期的结束和就绪事件
        std::string source_node_name_;  ///< 产生节点事件的工作节点名称
        std::string node_error_message_;  ///< 节点错误的可读描述
        MediaError node_media_error_;  ///< 节点错误的结构化信息
        std::exception_ptr node_exception_{};  ///< 节点工作线程捕获的原始异常；可能为空
        std::uint64_t open_sequence_{0};  ///< 产生完成事件的 Open 编号，用于过滤旧任务通知
        bool all_tracks_reached_eof_{false};  ///< 所有活动轨道是否都在 Seek 过程中到达 EOF
        std::uint64_t pipeline_generation_{0};  ///< 事件所属的 Pipeline 实例编号，用于丢弃旧实例的延迟事件

        Message() = default;
        explicit Message(MsgType type) : type_(type) {}
    };

    /**
     * @brief 控制线程入口声明
     *
     * 该线程循环等待消息并串行处理所有 Pipeline 生命周期操作普通 Stop 只停止当前媒体；
     * 只有析构请求退出时，循环才会在完成清理后真正结束
     */
    void ControlLoop() noexcept;

    /** @brief 根据消息类型调用对应的命令处理或事件处理函数 */
    void ProcessMessage(const Message& message);

    /** @brief 停止旧媒体并启动新的后台打开任务；实际打开 I/O 不在控制线程中执行 */
    void HandleOpen(const std::string& uri);

    /** @brief 收到 OpenTask 完成通知后检查任务编号、读取结果并构建 Pipeline */
    void HandleOpenFinished(std::uint64_t open_sequence);

    /** @brief 根据当前状态开始或恢复播放；如果已经到达媒体末尾，则尝试跳转到开头重播 */
    void HandlePlay();

    /** @brief 停止并释放当前媒体，但保留控制线程，以便之后还能接收新的 Open */
    void HandleStop();

    /**
     * @brief 应用暂停请求
     *
     * 如果媒体仍在打开或正在 Seek，只记录“之后保持暂停”的意图；正常播放时才暂停时间轴并门控节点
     * 本函数不 Join 节点，也不等待已经开始执行的显示回调完成
     */
    void HandlePause();

    /** @brief 校验当前状态后，启动一次时间跳转 */
    void HandleSeek(std::int64_t target_position_us);

    /** @brief 处理 Pipeline 发来的媒体结束事件；只接受当前 Pipeline 和当前 Seek 轮次的事件 */
    void HandleMediaEnded(std::uint64_t event_pipeline_generation, std::uint64_t event_seek_epoch);

    /**
     * @brief 处理工作节点报告的错误
     *
     * 由控制线程停止当前 Pipeline，避免在报告错误的节点回调栈上 Join 该节点自身
     * 即使事件的 epoch 已过期，只要它来自当前 Pipeline，通常也不能忽略：节点可能已经无法继续工作
     */
    void HandleNodeError(std::uint64_t event_pipeline_generation,
                         std::uint64_t event_seek_epoch,
                         const MediaError& node_media_error,
                         const std::string& node_error_message);

    /**
     * @brief 处理所有活动轨道都已完成 Seek 准备的通知
     * @param actual_position_us 节点报告的就绪位置，仅用于记录，不作为公共时钟的新起点
     * @param all_tracks_reached_eof 是否所有活动轨道都在 Seek 过程中到达 EOF
     */
    void HandleSeekCompleted(std::uint64_t event_pipeline_generation,
                             std::uint64_t event_seek_epoch,
                             std::int64_t actual_position_us,
                             bool all_tracks_reached_eof);

    /**
     * @brief 请求 Pipeline 定位到目标时间，并记住定位完成后是继续播放还是暂停
     * @return true 表示 Pipeline 接受了请求；不代表所有轨道已经定位完成
     */
    bool StartSeek(std::int64_t target_position_us, bool should_resume_after_seek);

    /** @brief 查询时间轴是否已将所有实际存在的轨道标记为结束 */
    bool IsOutputAtEnd() const noexcept;

    /**
     * @brief 判断事件是否属于最近一次创建的 Pipeline
     *
     * 编号相同只说明事件来自同一个 Pipeline 实例；调用处仍需检查 Pipeline 是否存在，
     * 并按事件类型判断 epoch 是否仍有效
     */
    bool IsCurrentPipelineGen(std::uint64_t event_pipeline_generation) const noexcept;

    /** @brief 将命令或工作节点事件放入消息队列，并通知控制线程 */
    void Post(Message message);

    /**
     * @brief 根据媒体实际包含的轨道创建节点、连接 Link 并启动 Pipeline
     * @param open_result OpenTask 产生的解封装器和编解码参数；所需资源会移动到对应节点
     * @return true 表示节点连接和线程启动成功，不保证音频设备此时已经打开或首帧已经显示
     */
    bool BuildPipeline(FfmpegDemuxerOpenResult open_result);

    /**
     * @brief 为当前 Pipeline 创建事件回调
     *
     * 节点线程触发回调时，只把事件打包并 Post 到消息队列；真正的停止、改状态等处理在控制线程完成
     * generation 用于区分不同 Pipeline，epoch 用于区分同一个 Pipeline 内不同的 Seek/播放轮次
     */
    PipelineEventCallbacks MakePipelineCallbacks(std::uint64_t pipeline_generation);

    /**
     * @brief 停止并销毁当前 Pipeline，同时撤销对外发布的时间轴
     *
     * 必须在控制线程调用，不能从节点回调中调用，因为 Pipeline::Stop() 会等待节点退出
     * 已经取得时间轴 shared_ptr 的查询线程可以继续持有旧时间轴，直到自己的副本释放
     */
    void StopCurrentPipeline();

    /**
     * @brief 更新对外播放状态，并在状态发生变化时写日志
     *
     * 仅由控制线程调用此函数只写状态，不负责验证状态转换，也不执行 Pause/Stop 等动作；
     * 具体操作顺序由调用它的处理函数决定
     */
    void SetState(PlaybackState next_state) {
        const PlaybackState previous = state_.exchange(next_state, std::memory_order_release);

        if (previous != next_state) {
            LOG_INFO("Player state {} -> {}", PlaybackStateToString(previous), PlaybackStateToString(next_state));
        }
    }

private:
    std::mutex mailbox_mutex_;  ///< 保护消息队列，也与退出标志的检查/等待配合使用
    std::condition_variable mailbox_cv_;  ///< 新消息或退出请求到来时，用它唤醒控制线程
    std::queue<Message> mailbox_;  ///< 先提交的消息先处理
    std::atomic<bool> shutting_down_{false};  ///< true 表示控制线程应清理媒体并退出

    std::unique_ptr<OpenTask> open_task_;  ///< 当前 OpenTask；即使已完成，也要 Join 后才能释放
    std::uint64_t next_open_sequence_{0};  ///< 为每次 Open 分配不同编号

    std::unique_ptr<Pipeline> pipeline_;  ///< 当前媒体的 Pipeline；仅由控制线程访问和销毁
    std::shared_ptr<VideoSinkDispatcher>
        video_sink_dispatcher_;  ///< 在视频节点和外部显示对象之间转发帧，并协调解绑与在途回调
    std::shared_ptr<PlaybackTimeline> timeline_;  ///< 通过 atomic_load/atomic_store 发布，供其他线程查询位置
    PlayerConfig config_;
    AudioSinkFactory audio_sink_factory_;
    std::chrono::steady_clock::time_point seek_deadline_{};  ///< 当前 Seek 的超时时刻；仅在 kSeeking 状态下检查

    std::atomic<PlaybackState> state_{PlaybackState::kIdle};  ///< 对外发布的状态；其他线程可原子读取
    bool resume_after_seek_{false};  ///< 本轮 Seek 完成后是否恢复播放
    bool play_after_open_{false};  ///< Open 期间收到的最终播放意图，打开完成后应用

    std::atomic<std::int64_t> media_duration_us_{-1};  ///< 对外发布的媒体总时长；无媒体或未知时为 -1，单位微秒
    std::uint64_t pipeline_generation_{0};  ///< 每次创建 Pipeline 时递增，用于识别旧 Pipeline 发出的事件
    bool can_attempt_seek_{true};  ///< Open 阶段得到的 Seek 能力判断；发起定位时还会检查 Pipeline::CanSeek()
};

/**
 * @brief 创建 Player 的内部状态并启动专属控制线程
 *
 * 构造阶段先创建 Control、一个预备的 Join 任务节点和共享 Reaper函数体中启动控制线程，
 * 线程入口通过 shared_ptr 持有 Control，而不是捕获 Player 的 this这样即使 Player 在回调内
 * 先销毁，Control 仍可留存到线程处理退出请求、清理 Pipeline 并返回为止
 */
Player::Player(PlayerConfig config, AudioSinkFactory audio_factory)
    : control_(std::make_shared<Control>(config, std::move(audio_factory))),
      join_task_(std::make_unique<PlayerJoinTask>()),
      reaper_(&PlayerThreadReaper::Instance()) {
    // 线程入口持有 Control 的 shared_ptr，确保 Player 析构后控制线程仍能安全完成清理
    join_task_->control_thread_ = std::thread([control = control_]() { control->ControlLoop(); });
}

/**
 * @brief 关闭显示回调入口，通知控制线程退出，并回收控制线程句柄
 *
 * 在普通外部线程中析构时，可以直接 Join 控制线程，直到 Pipeline 和 OpenTask 清理完毕
 * 如果析构发生在节点工作线程、显示回调或控制线程自身，则不能在这里 Join：控制线程停止
 * Pipeline 时可能正在等待当前回调返回，或者当前线程就是控制线程本身这些情况下把线程句柄
 * 交给 Reaper，先让析构返回，待当前回调退出后再由 Reaper 等待控制线程结束
 *
 * 委托回收路径不保证析构返回时后台清理已经完成关闭分发器会阻止新的显示回调进入，
 * 但已开始的回调仍需返回；外部显示对象必须按 Attach/Detach 的约定管理自身生命周期
 */
Player::~Player() {
    // 先阻止分发器启动新的显示回调，再通知控制线程清理 Pipeline已开始的回调仍需自行返回
    control_->video_sink_dispatcher_->Close();
    control_->RequestShutdown();
    if (WorkerThreadContext::IsWorkerThread() || control_->video_sink_dispatcher_->IsInCallback() ||
        join_task_->control_thread_.get_id() == std::this_thread::get_id()) {
        // 当前线程不能等待控制线程，移交句柄后由 Reaper 在其他线程中 Join
        reaper_->Adopt(std::move(join_task_));
    } else if (join_task_->control_thread_.joinable()) {
        // 当前线程不参与 Pipeline 回调，可直接等待控制线程完成全部清理
        join_task_->control_thread_.join();
    }
}

/**
 * @brief 将外部视频显示对象绑定到分发器
 * @param display_sink 非拥有指针；Player 不负责销毁该对象
 *
 * Attach 直接操作分发器，不经过控制线程若需要等待旧显示回调结束，不能从显示回调内部调用
 */
void Player::AttachDisplayView(IVideoSink* display_sink) {
    control_->AttachDisplayView(display_sink);
}

/**
 * @brief 解绑指定的外部视频显示对象
 * @param display_sink 先前绑定的非拥有指针
 *
 * 返回后，分发器中不再有针对该对象的在途回调，调用方才可以销毁它
 * 不能从显示回调内部调用，否则会等待当前回调自己结束
 */
void Player::DetachDisplayView(IVideoSink* display_sink) {
    control_->DetachDisplayView(display_sink);
}

/** @brief 异步提交 Open 请求；函数返回不代表媒体已经打开 */
void Player::Open(const std::string& url) {
    control_->Open(url);
}

/** @brief 异步提交 Play 请求；具体动作由控制线程按当前状态决定 */
void Player::Play() {
    control_->Play();
}

/** @brief 异步提交 Pause 请求；函数返回时节点可能还没来得及执行暂停 */
void Player::Pause() {
    control_->Pause();
}

/** @brief 异步提交 Stop 请求；它停止当前媒体，不会关闭 Player 的控制线程 */
void Player::Stop() {
    control_->Stop();
}

/** @brief 异步提交 Seek 请求；参数单位为微秒，合法性由控制线程检查 */
void Player::Seek(std::int64_t target_us) {
    control_->Seek(target_us);
}

/** @brief 读取最近发布的播放状态；不会等待消息队列中的命令执行完毕 */
PlaybackState Player::GetState() const {
    return control_->GetState();
}

/** @brief 读取已缓存的媒体时长；未知或当前没有媒体时返回 -1，单位为微秒 */
std::int64_t Player::GetDurationUs() const {
    return control_->GetDurationUs();
}

/** @brief 读取当前时间轴的位置快照；没有时间轴时返回 0，单位为微秒 */
std::int64_t Player::GetPositionUs() const {
    return control_->GetPositionUs();
}

/**
 * @brief 初始化 Control 的状态、显示分发器和音频设备工厂
 *
 * 这里会验证不依赖媒体轨道的公共配置，但不会打开媒体或声卡若未传入自定义工厂，
 * 则准备默认 ALSA 工厂；只有探测到音频轨道、实际构建音频分支时才创建音频设备对象
 */
Player::Control::Control(PlayerConfig config, AudioSinkFactory audio_sink_factory)
    : video_sink_dispatcher_(std::make_shared<VideoSinkDispatcher>()),
      config_(config),
      audio_sink_factory_(std::move(audio_sink_factory)) {
    // 还不知道媒体有哪些轨道，所以先校验公共参数；队列容量和音频预算要等
    // BuildPipeline 探测轨道后再按需校验
    const auto config_validation = config_.ValidateCommon();
    if (!config_validation.ok_) {
        throw std::invalid_argument(config_validation.message_);
    }
    // 没有自定义工厂时使用默认 ALSA 工厂；纯视频媒体不会调用该工厂
    if (!audio_sink_factory_) {
        // getenv 在"变量存在但为空字符串"时返回 ""，而不是 nullptr。
        // 只判 nullptr 会把空串当设备名传给 ALSA，构造函数随即抛 invalid_argument
        // 这里把空串也当作"未设置"，回退到 default
        const char* configured_device_name = std::getenv("AI_MEDIA_PLAYER_ALSA_DEVICE");
        const bool device_name_is_set = configured_device_name != nullptr && *configured_device_name != '\0';
        const std::string alsa_device_name = device_name_is_set ? configured_device_name : "default";
        const int device_buffer_ms = config_.audio_.device_buffer_ms_;
        audio_sink_factory_ = [alsa_device_name, device_buffer_ms] {
            return CreateAlsaAudioSink(alsa_device_name, device_buffer_ms);
        };
    }
    LOG_INFO("Player control state constructed");
}

/** @brief 通过分发器绑定显示对象；可能等待此前的显示回调退出 */
void Player::Control::AttachDisplayView(IVideoSink* display_sink) {
    video_sink_dispatcher_->Attach(display_sink);
}

/** @brief 通过分发器解绑显示对象，并等待其在途回调结束 */
void Player::Control::DetachDisplayView(IVideoSink* display_sink) {
    video_sink_dispatcher_->Detach(display_sink);
}

/** @brief 读取原子发布的播放状态；不会处理消息队列中的命令 */
PlaybackState Player::Control::GetState() const {
    return state_.load(std::memory_order_acquire);
}

/** @brief 读取原子发布的媒体时长；未知时返回 -1 */
std::int64_t Player::Control::GetDurationUs() const {
    return media_duration_us_.load(std::memory_order_acquire);
}

/** @brief 把 Open 请求包装成消息，交给控制线程处理 */
void Player::Control::Open(const std::string& media_uri) {
    Message message;
    message.type_ = MsgType::kComOpen;
    message.uri_ = media_uri;
    Post(message);
}

/** @brief 把 Play 请求放入消息队列 */
void Player::Control::Play() {
    Post(Message(Player::Control::MsgType::kComPlay));
}

/** @brief 把 Pause 请求放入消息队列 */
void Player::Control::Pause() {
    Post(Message(MsgType::kComPause));
}

/** @brief 把 Stop 请求放入消息队列；不在调用线程中停止 Pipeline */
void Player::Control::Stop() {
    Post(Message(MsgType::kComStop));
}

/** @brief 把 Seek 请求及其目标时间放入消息队列；此处不修改 Pipeline 的 epoch */
void Player::Control::Seek(std::int64_t target_position_us) {
    Message message;
    message.type_ = MsgType::kComSeek;
    message.seek_target_us_ = target_position_us;
    Post(message);
}

/**
 * @brief 把消息放入队列并唤醒控制线程
 *
 * 可以从应用线程、OpenTask 线程或节点线程调用锁内只检查退出标志并完成入队，
 * 不在锁内执行消息处理或停止 Pipeline；否则停止时等待节点退出，节点又要 Post 事件，
 * 就可能形成死锁退出请求已经设置时，新消息会被丢弃
 */
void Player::Control::Post(Message message) {
    {
        std::lock_guard<std::mutex> lock(mailbox_mutex_);
        if (shutting_down_.load(std::memory_order_acquire))
            return;
        mailbox_.push(std::move(message));
    }

    mailbox_cv_.notify_all();
}

/**
 * @brief 控制线程主循环：等待、判定、每轮只执行一个动作
 * @details
 */
void Player::Control::ControlLoop() noexcept {
    std::optional<std::size_t> messages_to_drain_before_timeout;
    std::uint64_t timeout_pipeline_generation = 0;
    std::uint64_t timeout_seek_epoch = 0;

    while (true) {
        try {
            Message message;
            bool has_message = false;
            bool shutdown_requested = false;
            bool seek_timeout_reached = false;
            bool open_task_finished = false;

            {
                // 锁只保护队列操作；等待、取消息和检查退出条件都在这里完成。
                // 处理消息和停止 Pipeline 必须在解锁后进行，否则节点退出时 Post 事件会死锁。
                std::unique_lock<std::mutex> lock(mailbox_mutex_);
                auto next_wakeup_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
                const bool seeking = GetState() == PlaybackState::kSeeking;
                if (seeking) {
                    next_wakeup_time = std::min(next_wakeup_time, seek_deadline_);
                }

                // 无论当前是否 Seeking 都要等待和取出邮箱消息。旧实现把整个出队逻辑放在
                // `if (seeking)` 内，导致 Idle/Opening/Playing 状态永远不消费 Open/Play/Stop。
                mailbox_cv_.wait_until(
                    lock, next_wakeup_time, [this]() { return !mailbox_.empty() || shutting_down_.load(); });

                shutdown_requested = shutting_down_.load(std::memory_order_acquire);
                const bool seek_deadline_reached = !shutdown_requested && seeking && pipeline_ &&
                    std::chrono::steady_clock::now() >= seek_deadline_;

                if (!seek_deadline_reached) {
                    // 已不在超时判定中：丢弃上一轮留下的配额，下次超时重新快照。
                    messages_to_drain_before_timeout.reset();
                } else if (!messages_to_drain_before_timeout ||
                           timeout_pipeline_generation != pipeline_generation_ ||
                           timeout_seek_epoch != pipeline_->GetCurrentEpoch()) {
                    // 截取超时检查开始时的队列长度；后续新消息不能无限推迟超时处理。
                    messages_to_drain_before_timeout = mailbox_.size();
                    timeout_pipeline_generation = pipeline_generation_;
                    timeout_seek_epoch = pipeline_->GetCurrentEpoch();
                }

                // 只有超时前已排队的消息全部处理完，才执行一次 Seek 超时处理；
                // 新到达的消息不计入配额，避免它们无限推迟失败判定。
                seek_timeout_reached = seek_deadline_reached && messages_to_drain_before_timeout &&
                    *messages_to_drain_before_timeout == 0;

                // Seeking 超时后先处理超时前已经排队的消息；其他状态直接按 FIFO 取消息。
                // 配额为 0 时不进入此分支，因此不会对 size_t 做下溢递减。
                if (!shutdown_requested && !seek_timeout_reached && !mailbox_.empty()) {
                    message = std::move(mailbox_.front());
                    mailbox_.pop();
                    has_message = true;
                    if (messages_to_drain_before_timeout)
                        --*messages_to_drain_before_timeout;
                } else if (!shutdown_requested && !seek_timeout_reached) {
                    // 完成通知丢失时的兜底检查；有正常消息时让消息先按 FIFO 处理。
                    open_task_finished = open_task_ && open_task_->IsFinished();
                }
            }

            // 以下动作均在临界区外执行：HandleStop 会 Join 节点，不能持有 mailbox_mutex_，
            // 否则节点在回调里 Post 事件时会与停止流程互相等待。
            if (shutdown_requested) {
                // 退出请求是最高优先级：即使邮箱里还有消息也不再处理，避免析构被无限拖住
                HandleStop();
                break;
            }
            if (has_message) {
                ProcessMessage(message);
                continue;
            }
            if (seek_timeout_reached) {
                // 超时动作是"整图停到 kError"，而不是跳过未就绪的轨道强行播放：
                // 否则会留下只有画面没有声音、且位置与实际数据不一致的半可用状态。
                // HandleNodeError 内部会先 StopCurrentPipeline 再改状态，因此下一轮
                // seek_deadline_reached 不再成立，同一次超时不会反复触发。
                const auto seek_epoch = pipeline_->GetCurrentEpoch();
                LOG_ERROR("Player seek timed out after {} ms at epoch {}",
                          config_.sync_.seek_timeout_.count(),
                          seek_epoch);
                HandleNodeError(pipeline_generation_,
                                seek_epoch,
                                MediaError::FromInternal("dual seek", "timed out waiting for all tracks"),
                                "");
                continue;
            }
            if (open_task_finished) {
                // 完成通知没送达时的兜底路径；入参用当前任务自己的序号，内部还会再校验一次
                HandleOpenFinished(open_task_->GetSequence());
                continue;
            }
            // 四个动作都不满足：回到 wait_until 继续等待。
        } catch (const std::exception& exception) {
            // 任何异常都收敛为"停图 + 清意图 + kError"，不让异常逃出 noexcept 的线程入口。
            // 只有退出请求已经发出时才跳出循环，否则继续等待后续命令（例如重新 Open）。
            LOG_ERROR("Player control exception: {}", exception.what());
            HandleStop();
            play_after_open_ = resume_after_seek_ = false;
            SetState(PlaybackState::kError);
            if (shutting_down_.load(std::memory_order_acquire)) {
                break;
            }

        } catch (...) {
            LOG_ERROR("Player control exception: unknown exception");
            HandleStop();
            play_after_open_ = resume_after_seek_ = false;
            SetState(PlaybackState::kError);
            if (shutting_down_.load(std::memory_order_acquire)) {
                break;
            }
        }
    }
}
/** @brief 在控制线程中分发一条已经从队列取出的消息 */
void Player::Control::ProcessMessage(const Message& message) {
    switch (message.type_) {
        case MsgType::kComOpen:
            HandleOpen(message.uri_);
            break;
        case MsgType::kComPlay:
            HandlePlay();
            break;
        case MsgType::kComPause:
            HandlePause();
            break;
        case MsgType::kComStop:
            HandleStop();
            break;
        case MsgType::kComSeek:
            HandleSeek(message.seek_target_us_);
            break;
        case MsgType::kEvtOpenFinished:
            HandleOpenFinished(message.open_sequence_);
            break;
        case MsgType::kEvtMediaEnded:
            HandleMediaEnded(message.pipeline_generation_, message.seek_epoch_);
            break;
        case MsgType::kEvtNodeError:
            HandleNodeError(message.pipeline_generation_,
                            message.seek_epoch_,
                            message.node_media_error_,
                            message.node_error_message_);
            break;
        case MsgType::kEvtSeekCompleted:
            HandleSeekCompleted(message.pipeline_generation_,
                                message.seek_epoch_,
                                message.seek_actual_position_us_,
                                message.all_tracks_reached_eof_);
            break;
        case MsgType::kComShutdown:
            HandleStop();
            shutting_down_ = true;
            mailbox_cv_.notify_all();
            break;
    }
}

/**
 * @brief 处理新的 Open 请求
 *
 * 新 Open 会替换当前媒体：先停止旧打开任务和旧 Pipeline，再启动新的 OpenTask
 * OpenTask 只负责打开输入和探测流；完成后投递事件，由控制线程创建 Pipeline
 */
void Player::Control::HandleOpen(const std::string& uri) {
    LOG_INFO("Player opening media '{}' ", uri);

    // 正在运行的旧打开任务先请求中断并 Join，确保它不再访问自身成员
    if (open_task_ && open_task_->IsJoinable()) {
        open_task_->RequestStopOpen();
        open_task_->Join();
        open_task_.reset();
    }

    // 旧 Pipeline 停止并 Join 所有节点后，才可以释放其中的 Link 和媒体资源
    StopCurrentPipeline();
    resume_after_seek_ = false;
    play_after_open_ = false;

    SetState(PlaybackState::kOpening);
    const std::uint64_t open_sequence = ++next_open_sequence_;

    // 打开线程完成后只投递通知；Pipeline 的创建仍由控制线程完成
    open_task_ = std::make_unique<OpenTask>(uri, open_sequence, [this, open_sequence]() {
        Message message;
        message.type_ = MsgType::kEvtOpenFinished;
        message.open_sequence_ = open_sequence;
        Post(std::move(message));
    });
}

/**
 * @brief 消费指定 OpenTask 的结果，并决定是否构建和启动 Pipeline
 *
 * 只接受当前 Open 对应的完成事件Join 确保工作线程不再写入结果后，控制线程才能读取结果
 * 如果用户已经 Stop，则丢弃打开结果；否则构建 Pipeline，并应用打开期间收到的 Play 请求
 */
void Player::Control::HandleOpenFinished(std::uint64_t open_sequence) {
    // 完成通知必须匹配当前 OpenTask，旧任务的延迟通知不能启动新 Pipeline
    if (!open_task_ || open_task_->GetSequence() != open_sequence) {
        return;
    }

    // Join 后打开线程不再修改结果，当前线程可以安全读取
    open_task_->Join();
    open_task_->RethrowFailure();

    // 打开期间若收到 Stop，状态已不再是 kOpening，因此丢弃本次打开结果
    if (state_ != PlaybackState::kOpening) {
        play_after_open_ = false;
        open_task_.reset();
        return;
    }

    // 打开或流信息探测失败时，不创建 Pipeline
    if (open_task_->Error().HasError()) {
        LOG_ERROR("Player open failed: {}", open_task_->Error().Message());

        play_after_open_ = false;
        SetState(PlaybackState::kError);
        open_task_.reset();
        return;
    }

    FfmpegDemuxerOpenResult open_result = open_task_->TakeOpenResult();
    open_task_.reset();

    // 使用打开线程交付的解封装器和编解码参数组装 Pipeline
    if (!BuildPipeline(std::move(open_result))) {
        // Pipeline 组装或启动失败
        LOG_ERROR("Player failed to build pipeline");
        play_after_open_ = false;
        SetState(PlaybackState::kError);
        return;
    }

    // 应用打开期间收到的最后一次 Play/Pause 意图
    const bool should_start_playback = std::exchange(play_after_open_, false);
    if (should_start_playback) {
        LoadTimeline()->SetPlaying(true);
        SetState(PlaybackState::kPlaying);
        pipeline_->Resume();
    } else {
        SetState(PlaybackState::kPaused);
    }
}

/**
 * @brief 根据音视频轨道情况装配并启动 Pipeline
 * @details
 * 根据没事实际包含的轨道创建节点和 Link：视频分支负责解码、同步和显示；音频分支负责解码和设备输出
 * 所有连接成功后才启动 Pipeline
 * 音频设备对象由工厂函数创建并交给 AudioSinkNode，设备 Open 在节点线程中执行
 */
bool Player::Control::BuildPipeline(FfmpegDemuxerOpenResult open_result) {
    ++pipeline_generation_;

    // 1. 创建 pipeline
    auto new_pipeline = std::make_unique<Pipeline>(MakePipelineCallbacks(pipeline_generation_));

    // 2. 获取媒体信息
    const auto media_info = open_result.demuxer_->GetMediaInfo();
    const bool has_video_stream = media_info.video_stream_.has_value();
    const bool has_audio_stream = media_info.audio_stream_.has_value();

    // 3. 根据媒体实际包含的轨道校验配置，若没有音频也没有视频，就无法构建可播放的 Pipeline，报错返回
    const auto track_validation = config_.ValidateForTracks(has_video_stream, has_audio_stream);
    if (!track_validation.ok_) {
        LOG_ERROR("Player rejected media topology: {}", track_validation.message_);
        return false;
    }

    // 4. 获取共享时间轴
    auto playback_timeline =
        std::make_shared<PlaybackTimeline>(has_video_stream, has_audio_stream, config_.sync_.reanchor_timeout_);

    // 5. 是否支持 Seek
    can_attempt_seek_ = open_result.demuxer_->CanAttemptSeek();

    // 6. 获取可 Seek 的源节点：DemuxerNode
    auto& demuxer_node =
        new_pipeline->EmplaceNode<FfmpegDemuxerNode>(std::move(open_result.demuxer_), playback_timeline);
    new_pipeline->SetSeekSource(&demuxer_node);

    // 7. 视频分支：解封装 -> 视频解码 -> 音视频同步 -> 显示输出
    if (has_video_stream) {
        // 创建视频解复用输出队列
        auto video_packets_link = new_pipeline->CreateLink<StreamItem<Packet>>(config_.demux_.video_packet_capacity_);

        // 创建视频解码器输入队列
        auto decoded_video_frame_link =
            new_pipeline->CreateLink<StreamItem<VideoFrame>>(config_.decode_.video_frame_capacity_);

        // AvSyncNode 已决定视频帧的放行时机；此 Link 只保留一帧，避免同步之后再次积累延迟
        auto synced_video_frame_link = new_pipeline->CreateLink<StreamItem<VideoFrame>>(1);

        // 打开音频解码器
        auto video_decode_open_result = FfmpegVideoDecoder::Open(std::move(open_result.video_codec_parameters_),
                                                                 media_info.video_stream_->time_base_);

        if (!video_decode_open_result.Succeeded()) {
            LOG_ERROR("Video decoder open failed: {}", video_decode_open_result.error_.ToString());

            return false;
        }

        // 创建视频解码节点
        auto& video_decoder_node =
            new_pipeline->EmplaceNode<FfmpegVideoDecoderNode>(std::move(video_decode_open_result.video_decoder_));

        // 缺少有效帧率时按 25 fps（每帧约 40 ms）估算，仅用于缺失 PTS 和最后一帧结束时间的兜底
        const auto video_frame_rate = media_info.video_stream_->frame_rate_;

        const std::int64_t video_frame_duration_us = video_frame_rate.IsValid()
            ? std::max<std::int64_t>(1, 1000000 * video_frame_rate.denominator_ / video_frame_rate.numerator_)
            : 40000;

        // 创建音画同步节点
        auto& av_sync_node = new_pipeline->EmplaceNode<AvSyncNode>(
            playback_timeline, video_frame_duration_us, config_.sync_.late_drop_us_);

        // 创建音频分发节点
        auto& video_sink_node = new_pipeline->EmplaceNode<VideoSinkNode>(video_sink_dispatcher_, playback_timeline);

        if (!demuxer_node.ConnectVideoOutput(video_packets_link) ||
            !video_decoder_node.ConnectInput(video_packets_link) ||
            !video_decoder_node.ConnectOutput(decoded_video_frame_link) ||
            !av_sync_node.ConnectInput(decoded_video_frame_link) ||
            !av_sync_node.ConnectOutput(synced_video_frame_link) ||
            !video_sink_node.ConnectInput(synced_video_frame_link)) {
            LOG_ERROR("Player failed to connect the video branch");

            return false;
        }
    }

    // 8. 音频分支：解封装 -> 音频解码 -> 音频设备输出纯音频媒体只创建这一分支
    if (has_audio_stream) {
        // 创建音频解复用输出队列
        auto audio_packets_link = new_pipeline->CreateLink<StreamItem<Packet>>(config_.demux_.audio_packet_capacity_);

        // 创建音频解码器输入队列
        auto decoded_audio_frame_link =
            new_pipeline->CreateLink<StreamItem<PcmFrame>>(config_.decode_.audio_frame_capacity_);

        // 打开音频解码器
        auto audio_decoder_open_result = FfmpegAudioDecoder::Open(std::move(open_result.audio_codec_parameters_),
                                                                  media_info.audio_stream_->time_base_);

        if (!audio_decoder_open_result.Successed()) {
            LOG_ERROR("Audio decoder open failed: {}", audio_decoder_open_result.media_error_.ToString());

            return false;
        }

        // 创建音频解码节点
        auto& audio_decoder_node =
            new_pipeline->EmplaceNode<FfmpegAudioDecoderNode>(std::move(audio_decoder_open_result.audio_decoder_));

        // 在控制线程中创建 IAudioSink 对象；AudioSinkNode 接管对象后，会在自己的工作线程里调用 Open()，
        // 因此这里是"对象构造失败"的唯一出口，声卡真正打开失败则在节点运行阶段暴露。
        // 工厂可能抛异常（如 ALSA 参数非法），必须就地捕获并带上上下文，
        // 否则异常会一路冒到 ControlLoop，日志只能看到底层原因，看不出是哪个分支的哪一步失败的。
        std::unique_ptr<IAudioSink> audio_sink_device;
        try {
            audio_sink_device = audio_sink_factory_ ? audio_sink_factory_() : nullptr;
        } catch (const std::exception& error) {
            LOG_ERROR("Audio sink creation failed while building the audio branch: {}", error.what());
            return false;
        }

        if (!audio_sink_device) {
            LOG_ERROR("Audio sink factory returned no device; audio branch cannot be built");

            return false;
        }

        // 创建音频分发节点
        auto& audio_sink_node = new_pipeline->EmplaceNode<AudioSinkNode>(
            playback_timeline, std::move(audio_sink_device), config_.audio_.device_buffer_ms_);

        // 判断音频模块是否创建成功
        if (!demuxer_node.ConnectAudioOutput(audio_packets_link) ||
            !audio_decoder_node.ConnectInput(audio_packets_link) ||
            !audio_decoder_node.ConnectOutput(decoded_audio_frame_link) ||
            !audio_sink_node.ConnectInput(decoded_audio_frame_link)) {
            LOG_ERROR("Player failed to connect the audio branch");

            return false;
        }
    }

    // 所有节点和 Link 全部连接成功后，开始启动
    if (new_pipeline->Start() != PipelineStartResult::kStarted) {
        // 启动失败
        LOG_ERROR("Player Pipeline start failed");
        return false;
    }

    pipeline_ = std::move(new_pipeline);
    std::atomic_store(&timeline_, playback_timeline);
    media_duration_us_.store(media_info.duration_us_.value_or(-1), std::memory_order_release);

    LOG_INFO("Player Pipeline built: video {}, audio {}", has_video_stream, has_audio_stream);
    return true;
}

/**
 * @brief 处理 Play 请求
 * @details
 * 正在打开时记录打开后播放、正在 seeking 时记录 Seeking结束后继续播放、到达末尾的时候重播
 */
void Player::Control::HandlePlay() {
    LOG_INFO("Start playing");

    // 1. 打开尚未完成时，只记录播放意图
    if (GetState() == PlaybackState::kOpening) {
        play_after_open_ = true;

        return;
    }

    if (!pipeline_) {
        LOG_WARN("Player play ignored: no pipeline");
    }

    const PlaybackState playback_state = GetState();

    // 2. Seek 期间不直接恢复节点，只修改完成后的播放意图
    if (playback_state == PlaybackState::kSeeking) {
        resume_after_seek_ = true;

        return;
    }

    // 3. 媒体结束或所有输出轨道均到达末尾时，重播
    if (playback_state == PlaybackState::kEnded ||
        (((playback_state == PlaybackState::kPlaying) || playback_state == PlaybackState::kPaused) &&
         IsOutputAtEnd())) {
        LOG_INFO("Player replaying from start after EOF");
        if (!StartSeek(0, true)) {
            LOG_ERROR("Player replay seek was rejected");
        }

        return;
    }

    // 普通暂停状态下，先恢复时间轴，再放行 Pipeline 节点
    if (playback_state == PlaybackState::kPaused) {
        LoadTimeline()->SetPlaying(true);
        SetState(PlaybackState::kPlaying);
        pipeline_->Resume();
        return;
    }
}

/**
 * @brief 处理 Pause 请求
 * @details
 * 打开中或 Seek 中只修改后续的播放意图，正常播放时先暂停时间轴，再通知 Pipeline 的节点
 */
void Player::Control::HandlePause() {
    LOG_INFO("Player pause requested");

    const PlaybackState playback_state = GetState();

    // 1. 处于 open 阶段，清除自动播放意图，使打开后保持暂停
    if (playback_state == PlaybackState::kOpening) {
        play_after_open_ = false;
        return;
    }

    // 2. Seek 期间，只修改 Seek 后的播放意图，不立即暂停预滚节点
    if (playback_state == PlaybackState::kSeeking) {
        resume_after_seek_ = false;

        return;
    }

    // 3. 只有正在播放时才需要暂停时间轴和 Pipeline
    if (pipeline_ && playback_state == PlaybackState::kPlaying) {
        LoadTimeline()->SetPlaying(false);
        SetState(PlaybackState::kPaused);
        pipeline_->Pause();
    } else {
        LOG_WARN("Player pause ignored: not playing or no pipeline");
    }
}

/**
 * @brief 停止当前媒体源并回收相关资源
 * @details
 * 先清除自动播放/Seek 恢复意图，再停止并销毁 Pipeline，最后中断并 Join OpenTask
 */
void Player::Control::HandleStop() {
    LOG_INFO("Player stop requested");

    play_after_open_ = false;
    resume_after_seek_ = false;

    // 停止节点
    StopCurrentPipeline();

    // 无论打开是否已完成，都 Join 后再销毁 OpenTask，确保其工作线程已经退出
    if (open_task_) {
        open_task_->RequestStopOpen();
        open_task_->Join();
        open_task_.reset();
    }

    SetState(PlaybackState::kStopped);
}

/**
 * @brief 检查 Seek 请求是否能在当前状态执行，并确定完成后是否继续播放
 * @details
 * Seek 完成后的播放行为取决于请求到达时的状态：播放中继续，暂停或结束时保持暂停,连续 Seek
 * 不改变上一轮已经确定的完成后播放意图
 */
void Player::Control::HandleSeek(std::int64_t target_position_us) {
    // 1. pipeline 无效时直接返回
    if (!pipeline_) {
        LOG_WARN("Player seek to {} us ignored,: no pipeline", target_position_us);

        return;
    }

    // 2. // 负时间不是有效的媒体位置，直接拒绝
    if (target_position_us < 0) {
        LOG_WARN("Player seek to {} us ignored: negative target", target_position_us);

        return;
    }

    // 3. 根据当前播放状态确定 Seek 完成后的动作
    const PlaybackState current_playback_state = GetState();
    bool should_resume_after_seek;

    switch (current_playback_state) {
        case PlaybackState::kPlaying:
            should_resume_after_seek = true;
            break;
        case mediacore::PlaybackState::kPaused:
        case mediacore::PlaybackState::kEnded:
            should_resume_after_seek = false;
            break;
        case mediacore::PlaybackState::kSeeking:
            // 连续 Seek 沿用上一轮已记录的 "完成后是否播放" 选择
            should_resume_after_seek = resume_after_seek_;
            break;
        default:
            LOG_WARN(
                "Player seek to {} us ignored: state is {}", target_position_us, PlaybackStateToString(state_.load()));
            return;
    }

    // 4. 将定位请求交给 Pipeline
    if (!StartSeek(target_position_us, should_resume_after_seek)) {
        LOG_WARN("Player seek to {} us was rejected", target_position_us);
    }
}

/**
 * @brief 发起一次 Seek，并让相关节点处理新一轮的数据
 * @details
 * 先验证 Seek 能力并修正目标位置，再让时间轴和 Pipeline 进入新的 Seek 轮次
 * 即使此前处于暂停状态，也要临时放行节点，使其处理流重置和目标数据
 * 所有活动轨道都报告就绪后，HandleSeekCompleted 才决定恢复播放还是保持暂停
 */
bool Player::Control::StartSeek(std::int64_t target_position_us, bool should_resume_after_seek) {
    // 1. Pipeline 不存在时直接返回
    if (!pipeline_) {
        LOG_ERROR("Player seek rejected: no active Pipeline");
        return false;
    }

    // 2. 如果不支持 Seek
    if (!can_attempt_seek_ || !pipeline_->CanSeek()) {
        LOG_WARN("Player seek to {} us rejected: input does not support Seek", target_position_us);
        return false;
    }

    // 3. 已知媒体时长时，将超出末尾的目标限制到媒体末尾
    const auto media_duration_us = GetDurationUs();
    if (media_duration_us > 0) {
        target_position_us = std::min(target_position_us, media_duration_us);
    }

    // 4. 先生成新 epoch 并同步给时间轴，再由 Pipeline 请求 Demuxer 定位
    //    不能在 Seek 前读取 GetCurrentEpoch()：该值还是上一轮，Seek 内部才会递增
    //    也不能等 Seek 返回再更新时间轴：Demuxer 可能已经发出了新 epoch 的 Reset
    resume_after_seek_ = should_resume_after_seek;
    seek_deadline_ = std::chrono::steady_clock::now() + config_.sync_.seek_timeout_;
    SetState(PlaybackState::kSeeking);

    const auto timeline = LoadTimeline();
    const auto seek_epoch = pipeline_->Seek(target_position_us, [timeline, target_position_us](std::uint64_t epoch) {
        timeline->BeginSeek(epoch, target_position_us);
    });
    if (!seek_epoch.has_value()) {
        // 请求未被接受时清理本轮状态，不能继续等待完成事件。
        HandleStop();
        SetState(PlaybackState::kError);
        return false;
    }
    // 暂时放行节点，使暂停状态下的节点也能处理流重置、预滚数据和就绪通知
    pipeline_->Resume();

    SetState(PlaybackState::kSeeking);
    return true;
}

/**
 * @brief 处理所有活动轨道均已结束的通知
 * @details
 * 只处理来自当前 Pipeline、当前 Seek 轮次的事件正常播放期间收到 EOS 后暂停时间轴和节点
 * 并将状态设为 kEnded；保留 Pipeline，以便用户点击 Play 时从头重播
 */
void Player::Control::HandleMediaEnded(std::uint64_t event_pipeline_generation, std::uint64_t event_seek_epoch) {
    // 1. 旧 Pipeline 的延迟事件不能改变当前媒体状态
    if (!IsCurrentPipelineGen(event_pipeline_generation)) {
        return;
    }

    // 2. 当前 Pipeline 不存在或事件属于旧 Seek 轮次时，不处理该事件
    if (!pipeline_ || event_seek_epoch != pipeline_->GetCurrentEpoch()) {
        return;
    }

    // 3. 只有播放/暂停 状态才因普通 EOS 转为结束状态
    if (state_ == PlaybackState::kPlaying || state_ == PlaybackState::kPaused) {
        LOG_INFO("Player media ended, epoch {}", event_seek_epoch);
        LoadTimeline()->SetPlaying(false);
        pipeline_->Pause();
        SetState(PlaybackState::kEnded);
    }
}

/**
 * @brief 处理所有活动轨道都已报告 Seek 就绪的事件
 * @details
 * 忽略来自旧 Pipeline、旧 Seek 轮次或当前已不处于 Seeking 的事件，根据就绪结果判断是否已经到末尾
 * 先更新时间轴的 Seek 状态，再恢复或暂停节点，最后发布对应的 Player 状态
 */
void Player::Control::HandleSeekCompleted(std::uint64_t event_pipeline_generation,
                                          std::uint64_t event_seek_epoch,
                                          std::int64_t actual_position_us,
                                          bool all_tracks_reached_eof) {
    // 1. 如果 pipeline 不合法
    if (!IsCurrentPipelineGen(event_pipeline_generation) || !pipeline_ ||
        event_seek_epoch != pipeline_->GetCurrentEpoch() || GetState() != PlaybackState::kSeeking) {
        return;
    }

    // 2. 此事件由最后一个就绪的活动轨道上报，因此此时所有活动轨道都已完成 Seek 准备
    const auto playback_timeline = LoadTimeline();
    const bool is_at_end = all_tracks_reached_eof || playback_timeline->AtEnd();
    const bool should_resume_playback = resume_after_seek_ && !is_at_end;

    // actual_position_us 只表示轨道为 Seek 准备好的位置，不代表画面已经显示或音频已经播放；
    // 公共时间轴仍按用户请求的 target 结束 Seek，不把 actual_position_us 当成新的时钟起点
    playback_timeline->CompleteSeek(should_resume_playback);

    if (should_resume_playback) {
        pipeline_->Resume();
    } else {
        pipeline_->Pause();
    }

    SetState(is_at_end                    ? PlaybackState::kEnded
                 : should_resume_playback ? PlaybackState::kPlaying
                                          : PlaybackState::kPaused);

    resume_after_seek_ = false;

    LOG_INFO("Dual seek completed: epoch {}, selected position {} us, resume {}, at end {}",
             event_seek_epoch,
             actual_position_us,
             should_resume_playback,
             is_at_end);
}

/**
 * @brief 处理当前 Pipeline 的致命节点错误
 * @details
 * 只处理当前 Pipeline 的错误事件，即使事件的 Seek epoch 已经过期，也不能直接忽略
 * 当前 Pipeline 中的节点可能已经无法继续工作停止和 Join 由控制线程执行，避免节点在自己的回调栈中等待自身退出
 */
void Player::Control::HandleNodeError(std::uint64_t event_pipeline_generation,
                                      std::uint64_t event_seek_epoch,
                                      const MediaError& node_media_error,
                                      const std::string& node_error_message) {
    if (!pipeline_ || !IsCurrentPipelineGen(event_pipeline_generation)) {
        return;
    }

    // 当前 Pipeline 的节点错误需要停止整个 Pipeline；旧 epoch 不代表节点错误已经失效

    if (node_media_error.HasError()) {
        LOG_ERROR("Player received terminal node error at epoch {}: {}", event_seek_epoch, node_media_error.ToString());
    } else {
        LOG_ERROR("Player received terminal node error at epoch {}: {}", event_seek_epoch, node_error_message);
    }

    if (state_ != PlaybackState::kStopped) {
        StopCurrentPipeline();
        play_after_open_ = false;
        resume_after_seek_ = false;
        SetState(PlaybackState::kError);
    }
}

/** @brief 检查事件记录的 Pipeline 编号是否仍是当前编号 */
bool Player::Control::IsCurrentPipelineGen(std::uint64_t event_pipeline_generation) const noexcept {
    return event_pipeline_generation == pipeline_generation_;
}

/** @brief 查询时间轴是否认为所有实际存在的轨道都已结束 */
bool Player::Control::IsOutputAtEnd() const noexcept {
    const auto playback_timeline = LoadTimeline();
    return playback_timeline && playback_timeline->AtEnd();
}

/**
 * @brief 读取时间轴中的当前位置
 *
 * 先复制 shared_ptr，保证查询期间时间轴对象不会被销毁；时间轴内部负责提供线程安全快照
 * 返回值是媒体时间（微秒），不保证 GUI 已完成对应画面的绘制
 */
std::int64_t Player::Control::GetPositionUs() const {
    const auto playback_timeline = LoadTimeline();
    return playback_timeline ? playback_timeline->Get().media_position_us_ : 0;
}

/**
 * @brief 停止并销毁当前 Pipeline，撤销对外发布的时间轴和时长
 *
 * 先停止时间轴以唤醒可能正在等待它的线程，再调用 Pipeline::Stop() 让节点退出并 Join，
 * 确认节点不再使用资源后销毁 Pipeline最后清空公开的时间轴句柄和时长缓存
 * 本函数不处理中断/回收 OpenTask；那由 HandleStop() 或 HandleOpen() 完成
 */
void Player::Control::StopCurrentPipeline() {
    // 局部 shared_ptr 保证停止期间时间轴仍存活；其他查询线程持有的副本也不会因此失效
    const auto playback_timeline = LoadTimeline();
    if (playback_timeline) {
        playback_timeline->Stop();
    }

    if (pipeline_) {
        pipeline_->Stop();
        pipeline_.reset();
    }

    std::atomic_store(&timeline_, std::shared_ptr<PlaybackTimeline>{});

    media_duration_us_.store(-1, std::memory_order_release);
    can_attempt_seek_ = true;
}

/**
 * @brief 将 Pipeline 的回调转换成消息队列事件
 *
 * 这些回调在触发它们的节点线程中执行，不会自动切换到控制线程
 * 回调只提取事件数据并 Post；状态修改和 Pipeline 清理统一由 ControlLoop 执行
 * pipeline_generation 区分 Pipeline 实例，epoch 区分同一实例中的 Seek 轮次
 */
PipelineEventCallbacks Player::Control::MakePipelineCallbacks(std::uint64_t pipeline_generation) {
    return PipelineEventCallbacks{
        [this, pipeline_generation](MediaEnded media_ended_event) {
            Message message;
            message.type_ = MsgType::kEvtMediaEnded;
            message.seek_epoch_ = media_ended_event.epoch_;
            message.pipeline_generation_ = pipeline_generation;
            message.source_node_name_ = std::move(media_ended_event.node_name_);
            Post(std::move(message));
        },
        [this, pipeline_generation](NodeError node_error_event) {
            Message message;
            message.type_ = MsgType::kEvtNodeError;
            message.pipeline_generation_ = pipeline_generation;
            message.seek_epoch_ = node_error_event.epoch_;
            message.source_node_name_ = std::move(node_error_event.node_name_);
            message.node_error_message_ = std::move(node_error_event.message_);
            message.node_exception_ =
                std::move(node_error_event.exception_);  // 保留原始异常，必要时据此生成 MediaError
            message.node_media_error_ = std::move(node_error_event.media_error_);
            if (message.node_exception_ && !message.node_media_error_.HasError()) {
                try {
                    std::rethrow_exception(message.node_exception_);
                } catch (const std::exception& error) {
                    message.node_media_error_ = MediaError::FromInternal(message.source_node_name_, error.what());
                } catch (...) {
                    message.node_media_error_ =
                        MediaError::FromInternal(message.source_node_name_, "unknown worker exception");
                }
            }
            Post(message);
        },
        [this, pipeline_generation](SeekCompleted seek_completed_event) {
            Message message;
            message.type_ = MsgType::kEvtSeekCompleted;
            message.seek_epoch_ = seek_completed_event.epoch_;
            message.pipeline_generation_ = pipeline_generation;
            message.seek_actual_position_us_ = seek_completed_event.best_effort_position_us_;
            message.all_tracks_reached_eof_ = seek_completed_event.reached_eof_;
            Post(std::move(message));
        },
    };
}

}  // namespace mediacore

/**
 * @file player.cpp
 * @brief Player 的实现，控制线程根据消息队列中的消息处理命令或事件
 */

#include "core/player.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "core/inode.h"
#include "core/link.h"
#include "core/pipeline.h"
#include "demux/ffmpeg_demuxer_node.h"
#include "mediacore/ivideo_sink.h"
#include "mediacore/logger.h"
#include "mediacore/media_error.h"
#include "sink/video_sink_dispatcher.h"
#include "sink/video_sink_node.h"
#include "video_decoder/ffmpeg_video_decoder_node.h"

namespace mediacore {

/**
 * @brief 在后台使用独立的线程打开媒体
 */
OpenTask::OpenTask(std::string uri, std::uint64_t sequence, std::function<void()> on_finished)
    : uri_(std::move(uri)), sequence_(sequence) {
    // 打开媒体文件或者网络连接属于高耗时阻塞型 I/O 操作，必须放在独立后台线程运行
    open_thread_ = std::thread([this, on_finished = std::move(on_finished)] {
        open_result_ = FfmpegDemuxer::Open(uri_, interrupt_);
        media_error_ = open_result_.error_;

        if (on_finished) {
            on_finished();
        }
    });
}

/**
 * @brief 析构函数：请求取消正在执行的打开操作，并等待回收后台线程
 */
OpenTask::~OpenTask() {
    if (open_thread_.joinable()) {
        interrupt_.RequestStop();
        open_thread_.join();
    }
}

/**
 * @brief 创建播放器实例并启动控制处理线程
 */
Player::Player() : video_sink_dispatcher_(std::make_shared<VideoSinkDispatcher>()) {
    LOG_INFO("Player constructed, starting control thread");

    // &Player::ControlLoop() -> 对一个函数调用表达式的结果取地址
    control_thread_ = std::thread(&Player::ControlLoop, this);
}

/**
 * @brief 析构函数，关闭播放器，清理相关资源
 */
Player::~Player() {
    // 1. 发送关闭操作
    Post(Message(MsgType::kComShutdown));

    // 2. 等待线程回收
    if (control_thread_.joinable()) {
        control_thread_.join();
    }
}

/**
 * @brief 动态绑定视频渲染显示端
 */
void Player::AttachDisplayView(IVideoSink* sink) {
    video_sink_dispatcher_->Attach(sink);
}

/**
 * @brief 动态解绑视频渲染显示端
 */
void Player::DetachDisplayView(IVideoSink* sink) {
    video_sink_dispatcher_->Detach(sink);
}

/**
 * @brief 线程安全的获取当前的播放状态
 */
PlaybackState Player::GetState() const {
    return state_.load(std::memory_order_acquire);
}

/**
 * @brief 线程安全的获取媒体总时长
 */
std::int64_t Player::GetDurationUs() const {
    return durations_us_.load(std::memory_order_acquire);
}

/**
 * @brief 异步投递打开媒体命令
 */
void Player::Open(const std::string& url) {
    Message msg;
    msg.type_ = MsgType::kComOpen;
    msg.uri_ = url;
    Post(msg);
}

/**
 * @brief 异步投递播放媒体命令
 */
void Player::Play() {
    Post(Message(Player::MsgType::kComPlay));
}

/**
 * @brief 异步投递暂停命令
 */
void Player::Pause() {
    Post(Message(MsgType::kComPause));
}

/**
 * @brief 异步投递停止命令
 */
void Player::Stop() {
    Post(Message(MsgType::kComStop));
}

/**
 * @brief 异步投递Seek跳转命令
 */
void Player::Seek(std::int64_t target_us) {
    Message msg;
    msg.type_ = MsgType::kComSeek;
    msg.target_us_ = target_us;
    Post(msg);
}

/**
 * @brief 将控制命令或节点事件安全压入控制线程的邮箱队列
 */
void Player::Post(Message msg) {
    {
        std::lock_guard<std::mutex> lock(mailbox_mutex_);
        mailbox_.push(std::move(msg));
    }

    mailbox_cv_.notify_all();
}

/**
 * @brief 控制线程主循环：按 FIFO 顺序串行消费队列的中的命令或事件
 */
void Player::ControlLoop() noexcept {
    while (true) {
        Message msg;

        {
            std::unique_lock<std::mutex> lock(mailbox_mutex_);

            // 队列有数据或结束了，唤醒线程
            mailbox_cv_.wait(lock, [this]() { return !mailbox_.empty() || shutting_down_; });

            // 醒来后重新检查
            if (mailbox_.empty() && shutting_down_) {
                break;
            }

            msg = std::move(mailbox_.front());
            mailbox_.pop();
        }

        // 出队后立即在无锁环境下处理消息
        ProcessMessage(msg);
    }
}

/**
 * @brief 消息分发中心：将消息按照类型分发到具体的命令或事件处理函数
 * @param 要分发的消息
 */
void Player::ProcessMessage(const Message& msg) {
    switch (msg.type_) {
        case MsgType::kComOpen:
            HandleOpen(msg.uri_);
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
            HandleSeek(msg.target_us_);
            break;
        case MsgType::kEvtOpenFinished:
            HandleOpenFinished(msg.open_seq_);
            break;
        case MsgType::kEvtMediaEnded:
            HandleMediaEnded(msg.pipeline_gen_, msg.epoch_);
            break;
        case MsgType::kEvtNodeError:
            HandleNodeError(msg.pipeline_gen_, msg.epoch_, msg.media_error_, msg.error_msg_);
            break;
        case MsgType::kEvtSeekCompleted:
            HandleSeekCompleted(msg.pipeline_gen_, msg.epoch_, msg.target_us_, msg.is_reached_eof_);
            break;
        case MsgType::kComShutdown:
            HandleStop();
            shutting_down_ = true;
            mailbox_cv_.notify_all();
            break;
    }
}

/**
 * @brief 处理打开媒体命令：中断并销毁旧任务与旧的 Pipeline 管线，启动新的异步打开进程
 */
void Player::HandleOpen(const std::string& uri) {
    LOG_INFO("Player opening media '{}' ", uri);

    // 优先结束并回收可能正在执行的旧 OpenTask，防止两个打开任务争夺资源
    if (open_task_ && open_task_->IsRunning()) {
        open_task_->RequestStopOpen();
        open_task_->Join();
        open_task_.reset();
    }

    // 彻底停止并销毁前一个媒体的 Pipeline 拓扑
    StopCurrentPipeline();
    seek_resume_playing_ = false;
    play_when_opened_ = false;

    SetState(PlaybackState::kOpening);
    const std::uint64_t seq = ++next_open_sequence_;

    // 开始创建后台的 OpenTask，并绑定 Open 完成后向控制线程投递的 kEvtOpenFinished 事件回调
    open_task_ = std::make_unique<OpenTask>(uri, seq, [this, seq]() {
        Message msg;
        msg.type_ = MsgType::kEvtOpenFinished;
        msg.open_seq_ = seq;
        Post(std::move(msg));
    });
}

/**
 * @brief 处理打开完成事件：校验版本序号，构建 Pipeline 拓扑并确认初始播放状态
 * @param seq 本轮的序列号
 */
void Player::HandleOpenFinished(std::uint64_t seq) {
    // 1. 校验序号，避免在本次 Open 期间又发起了新的 Open
    if (!open_task_ || open_task_->GetSequence() != seq) {
        return;
    }

    // 2. 确保后台线程已经执行完毕
    open_task_->Join();

    // 3. 如果在打开期间外部触发了 Stop，则放弃后续的全部操作
    if (state_ != PlaybackState::kOpening) {
        play_when_opened_ = false;
        open_task_.reset();
        return;
    }

    // 4. 检查 解复用是否出问题
    if (open_task_->Error().HasError()) {
        LOG_ERROR("Player open failed: {}", open_task_->Error().Message());

        play_when_opened_ = false;
        SetState(PlaybackState::kError);
        open_task_.reset();
        return;
    }

    FfmpegDemuxerOpenResult open_result = open_task_->TakeOpenResult();
    open_task_.reset();

    // 5. 启动 Pipeline
    if (!BuildPipeline(std::move(open_result))) {
        // 启动失败
        LOG_ERROR("Player failed to build pipeline");
        play_when_opened_ = false;
        SetState(PlaybackState::kError);
        return;
    }

    // 6. 消费并设置打开期间是否播放标志
    const bool start_playing = std::exchange(play_when_opened_, false);
    if (start_playing) {
        SetState(PlaybackState::kPlaying);
        pipeline_->Resume();
    } else {
        SetState(PlaybackState::kPaused);
    }
}

/**
 * @brief 基于解复用的结果组建 Node 拓扑，连接 Link 并启动 Pipeline
 */
bool Player::BuildPipeline(FfmpegDemuxerOpenResult open_result) {
    // 1. 为新的 Pipeline 拓扑分配独立的序列编号
    ++pipeline_generation_;
    const std::uint64_t generation = pipeline_generation_;
    auto pipeline = std::make_unique<Pipeline>(MakePipelineCallbacks(generation));

    // 2. 创建有节阻塞队列 Link（Packet 深度 16, Frame 深度 4, 构建反向背压机制）
    auto packet_link = pipeline->CreateLink<StreamItem<Packet>>(16);
    auto frame_link = pipeline->CreateLink<StreamItem<VideoFrame>>(4);

    // 3. 移交解复用器所有权，创建解复用节点
    auto& source = pipeline->EmplaceNode<FfmpegDemuxerNode>(std::move(open_result.demuxer_));

    // 4. 检验视频流的有效性
    const MediaInfo media_info = source.GetMediaInfo();
    if (!media_info.video_stream_.has_value()) {
        return false;
    }

    // 5. 注册 Seek 数据源节点
    pipeline->SetSeekSource(&source);

    // 6. 使用获取到的流参数初始化视频流解码器上下文
    auto video_decoder_result =
        FfmpegVideoDecoder::Open(std::move(open_result.video_codec_parameters_), media_info.video_stream_->time_base_);

    // 如果 视频解码器初始化失败
    if (!video_decoder_result.Succeeded()) {
        if (video_decoder_result.error_.HasError()) {
            LOG_ERROR("Player failed to open video decoder: {}", video_decoder_result.error_.ToString());
        } else {
            LOG_ERROR("Player failed to open video decoder without a structured error");
        }

        return false;
    }

    // 7. 创建解码节点与渲染节点
    auto& video_decoder = pipeline->EmplaceNode<FfmpegVideoDecoderNode>(std::move(video_decoder_result.video_decoder_));

    auto& sink = pipeline->EmplaceNode<VideoSinkNode>(video_sink_dispatcher_);

    pipeline->SetSeekHoldReleasable(&sink);

    // 8. 搭建数据拓扑管道：Demuxer -> Link1 -> VideoDecoder -> Link2 -> Sink
    if (!source.ConnectOutput(packet_link) || !video_decoder.ConnectInput(packet_link) ||
        !video_decoder.ConnectOutput(frame_link) || !sink.ConnectInput(frame_link)) {
        return false;
    }

    // 9. 启动 Pipeline
    const PipelineStartResult start_result = pipeline->Start();
    if (start_result != PipelineStartResult::kStarted) {
        LOG_ERROR("Player pipeline start failed: {}", PipelineStartResultToString(start_result));

        return false;
    }

    LOG_INFO("Pipeline start successed");

    // 10. 启动后先进入暂停状态，等待 HandleOpenFinished 决定是否开始播放
    pipeline->Pause();
    pipeline_ = std::move(pipeline);
    video_sink_node_ = &sink;
    durations_us_.store(media_info.duration_us_.value_or(-1), std::memory_order_release);

    return true;
}

/**
 * @brief 处理播放请求
 */
void Player::HandlePlay() {
    LOG_INFO("Start playing");

    // 1. 场景 1：媒体尚在后台打开中，先记录自动播放意图
    if (GetState() == PlaybackState::kOpening) {
        play_when_opened_ = true;
        return;
    }

    if (!pipeline_) {
        LOG_WARN("Player play ignored: no pipeline");
        return;
    }

    const PlaybackState state = GetState();

    // 2. 场景 2：处于 Seek 寻帧阶段，记录目标帧到达后自动继续播放
    if (state == PlaybackState::kSeeking) {
        seek_resume_playing_ = true;
        return;
    }

    // 3. 场景 3：已到达流末尾（EOS 已触发或底层已消费完），点击 Play 触发从头重播
    if (state == PlaybackState::kEnded ||
        ((state == PlaybackState::kPlaying) || state == PlaybackState::kPaused) && IsOutputAtEnd()) {
        LOG_INFO("Player replaying from start after EOF");
        if (!StartSeek(0, true)) {
            LOG_ERROR("Player replay seek was rejected");
        }

        return;
    }

    // 4. 场景 4：常规暂停状态下恢复播放
    if (state == PlaybackState::kPaused) {
        SetState(PlaybackState::kPlaying);
        pipeline_->Resume();
        return;
    }
}

/**
 * @brief 处理暂停请求：挂起 Pipeline 数据流动并定格画面
 */
void Player::HandlePause() {
    LOG_INFO("Player pause requested");

    const PlaybackState state = GetState();

    // 1. 打开途中暂停：取消打开成功后的自动播放意图，但不会中断打开操作
    if (state == PlaybackState::kOpening) {
        play_when_opened_ = false;
        return;
    }

    // 2. Seek 时暂停：确保 Seek 目标帧交付后保持定格暂停
    if (state == PlaybackState::kSeeking) {
        seek_resume_playing_ = false;
        return;
    }

    // 3. 如果是播放中暂停
    if (pipeline_ && state == PlaybackState::kPlaying) {
        SetState(PlaybackState::kPaused);
        pipeline_->Pause();
    } else {
        LOG_WARN("Player pause ingored: not playing or pipeline");
    }
}

/**
 * @brief 处理停止请求：同步销毁管线并重置各种状态标记
 */
void Player::HandleStop() {
    LOG_INFO("Player stop requested");
    play_when_opened_ = false;
    seek_resume_playing_ = false;

    // 停止 pipeline
    StopCurrentPipeline();

    // 如果正处于打开媒体阶段
    if (open_task_) {
        open_task_->RequestStopOpen();
        open_task_->Join();
        open_task_.reset();
    }

    SetState(PlaybackState::kStopped);
}

/**
 * @brief 处理 Seek 请求：校验状态有效性并启动精准 Seek 跳转
 * @param target_us 要跳转的精确时间
 */
void Player::HandleSeek(std::int64_t targrt_us) {
    // 1. 校验 Pipleine 是否有效
    if (!pipeline_) {
        LOG_WARN("Player seek to {} us ingored: no pipeline", targrt_us);
        return;
    }

    // 2. 当前既不是播放状态，也不是暂停状态，也不是结束状态，就拒绝 Seek
    if (state_ != PlaybackState::kPlaying && state_ != PlaybackState::kPaused && state_ != PlaybackState::kEnded) {
        LOG_WARN("Player Seek to {} us ingored: state is {}", targrt_us, PlaybackStateToString(state_.load()));

        return;
    }

    // 3. 开始执行 Seek 操作，Seek 完成后是恢复播放还是保持暂停，取决于发起 Seek 瞬间的原始状态
    StartSeek(targrt_us, GetState() == PlaybackState::kPlaying);
}

/**
 * @brief 底层的 Seek 处理：触发新世代的流水线冲刷，并且在暂停的状态临时放行管线帧
 * @param target_us 要跳转的目标时间（微秒）
 * @param resume_playing seek 结束时是否继续播放
 */
bool Player::StartSeek(std::int64_t target_us, bool resume_playing) {
    // 1. 判断 pipeline 是否有效
    if (!pipeline_) {
        LOG_ERROR("pipeline is not valid");
        return false;
    }

    // 2. Pipeline 执行 Seek 请求
    seek_resume_playing_ = resume_playing;
    if (!pipeline_->Seek(target_us).has_value()) {
        seek_resume_playing_ = false;
        return false;
    }

    // 3. 如果播放器处于暂停或结束状态，必须先放行 pipeline 线程运转，才能解码并输出目标首帧
    if (state_ == PlaybackState::kPaused || state_ == PlaybackState::kEnded) {
        pipeline_->Resume();
    }

    SetState(PlaybackState::kSeeking);
    return true;
}

/**
 * @brief 处理底层节点上报的媒体流结束事件
 * @param gen 事件关联的 pipeline 版本号
 * @param epoch 事件关联的 seek 世代号
 */
void Player::HandleMediaEnded(std::uint64_t gen, std::uint64_t epoch) {
    // 1. 如果 pipeline 版本号不对
    if (!IsCurrentPipelineGen(gen)) {
        return;
    }

    // 2. 如果 pipeline 无效或 epoch 不对
    if (!pipeline_ || epoch != pipeline_->GetCurrentEpoch()) {
        return;
    }

    // 3. 如果处于播放阶段或暂停阶段
    if (state_ == PlaybackState::kPlaying || state_ == PlaybackState::kPaused) {
        LOG_INFO("Player media ended, epoch {}", epoch);
        SetState(PlaybackState::kEnded);
    }
}

/**
 * @brief 处理 Seek 目标帧交付事件，恢复管线后播放或者保持帧定格
 * @param gen 事件关联的 Pipeline 版本号
 * @param epoch 事件关联的 seek 版本号
 * @param actual_pos 实际交付到显示端的 pts
 * @param reached_eof 是否因目标帧越界触发 EOF 兜底帧交付到显示端
 */
void Player::HandleSeekCompleted(std::uint64_t gen, std::uint64_t epoch, std::int64_t actual_pos, bool reached_eof) {
    (void)actual_pos;
    (void)reached_eof;

    // 1. 如果不是当前的 pipeline
    if (!IsCurrentPipelineGen(gen)) {
        return;
    }

    // 2. 如果 pipeline 无效或 seek 版本号不对
    if (!pipeline_ || epoch != pipeline_->GetCurrentEpoch()) {
        return;
    }

    // 3. 如果播放器状态不处于 Seeking 状态
    if (state_ != PlaybackState::kSeeking) {
        return;
    }

    // 4. 必须在释放 Sink 节点的挂起锁前设定 Pipeline 状态，避免在状态决定前向显示端多吐后续帧
    if (seek_resume_playing_) {
        pipeline_->Resume();
        pipeline_->ReleaseSeekHold();
        SetState(PlaybackState::kPlaying);
    } else {
        pipeline_->Pause();
        pipeline_->ReleaseSeekHold();
        SetState(PlaybackState::kPaused);
    }

    seek_resume_playing_ = false;
    LOG_INFO("Player seek completed, epoch {}, resume_playing {}", epoch, state_ == PlaybackState::kPlaying);
}

/**
 * @brief 处理节点不可逆的错误：停止当前的 Pipeline 并流转至 Error 状态
 * @param gen 事件关联的 Pipeline 版本号
 * @param epoch 发生错误时的 Seek 世代号
 * @param media_error 结构化错误对象
 * @param message 文本错误详情
 */
void Player::HandleNodeError(std::uint64_t gen,
                             std::uint64_t epoch,
                             const MediaError& media_error,
                             const std::string& message) {
    if (!IsCurrentPipelineGen(gen)) {
        return;
    }

    if (!pipeline_ || epoch != pipeline_->GetCurrentEpoch()) {
        return;
    }

    if (media_error.HasError()) {
        LOG_ERROR("Player received termianl node error at epoch {}: {}", epoch, media_error.ToString());
    } else {
        LOG_ERROR("Player received termianl node error at epoch {}: {}", epoch, message);
    }

    if (state_ != PlaybackState::kStopped) {
        StopCurrentPipeline();
        play_when_opened_ = false;
        seek_resume_playing_ = false;
        SetState(PlaybackState::kError);
    }
}

/**
 * @brief 校验事件是否属于当前活跃的 Pipeline 实例
 */
bool Player::IsCurrentPipelineGen(std::uint64_t gen) const noexcept {
    return gen == pipeline_generation_;
}

/**
 * @brief 判断视频渲染输出端是否已经处理并到达流末尾
 */
bool Player::IsOutputAtEnd() const noexcept {
    return video_sink_node_ != nullptr && video_sink_node_->IsEndOfStreamReached();
}

/**
 * @brief 停止并销毁当前媒体的 Pipeline
 */
void Player::StopCurrentPipeline() {
    // video_sink_node_ 是 Player 的成员变量但是属于 pipeline 的一个节点
    // video_sink_dispatcher_ 不是节点
    video_sink_node_ = nullptr;
    if (pipeline_) {
        pipeline_->Stop();
        pipeline_.reset();
    }

    durations_us_.store(-1, std::memory_order_release);
}

/**
 * @brief 创建底层的 Pipeline 回调，将工作节点线程中的异步事件封装并投递至 Player 邮箱
 * @param generation 当前 pipeline 实例的版本号
 */
PipelineEventCallbacks Player::MakePipelineCallbacks(std::uint64_t generation) {
    return PipelineEventCallbacks{
        [this, generation](MediaEnded end) {
            Message msg;
            msg.type_ = MsgType::kEvtMediaEnded;
            msg.epoch_ = end.epoch_;
            msg.pipeline_gen_ = generation;
            msg.node_name_ = std::move(end.node_name_);
            Post(std::move(msg));
        },
        [this, generation](NodeError node_error) {
            Message msg;
            msg.type_ = MsgType::kEvtNodeError;
            msg.pipeline_gen_ = generation;
            msg.epoch_ = node_error.epoch_;
            msg.node_name_ = std::move(node_error.node_name_);
            msg.error_msg_ = std::move(node_error.message_);
            msg.exception_ = std::move(node_error.exception_);  // 可选的原始异常
            msg.media_error_ = std::move(node_error.media_error_);
            Post(msg);
        },
        [this, generation](SeekCompleted e) {
            Message msg;
            msg.type_ = MsgType::kEvtSeekCompleted;
            msg.epoch_ = e.epoch_;
            msg.pipeline_gen_ = generation;
            msg.target_us_ = e.best_effort_position_us_;
            msg.is_reached_eof_ = e.reached_eof_;
            Post(std::move(msg));
        },
    };
}

}  // namespace mediacore

/**
 * @file ffmpeg_demuxer_node.cpp
 * @brief FfmpegDemuxerNode 的读包、时间跳转（Seek）以及 EOF 挂起等待逻辑实现
 * @details
 * 作为 Pipeline 的数据源驱动节点，工作线程内部采用协作式循环驱动：
 * 1. 优先处理控制流：在每次读包前检查 HasPendingControl()，确保 Seek 请求具备高于普通数据读取的响应优先级；
 * 2. 数据流驱动：循环调用 FfmpegDemuxer::Read() 提取目标视频流 Packet，并通过 Link 阻塞推向下游；
 * 3. 流末尾挂起（EOF Parking）：遇到流末尾并向下游发送 EndOfStream 信号后，使用条件变量挂起自身工作线程，
 *    杜绝在文件末尾高频空转消耗 CPU，等待下一次 Seek 或 Stop 唤醒
 */

#include "demux/ffmpeg_demuxer_node.h"

#include <array>
#include <chrono>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "av_sync/playback_timeline.h"
#include "core/inode.h"
#include "mediacore/logger.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

namespace mediacore {

namespace {
constexpr const char* kNodeName = "Demuxer";

// 稳态发送与预滚发送用同样的等待上限：队列满时定期回到主循环，
// 以便检查 Stop、新的 Seek 和过期待发项
const auto kSeekSendWait = std::chrono::milliseconds(5);
const auto kSteadySendWait = std::chrono::milliseconds(5);

using PacketDigest = std::array<std::uint8_t, 32>;

/**
 * @class PacketHasher
 * @brief 使用 FFmpeg SHA-256 记录包前缀，内存不随预滚长度增长，仅由源线程使用
 */
class PacketHasher final {
public:
    /**
     * @brief 为当前一遍读取建立新的 SHAM-256 累积器
     */
    PacketHasher() : context_(av_sha_alloc()) {
        if (!context_) {
            throw std::bad_alloc();
        }
        // 分配成功才初始化摘要状态；原来这一步写在了 throw 分支里，永远执行不到。
        Reset();
    }
    ~PacketHasher() { av_free(context_); }

    /**
     * @brief 禁止拷贝构造和赋值
     */
    PacketHasher(const PacketHasher&) = delete;
    PacketHasher& operator=(const PacketHasher&) = delete;

    /**
     * @brief 丢弃上一包的摘要状态，开始计算下一包指纹
     */
    void Reset() { av_sha_init(context_, 256); }

    /**
     * @brief 把一段元数据或包体字节加入当前摘要
     */
    void Append(const std::uint8_t* bytes, std::size_t size) { av_sha_update(context_, bytes, size); }

    /**
     * @brief 以固定的小段字节序加入整数，避免不同平台的内存布局影响摘要
     */
    void AppendNumber(std::uint64_t value) {
        std::array<std::uint8_t, 8> bytes{};
        for (auto& byte : bytes) {
            byte = static_cast<std::uint8_t>(value);

            value >>= 8;
        }

        Append(bytes.data(), bytes.size());
    }

    /**
     * @brief 结束当前摘要并返回 32 字节 SHA-256 结果
     */
    PacketDigest Finish() {
        PacketDigest result{};
        av_sha_final(context_, result.data());
        return result;
    }

    /**
     * @brief 对一个压缩包的流号、时间戳、标志、包体和 side data 计算指纹
     * @details
     * 指纹只用于确认 Seek 重读时遇到的是已经交付过的 同一个包，不用于安全确认
     */
    PacketDigest Fingerprint(const Packet& packet) {
        Reset();

        const auto* raw = packet.GetAVPacket();
        AppendNumber(raw->stream_index);
        AppendNumber(raw->pts);
        AppendNumber(raw->dts);
        AppendNumber(raw->duration);
        AppendNumber(raw->flags);
        AppendNumber(raw->size);

        if (raw->size > 0) {
            Append(raw->data, static_cast<std::size_t>(raw->size));
        }

        AppendNumber(raw->side_data_elems);

        for (int i = 0; i < raw->side_data_elems; ++i) {
            AppendNumber(raw->side_data[i].type);
            AppendNumber(raw->side_data[i].size);

            if (raw->side_data[i].size > 0) {
                Append(raw->side_data[i].data, raw->side_data[i].size);
            }
        }

        return Finish();
    }

private:
    AVSHA* context_;
};

/**
 * @brief 一条轨道在本次 Seek 中"已经喂给解码器"的那串包，及其重读校验状
 * @details
 * Seek 分三遍读取：视频、音频、重读校验，第三遍读取会重新读到前两遍已经发送过的包，那些包需要被跳过
 * 交付时，把每个包的指纹(32字节)累加成一个摘要，用常数内存记下：送了 xx 个包，整批摘要 = x
 * 重读时同样累计，读满 12 个后比较两个摘要：
 * 一致 -> 这 12 个确实是旧包，跳过；不一致 -> 报错，绝不静默丢包
 */
struct PacketPrefix {
    /// 已交付侧（前两遍预滚时写入）
    PacketHasher delivered_hash_;  ///< 把每个已交付包的指纹依次累加进来
    std::uint64_t delivered_packet_count_{0};  ///< 已经送进解码器的包数
    PacketDigest delivered_prefix_digest_{};  ///< Seal 后冻结：上面那些包的整段摘要

    /// 重读侧（第三遍校验时写入）
    PacketHasher replay_hash_;  ///< 把重读到的包指纹依次累加进来
    std::uint64_t compared_packet_count_{0};  ///< 重读时已经比对过多少包，即比对进度
    bool prefix_verified_{false};  ///< 重读序列是否完整且与已交付的一致

    /**
     * @brief 一个包成功入队后调用：记录指纹并计数
     * @param digest 用 PacketHasher::Fingerprint() 算出的 32 字节指纹
     * @throw std::overflow_error 包数超出 uint64 范围（实际不可能发生，仅防御）
     */
    void RecordDeliveredPacket(const PacketDigest& digest) {
        if (delivered_packet_count_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Seek packet prefix is too long");
        }

        delivered_hash_.Append(digest.data(), digest.size());
        ++delivered_packet_count_;
    }

    /**
     * @brief 本遍预滚结束时调用：冻结已交付前缀的整段摘要
     * @details
     * 冻结后 delivered_hash 不再变化，第三遍只和这个值比较，一个包都没送时（例如目标已在末尾）直接标记为已验证
     */
    void FinalizeDeliveredPrefix() {
        delivered_prefix_digest_ = delivered_hash_.Finish();
        prefix_verified_ = delivered_packet_count_ == 0;
    }

    /**
     * @brief 第三遍重读时，逐个比对读到的包是否属于已交付前缀
     * @param digest 重读到包的指纹
     * @return
     * true 表示"仍是旧包，跳过不要送解码器"：目前还没读满前缀，或刚读满且摘要一致
     * false 表示"读满前缀后摘要对不上"：调用方必须报错，不能静默丢包
     */
    bool MatchesDeliveredPrefix(const PacketDigest& digest) {
        replay_hash_.Append(digest.data(), digest.size());

        ++compared_packet_count_;
        if (compared_packet_count_ == delivered_packet_count_) {
            prefix_verified_ = replay_hash_.Finish() == delivered_prefix_digest_;
        }

        return compared_packet_count_ < delivered_packet_count_ || prefix_verified_;
    }
};

}  // namespace

/**
 * @brief 一次双路 Seek 正在进行时的状态：先喂视频、再喂音频、最后重读校验
 * @details
 * 为什么分三遍：
 * 解复用时只有哦一个读游标，只能顺序往前读，视频要从关键帧解到目标时，通常需要几十上百个包，音频往往几个包
 * 如果一遍一直读下去同时输送 2 路包，那么任何一方的队列满了都会卡在 Send 上，另一方再也读不到数据
 * 所以每次只输送一种包，另一种包读出来直接丢弃，稍后重定位再读取
 */
struct FfmpegDemuxerNode::SeekPreroll {
    /** @brief 当前在第几遍 */
    enum class Phase : uint8_t {
        kVideo,  ///< 第一遍：只把视频包送进视频队列，音频包读完丢弃；等视频终端报告已到目标
        kAudio,  ///< 第二遍：重新定位，只把音频包送进音频队列，视频包读完丢弃；等音频终端报告已到目标
        kReplay  ///< 第三遍：重新定位，两路都读；已交付前缀内的包只校验不转发，之后的新包正常送出
    };

    /**
     * @brief 拷贝两份 POD 字段，不持有任何资源
     */
    explicit SeekPreroll(PendingSeek seek) : request_(seek) {}

    PendingSeek request_;  ///< 本轮的 epoch 与目标位置；三遍都读同一个目标，不随遍数改变
    Phase phase_{Phase::kVideo};  ///< 当前第几遍只有阶段推进时改变，普通读包不会动它

    PacketPrefix video_;  ///< 视频轨已交付的包序列（包数 + 滚动摘要），以及第三遍的校验进度
    PacketPrefix audio_;  ///< 音频轨已交付的包序列（包数 + 滚动摘要），以及第三遍的校验进度

    PacketHasher packet_hasher_;  ///< 计算当前包的指纹

    std::optional<PacketStreamItem>
        pending_;  ///< 队列 5 ms 内没空位时留下的唯一待发项（Packet 或 EOS），下一轮优先重试
    PacketDigest pending_digest_{};  ///< pending 是 Packet 时它的指纹；只有真正发送成功后才计入已交付前缀

    PlaybackTrack pending_track_{PlaybackTrack::kVideo};  ///< 当前应该发送到视频输出还是音频输出
    bool pass_eof_{false};  ///< 本遍从目标位置读到了文件末尾，下一轮先做收尾，不再读包
    bool video_eos_sent_{false};  ///< 本轮流程里视频队列是否已收到 EOS，防止同一条轨道重复发送
    bool audio_eos_sent_{false};  ///< 本轮流程里音频队列是否已收到 EOS，防止同一条轨道重复发送
};

/**
 * @brief 接管已打开的解复用器，并记录初始播放世代
 * @param sync_ctx 全局播放控制上下文（暂停、停止、epoch），由基类保存
 * @param event_callback 向 Pipeline 上报错误与状态事件的回调，由基类保存
 * @param demuxer 已完成 Open() 的解复用器；本节点接管所有权，必须非空
 * @param timeline 共享播放时间轴只有双路 Seek 预滚用它判断某条轨道是否已到目标，传空表示不做预滚
 * @throws std::invalid_argument 当 demuxer 为空时抛出异常
 */
FfmpegDemuxerNode::FfmpegDemuxerNode(std::shared_ptr<SyncContext> sync_ctx,
                                     PipelineEventCallbacks event_callback,
                                     std::unique_ptr<FfmpegDemuxer> demuxer,
                                     std::shared_ptr<PlaybackTimeline> timeline)
    : INodeBase(kNodeName, std::move(sync_ctx), std::move(event_callback)),
      demuxer_(std::move(demuxer)),
      timeline_(std::move(timeline)) {
    if (!demuxer_) {
        LOG_ERROR("FfmpegDemuxerNode requires an opened FfmpegDemuxer");
        throw std::invalid_argument("FfmpegDemuxerNode requires an opened FfmpegDemuxer");
    }

    // 本节点从构造后这一刻的世代开始输出；首次播放沿用这个编号，不需要额外的 Reset
    output_epoch_ = GetCurrentEpoch();
    LOG_INFO("FfmpegDemuxerNode constructed, active epoch {}", output_epoch_);
}

/**
 * @brief 析构函数
 */
FfmpegDemuxerNode::~FfmpegDemuxerNode() = default;

/**
 * @brief 把视频压缩包队列接入本节点
 * @param output Pipeline 创建的有界阻塞队列，本节点只保存 shared_ptr
 * @return 处于可配置阶段（Start 之前）且 output 非空时返回 true
 */
bool FfmpegDemuxerNode::ConnectVideoOutput(std::shared_ptr<OutputLink> output) {
    if (!CanConfigure() || !output) {
        return false;
    }

    video_output_ = std::move(output);
    return true;
}

/**
 * @brief 把视频压缩包队列接入本节点
 * @param output Pipeline 创建的有界阻塞队列，本节点只保存 shared_ptr
 * @return 处于可配置阶段（Start 之前）且 output 非空时返回 true
 */
bool FfmpegDemuxerNode::ConnectAudioOutput(std::shared_ptr<OutputLink> output) {
    if (!CanConfigure() || !output) {
        return false;
    }

    audio_output_ = std::move(output);
    return true;
}

/**
 * @brief 登记一条 Seek 请求，并打断当前可能正在阻塞的底层读取
 * @param epoch 本次 Seek 的新世代号，由 Pipeline::BeginNewEpoch() 产生
 * @param target_us 目标时间戳（微秒）
 * @note 本函数仅快速记录控制参数并发送中断，不会阻塞执行耗时的底层跳转
 */
void FfmpegDemuxerNode::RequestSeek(std::uint64_t epoch, std::int64_t target_us) {
    bool had_pending_seek = false;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);

        // 检查此前是否已有未消费的 Seek 请求（用于诊断高频连续拖动进度条场景）
        had_pending_seek = pending_seek_.has_value();

        // 覆盖式更新：同一时刻仅保留最新的单条 Seek 请求
        pending_seek_ = PendingSeek{epoch, target_us};

        // 协作式中断：促使正在底层阻塞的 FFmpeg Read/Seek 尽快放弃退出
        demuxer_->RequestSeekInterrupt();
    }

    if (had_pending_seek) {
        LOG_DEBUG("Demuxer replaced a pending seek with target {} us, epoch {}", target_us, epoch);
    } else {
        LOG_DEBUG("Demuxer queued seek to {} us, epoch {}", target_us, epoch);
    }

    // 通知基类状态机有控制事件到来，并唤醒可能阻塞在 EOF 处的解复用工作线程
    NotifyControlPending();
    eof_cv_.notify_all();
}

/**
 * @brief 工作线程每轮调一次：要么推进一小步预滚，要么读一个包，要么在末尾挂
 * @return kContinue 继续下一轮；kStopped 收到停止；kError 不可恢复的错误
 * @details
 * 四件事按固定优先级排列
 * 1. 存在的轨道必须有对应的队列，否则立即报错
 * 2. 正在预滚就先走预滚，普通读包完全让路
 * 3. 本世代已经读到末尾且发过 EOS，就挂起等 Seek 或 Stop
 * 4. 否则读一个包按 stream_index 分发
 */
TickResult FfmpegDemuxerNode::Tick() {
    // 1. 拓扑自检必须最先执行：后续任何一步都可能访问输出队列
    if ((demuxer_->GetVideoStreamIndex() >= 0 && !video_output_) ||
        (demuxer_->GetAudioStreamIndex() >= 0 && !audio_output_)) {
        return Fail("Every selected demux stream must have an output link", output_epoch_);
    }

    // 2. 丢弃跨越 Seek 的待发项：它的世代已经不是当前世代
    if (steady_pending_ && steady_pending_epoch_ != GetCurrentEpoch()) {
        LOG_DEBUG("Demuxer drops stale steady pending item for epoch {}", steady_pending_epoch_);

        steady_pending_.reset();
    }

    // 3. 优先重试待发项；无论成功还是超时都返回，保持"一轮只做一个动作"
    if (steady_pending_) {
        auto& output = steady_pending_track_ == PlaybackTrack::kVideo ? video_output_ : audio_output_;

        switch (output->SendFor(*steady_pending_, kSteadySendWait)) {
            case QueuePushResult::kTimedOut:
                // 队列仍然满；下一轮再试，期间可以响应 Stop 与新的 Seek
                return TickResult::kContinue;

            case QueuePushResult::kAborted:
                if (IsStopRequested()) {
                    return TickResult::kStopped;
                }
                return Fail("Demuxer output link aborted while sending a steady item", steady_pending_epoch_);

            case QueuePushResult::kPushed:
                OnSteadyPendingPushed();
                steady_pending_.reset();
                return TickResult::kContinue;
        }
    }

    // 4. 预滚优先于普通读包
    if (seek_preroll_) {
        return TickSeekPreroll();
    }

    // 5. 本世代已读完并发出 EOS：挂起等待 Seek 或 Stop
    if (eos_sent_) {
        return WaitAtEof();
    }

    // 6. 读之前先看有没有新的 Seek，有就本轮不读取，让基类去处理
    std::uint64_t read_epoch = 0;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);

        if (pending_seek_.has_value()) {
            return TickResult::kContinue;
        }

        demuxer_->PrepareNextIoForSeekInterrupt();
        read_epoch = output_epoch_;
    }

    // 7. 读一个包
    Packet packet;
    FfmpegReadResult read_result = demuxer_->Read(packet);

    switch (read_result.status_) {
        case mediacore::FfmpegReadStatus::kPacket: {
            if (read_epoch != GetCurrentEpoch()) {
                return TickResult::kContinue;
            }

            packet.SetEpoch(read_epoch);

            steady_pending_track_ = packet.GetStreamIndex() == demuxer_->GetVideoStreamIndex() ? PlaybackTrack::kVideo
                                                                                               : PlaybackTrack::kAudio;
            steady_pending_epoch_ = read_epoch;
            steady_pending_ = PacketStreamItem{std::in_place_type<Packet>, std::move(packet)};

            // 本轮只负责读；真正的发送留到下一轮的第 3 步
            return TickResult::kContinue;
        }

        case mediacore::FfmpegReadStatus::kSkipped:
            return TickResult::kContinue;

        case mediacore::FfmpegReadStatus::kSeekInterrupted:
            LOG_DEBUG("Demuxer read for epoch {} was interrupted by a newer seek", read_epoch);
            return TickResult::kContinue;

        case mediacore::FfmpegReadStatus::kStopInterrupted:
            LOG_INFO("Demuxer read for epoch {} was interrupted by stop", read_epoch);
            return TickResult::kStopped;

        case mediacore::FfmpegReadStatus::kEndOfStream: {
            if (read_epoch != GetCurrentEpoch()) {
                LOG_DEBUG("Ignoring stale EndOfStream for epoch {}", read_epoch);
                return TickResult::kContinue;
            }

            const bool need_video = video_output_ && !video_eos_sent_;
            const bool need_audio = audio_output_ && !audio_eos_sent_;

            if (need_video || need_audio) {
                // 两条队列的 EOS 逐个投递，同样走待发机制，避免阻塞
                steady_pending_track_ = need_video ? PlaybackTrack::kVideo : PlaybackTrack::kAudio;
                steady_pending_epoch_ = read_epoch;
                steady_pending_ = PacketStreamItem{EndOfStream{read_epoch}};
                return TickResult::kContinue;
            }

            eos_sent_ = true;
            LOG_INFO("Demuxer reached end of stream for epoch {}; waiting for seek or stop", read_epoch);
            return TickResult::kContinue;
        }

        case mediacore::FfmpegReadStatus::kError:
            if (read_epoch != GetCurrentEpoch()) {
                LOG_DEBUG("Ignoring stale Demuxer read error for epoch {}", read_epoch);
                return TickResult::kContinue;
            }
            LOG_ERROR("Demuxer read error for epoch {}: {}", read_epoch, read_result.error_.Message());
            return Fail(read_result.error_, read_epoch);
    }

    LOG_ERROR("FfmpegDemuxer returned an unknown read status for epoch {}", read_epoch);
    return Fail("FfmpegDemuxer returned an unknown read status", read_epoch);
}

/**
 * @brief 停止流程调用：打断正在进行的 FFmpeg I/O，并叫醒挂在末尾等待的线程
 * @details
 * 基类在 SignalStop 之后，Join 之前调用本函数，工作线程此时可能停在两种等待上：
 * 1. 阻塞在 av_read_frame/av_seek_frame 里
 * 2. 或停在 WaitAtEof 的条件变量上
 */
void FfmpegDemuxerNode::OnInterruptBlockingIo() noexcept {
    LOG_DEBUG("Demuxer node interrupting FFmpeg I/O and EOF wait for stop");
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        demuxer_->RequestStop();
    }

    // 唤醒可能正处于 WaitAtEof() 挂起状态的工作线程
    eof_cv_.notify_all();
}

/**
 * @brief 检查是否存在外部提交但尚未被执行的 Seek 控制请求
 * @return 存在待处理的请求时返回 true
 */
bool FfmpegDemuxerNode::HasPendingControl() const noexcept {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return pending_seek_.has_value();
}

/**
 * @brief 取走暂存的 Seek 请求并执行
 * @note 必须在工作线程执行 Tick 前优先调用，确保控制流优先于数据流
 */
TickResult FfmpegDemuxerNode::ProcessPendingControl() {
    PendingSeek request;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);

        if (!pending_seek_) {
            return TickResult::kContinue;
        }

        // 取走请求并清空暂存槽，同一个请求不会被处理两次
        request = pending_seek_.value();
        pending_seek_.reset();

        // 确认本轮 Seek 已经正式接管执行，重置中断比较基准
        demuxer_->PrepareNextIoForSeekInterrupt();
    }

    // 执行耗时的底层 Seek 时必须在锁外进行，避免阻塞外部线程继续登记新的 Seek
    return ExecuteSeek(request);
}

/**
 * @brief 读到文件末尾后挂起工作线程，等下一次 Seek 或 Stop 再醒
 * @return 被新 Seek 唤醒时返回 kContinue；被 Stop 唤醒时返回 kStopped
 * @details
 * 不挂起的话，工作线程会不停调用 Read，把 CPU 迟满
 */
TickResult FfmpegDemuxerNode::WaitAtEof() {
    std::unique_lock<std::mutex> lock(control_mutex_);

    // 进入睡眠前先置标志，外部才能观察到"已停在末尾"
    eof_parked_.store(true, std::memory_order_release);
    LOG_DEBUG("Demuxer is waiting at EOF for a seek or stop request");

    // 挂起等待外部触发 Stop 或提交新的 Seek
    eof_cv_.wait(lock, [this]() { return IsStopRequested() || pending_seek_.has_value(); });

    const bool stop_requested = IsStopRequested();

    // 醒来即清除标志，后续查询会看到"已恢复活动"
    eof_parked_.store(false, std::memory_order_release);
    lock.unlock();

    if (stop_requested) {
        LOG_DEBUG("Demuxer left EOF wait because stop was requested");
        return TickResult::kStopped;
    }

    LOG_DEBUG("Demuxer left EOF wait to process a seek request");
    return TickResult::kContinue;
}

/**
 * @brief 执行一次 Seek：定位底层读取位置，向下游广播 StreamReset，必要时启动双路预滚
 * @param request 目标位置与本次 Seek 的世代号
 * @return kContinue 成功或已作废；kStopped 被停止打断；kError 定位失败
 * @details
 * 处理顺序：
 * 1. 先丢弃上一轮残留的预滚状态
 * 2. 调 av_seek_frame 定位
 * 3. 如果这期间又有更新的 Seek，本次结果作废，直接返回
 * 4. 确认生效后才更新 output_epoch_ 并清 eos_sent_
 * 5. 先给两个队列各自发一个 StreamReset，再发送数据，保证解码器先清理掉上一世代的残留
 * 6. 双路都在时建立预滚状态，由 TickSeekPreroll 接管后续读取
 */
TickResult FfmpegDemuxerNode::ExecuteSeek(PendingSeek request) {
    LOG_INFO("Demuxer executing seek to {} us, epoch {}", request.target_us_, request.epoch_);

    // 1. 先丢弃上一轮残留的预滚状态
    seek_preroll_.reset();

    // 2. 调 av_seek_frame 定位
    const FfmpegSeekResult seek_result = demuxer_->Seek(request.target_us_);

    // 3. 如果这期间又有更新的 Seek，本次结果作废，直接返回
    if (seek_result.status_ == FfmpegSeekStatus::kSeekInterrupted) {
        LOG_DEBUG("Demuxer seek for epoch {} was superseded", request.epoch_);
        return TickResult::kContinue;
    }

    // 4. 如果被停止了
    if (seek_result.status_ == FfmpegSeekStatus::kStopInterrupted) {
        LOG_DEBUG("Demuxer seek for epoch {} was interrupted by stop", request.epoch_);
        return TickResult::kStopped;
    }

    // 5. 若执行失败
    if (seek_result.status_ != FfmpegSeekStatus::kSucceeded) {
        if (request.epoch_ != GetCurrentEpoch()) {
            LOG_DEBUG("Ignoring failed superseded seek for epoch {}", request.epoch_);
            return TickResult::kContinue;
        }
        LOG_ERROR("Demuxer seek failed: {}", seek_result.error_.Message());
        return Fail(seek_result.error_, request.epoch_);
    }

    // 6. 如果 Seek 成功返回期间又有新的 Seek 发生
    if (request.epoch_ != GetCurrentEpoch()) {
        LOG_DEBUG("Ignoring successful seek because a newer seek arrived for epoch {}", request.epoch_);
        return TickResult::kContinue;
    }

    // 7. 本次 Seek 成功：更新世代号，并重置 EOF 标记
    output_epoch_ = request.epoch_;
    eos_sent_ = false;
    video_eos_sent_ = false;
    audio_eos_sent_ = false;
    steady_pending_.reset();

    // 8. 两条 FIFO 队列先各自收到 Reset，再接收新包；改为限时发送，
    //    发送过程中仍可响应 Stop 与更新的 Seek
    const TickResult reset_result = SendSeekReset(request);
    if (reset_result != TickResult::kContinue) {
        return reset_result;
    }

    // SendSeekReset 用 kContinue 同时表示"两条队列都发完了"和"被更新的 Seek 取代"，
    // 后者会在这里再校验一次世代，避免为已经过期的 request 建立预滚对象
    if (request.epoch_ != GetCurrentEpoch()) {
        LOG_DEBUG("Demuxer skips preroll setup for superseded epoch {}", request.epoch_);
        return TickResult::kContinue;
    }

    // 9. 双路都有输出时才建立预滚状态
    if (timeline_ && video_output_ && audio_output_) {
        seek_preroll_ = std::make_unique<SeekPreroll>(request);
        LOG_INFO("Demuxer starting bounded video/audio seek preroll at epoch {}", request.epoch_);
    }

    if (seek_result.best_effort_position_us_.has_value()) {
        LOG_INFO("Demuxer completed seek for epoch {}; target {} us, demuxer-reported position {} us",
                 request.epoch_,
                 request.target_us_,
                 *seek_result.best_effort_position_us_);
    } else {
        LOG_INFO("Demuxer completed seek for epoch {}; target {} us; "
                 "actual presentation position will be determined by decoded output",
                 request.epoch_,
                 request.target_us_);
    }

    return TickResult::kContinue;
}

/**
 * @brief 向两条输出队列发送 StreamReset，等待过程中仍可响应 Stop 与更新的 Seek
 * @return kContinue 全部发送完成或被更新的 Seek 取代；kStopped 收到停止；kError 队列终止
 */
TickResult FfmpegDemuxerNode::SendSeekReset(const PendingSeek& request) {
    for (const auto& output : {video_output_, audio_output_}) {
        if (!output) {
            continue;
        }

        PacketStreamItem item{StreamReset{request.epoch_, request.target_us_}};

        for (;;) {

            // 1. 如果停止了直接返回
            if (IsStopRequested()) {
                return TickResult::kStopped;
            }

            // 2. 如果不是当前世代
            if (request.epoch_ != GetCurrentEpoch()) {
                LOG_DEBUG("Demuxer abandoned StreamReset for superseded epoch {}", request.epoch_);
                return TickResult::kContinue;
            }

            // 3. 非阻塞发送
            switch (output->SendFor(item, kSeekSendWait)) {   // 已有常量，5 ms
                case QueuePushResult::kPushed:
                    break;

                case QueuePushResult::kTimedOut:
                    continue;

                case QueuePushResult::kAborted:
                    return IsStopRequested()
                        ? TickResult::kStopped
                        : Fail("Demuxer output link aborted while sending StreamReset", request.epoch_);
            }

            break;   // kPushed 跳到下一条队列
        }
    }

    return TickResult::kContinue;
}


/**
 * @brief 将 FFmpeg 读取位置重新定位到本次 Seek 目标，开始下一阶段预滚
 * @details
 * 本函数只用于同一次 Seek 内部的阶段切换，不是新的用户 Seek
 * 它不会生成新的 epoch，不会发送 StreamReset，也不会清空解码器状态
 * 调用者在视频预滚完成后调用它进入音频预滚，在音频预滚完成后调用它进入最终重读校验
 *
 * control_mutex_ 只用于检查是否有更新的 Seek 请求并建立 I/O 中断基准
 * 真正可能阻塞的 demuxer_->Seek() 在解锁后执行，这样控制线程仍然可以提交新的 Seek
 * @return
 *   kContinue：可以继续下一阶段；
 *   kStopped：Seek 被停止请求中断；
 *   kError：内部重新定位失败
 */
TickResult FfmpegDemuxerNode::RestartSeekPass() {
    auto& seek = *seek_preroll_;
    const char* const pass_name = seek.phase_ == SeekPreroll::Phase::kAudio ? "audio preroll"
        : seek.phase_ == SeekPreroll::Phase::kReplay                        ? "verified replay"
                                                                            : "video preroll";
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (pending_seek_ || !IsCurrentEpoch(seek.request_.epoch_)) {
            LOG_DEBUG("Demuxer abandoned {} pass for epoch {}: new seek pending {}, epoch still current {}",
                      pass_name,
                      seek.request_.epoch_,
                      pending_seek_.has_value(),
                      IsCurrentEpoch(seek.request_.epoch_));
            return TickResult::kContinue;
        }

        // 先占位再解锁：之后到达的 Seek 才会让下面这次定位返回 kSeekInterrupted。
        // 这一行原来写在 return 之后，永远不会执行。
        demuxer_->PrepareNextIoForSeekInterrupt();
    }

    LOG_DEBUG(
        "Demuxer relocating to {} us for {} (epoch {})", seek.request_.target_us_, pass_name, seek.request_.epoch_);

    // 解锁后再进入 FFmpeg：Seek 期间外部仍可提交更新的目标
    const auto result = demuxer_->Seek(seek.request_.target_us_);

    if (result.status_ == FfmpegSeekStatus::kStopInterrupted) {
        LOG_DEBUG("Demuxer {} relocation for epoch {} interrupted by stop", pass_name, seek.request_.epoch_);
        return TickResult::kStopped;
    }

    if (result.status_ == FfmpegSeekStatus::kSeekInterrupted || !IsCurrentEpoch(seek.request_.epoch_)) {
        LOG_DEBUG(
            "Demuxer {} relocation for epoch {} abandoned: a newer seek arrived", pass_name, seek.request_.epoch_);
        return TickResult::kContinue;
    }

    if (!result.IsSucceeded()) {
        LOG_ERROR(
            "Demuxer {} relocation failed for epoch {}: {}", pass_name, seek.request_.epoch_, result.error_.Message());
        return Fail(result.error_, seek.request_.epoch_);
    }

    // 新一遍从目标位置重新读：上一遍因队列满留下的那个待发包，已经不属于当前位置了
    seek.pending_.reset();
    seek.pass_eof_ = false;
    LOG_INFO("Demuxer seek epoch {} entered {} pass; delivered prefixes video {}, audio {}",
             seek.request_.epoch_,
             seek.phase_ == SeekPreroll::Phase::kAudio ? "audio preroll" : "verified replay",
             seek.video_.delivered_packet_count_,
             seek.audio_.delivered_packet_count_);
    return TickResult::kContinue;
}

/**
 * @brief 稳态待发项成功入队后的记账
 * @details 只有 EndOfStream 需要额外处理：标记对应轨道已发送，并在两路都发完后置 eos_sent_
 */
void FfmpegDemuxerNode::OnSteadyPendingPushed() {
    if (!std::holds_alternative<EndOfStream>(*steady_pending_)) {
        return;
    }

    if (steady_pending_track_ == PlaybackTrack::kVideo) {
        video_eos_sent_ = true;
    } else {
        audio_eos_sent_ = true;
    }

    const bool video_done = !video_output_ || video_eos_sent_;
    const bool audio_done = !audio_output_ || audio_eos_sent_;

    if (video_done && audio_done) {
        eos_sent_ = true;
    }

    LOG_INFO("Demuxer sent EndOfStream to {} queue at epoch {}",
             steady_pending_track_ == PlaybackTrack::kVideo ? "video" : "audio",
             steady_pending_epoch_);
}

/**
 * @brief Seek 后分阶段向音频和视频解码器提供数据，使两条轨道都准备到目标位置
 * @details
 * 本函数只由 Demuxer 工作线程调用每次调用最多推进一个动作，返回后由主循环再次调用
 *
 * kVideo：只发送视频包，直到视频输出节点找到目标画面，音频包暂时丢弃，并记录已经发送的视频包摘要
 * kAudio：重新定位到同一目标位置，只发送音频包，直到音频输出节点准备好目标音频，视频包暂时丢弃，并记录已经发送的音频包摘要
 * kReplay：再次定位到目标位置，同事读取两条轨道，对前两个阶段已经发送过的包进行校验并跳过
 * 校验完成后将新的包分别发送到音频和视频
 *
 * pending 保存一个暂时无法入队的 Packet 或 EndOfStream，队列发送最多等待 kSeekSendWait，
 * 超时后保留 pending，下一轮重新检查停止状态、epoch 和轨道就绪状态
 * @return
 *   kContinue：预滚仍需继续，或本轮已等待/丢弃/发送一个数据项；
 *   kStopped：收到停止请求；
 *   kError：Seek、队列或包前缀校验失败

 */
TickResult FfmpegDemuxerNode::TickSeekPreroll() {
    // 推进一次 Seek 预滚：优先处理阶段切换和待发数据，最后最多读取一个包
    // 队列发送采用限时等待，以便及时响应 Stop 和新的 Seek
    auto& seek = *seek_preroll_;
    const auto epoch = seek.request_.epoch_;

    // Stop 和过期的 Seek 都不再继续处理
    if (IsStopRequested())
        return TickResult::kStopped;

    if (!IsCurrentEpoch(epoch))
        return TickResult::kContinue;

    // 下游已经找到目标位置时，立即结束当前阶段并重新定位读取位置
    if (seek.phase_ == SeekPreroll::Phase::kVideo && timeline_->IsTrackReady(PlaybackTrack::kVideo, epoch)) {
        seek.video_.FinalizeDeliveredPrefix();
        LOG_INFO("Demuxer video preroll done at epoch {}: {} packets delivered, moving to audio preroll",
                 epoch,
                 seek.video_.delivered_packet_count_);
        seek.phase_ = SeekPreroll::Phase::kAudio;
        return RestartSeekPass();
    }

    if (seek.phase_ == SeekPreroll::Phase::kAudio && timeline_->IsTrackReady(PlaybackTrack::kAudio, epoch)) {
        seek.audio_.FinalizeDeliveredPrefix();
        LOG_INFO("Demuxer audio preroll done at epoch {}: {} packets delivered, moving to verified replay",
                 epoch,
                 seek.audio_.delivered_packet_count_);
        seek.phase_ = SeekPreroll::Phase::kReplay;
        return RestartSeekPass();
    }

    // 上一轮未能入队的数据优先重试，避免后续数据越过它
    if (seek.pending_) {
        auto& output = seek.pending_track_ == PlaybackTrack::kVideo ? video_output_ : audio_output_;
        const bool packet = std::holds_alternative<Packet>(*seek.pending_);

        switch (output->SendFor(*seek.pending_, kSeekSendWait)) {
            case QueuePushResult::kTimedOut:
                // 队列已满，保留 pending，下轮继续尝试
                LOG_DEBUG("Demuxer {} queue full at epoch {}, holding pending {} for retry",
                          seek.pending_track_ == PlaybackTrack::kVideo ? "video" : "audio",
                          epoch,
                          packet ? "packet" : "EOS");
                return TickResult::kContinue;

            case QueuePushResult::kAborted:
                LOG_DEBUG("Demuxer {} queue aborted while sending pending data at epoch {}",
                          seek.pending_track_ == PlaybackTrack::kVideo ? "video" : "audio",
                          epoch);
                return IsStopRequested() ? TickResult::kStopped : Fail("Seek output Link aborted", epoch);

            case QueuePushResult::kPushed:
                // 只有成功入队的数据才计入已交付前缀
                if (packet && seek.phase_ != SeekPreroll::Phase::kReplay) {
                    (seek.pending_track_ == PlaybackTrack::kVideo ? seek.video_ : seek.audio_)
                        .RecordDeliveredPacket(seek.pending_digest_);
                    LOG_DEBUG("Demuxer preroll delivered {} packet at epoch {}",
                              seek.pending_track_ == PlaybackTrack::kVideo ? "video" : "audio",
                              epoch);
                } else if (!packet) {
                    // EOS 入队成功后，才标记对应轨道已发送 EOS
                    (seek.pending_track_ == PlaybackTrack::kVideo ? seek.video_eos_sent_ : seek.audio_eos_sent_) = true;
                    LOG_INFO("Demuxer sent EndOfStream to {} queue at epoch {}",
                             seek.pending_track_ == PlaybackTrack::kVideo ? "video" : "audio",
                             epoch);
                }

                seek.pending_.reset();
                return TickResult::kContinue;
        }
    }

    // 当前这一遍已经读到 EOF，处理各轨道的 EOS
    if (seek.pass_eof_) {
        // Replay 必须完整重读并匹配前两个阶段已交付的包，否则不能安全跳过旧包
        if (seek.phase_ == SeekPreroll::Phase::kReplay &&
            (!seek.video_.prefix_verified_ || !seek.audio_.prefix_verified_)) {
            LOG_ERROR("Demuxer replay hit EOF at epoch {} before verifying prefixes (video done {}, audio done {})",
                      epoch,
                      seek.video_.compared_packet_count_,
                      seek.audio_.compared_packet_count_);
            return Fail("Seek replay ended before its delivered packet prefixes were restored", epoch);
        }

        // 当前阶段需要结束哪些轨道，就向对应输出队列发送 EOS
        const bool need_video = !seek.video_eos_sent_ && seek.phase_ != SeekPreroll::Phase::kAudio;
        const bool need_audio = !seek.audio_eos_sent_ && seek.phase_ != SeekPreroll::Phase::kVideo;

        if (need_video || need_audio) {
            // EOS 也通过 pending 限时发送，避免队列满时阻塞节点线程
            seek.pending_track_ = need_video ? PlaybackTrack::kVideo : PlaybackTrack::kAudio;
            seek.pending_ = PacketStreamItem{EndOfStream{epoch}};
        } else if (seek.phase_ == SeekPreroll::Phase::kReplay) {
            // Replay 完成，恢复普通 EOF 等待流程
            LOG_INFO(
                "Demuxer replay finished at epoch {}: prefixes verified (video {}, audio {} packets); "
                "resuming normal EOF wait",
                epoch,
                seek.video_.delivered_packet_count_,
                seek.audio_.delivered_packet_count_);
            seek_preroll_.reset();
            eos_sent_ = true;
        } else {
            // 当前轨道已发送 EOS，等待下游处理完成并报告 Ready
            LOG_DEBUG("Demuxer {} pass reached EOF at epoch {}; waiting for the other track to become ready",
                      seek.phase_ == SeekPreroll::Phase::kVideo ? "video" : "audio",
                      epoch);
            const auto state = timeline_->Get();
            timeline_->Wait(state.state_version_);
        }

        return TickResult::kContinue;
    }

    // 检查是否有更新的 Seek，并准备本次 Read 的中断状态
    // 不能持有 control_mutex_ 调用 Read()，因为底层读取可能阻塞
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (pending_seek_) {
            LOG_DEBUG("Demuxer preroll at epoch {} yields to a newer seek before reading", epoch);
            return TickResult::kContinue;
        }

        demuxer_->PrepareNextIoForSeekInterrupt();
    }

    // 每轮最多读取一个包
    Packet packet;
    const auto result = demuxer_->Read(packet);

    // Read 期间可能发生新的 Seek，旧 epoch 的读取结果直接丢弃
    if (!IsCurrentEpoch(epoch)) {
        LOG_DEBUG("Demuxer preroll discards read result for stale epoch {}", epoch);
        return TickResult::kContinue;
    }

    switch (result.status_) {
        case FfmpegReadStatus::kStopInterrupted:
            LOG_DEBUG("Demuxer preroll read interrupted by stop at epoch {}", epoch);
            return TickResult::kStopped;

        case FfmpegReadStatus::kSeekInterrupted:
            // 与 kSkipped 一样，返回主循环处理新的 Seek
            LOG_DEBUG("Demuxer preroll read interrupted by a newer seek at epoch {}", epoch);
        case FfmpegReadStatus::kSkipped:
            return TickResult::kContinue;

        case FfmpegReadStatus::kError:
            LOG_ERROR("Demuxer preroll read failed at epoch {}: {}", epoch, result.error_.Message());
            return Fail(result.error_, epoch);

        case FfmpegReadStatus::kEndOfStream:
            // 只记录本轮读取结束，EOS 在下一轮统一处理
            LOG_DEBUG("Demuxer preroll reached EOF at epoch {}, phase {}", epoch, static_cast<int>(seek.phase_));
            seek.pass_eof_ = true;
            return TickResult::kContinue;

        case FfmpegReadStatus::kPacket:
            break;
    }

    // Video/Audio 阶段只保留当前阶段需要的轨道数据
    // 另一条轨道会在后续重新定位时再次读取
    const bool video = packet.GetStreamIndex() == demuxer_->GetVideoStreamIndex();
    if ((seek.phase_ == SeekPreroll::Phase::kVideo && !video) || (seek.phase_ == SeekPreroll::Phase::kAudio && video)) {
        LOG_DEBUG("Demuxer {} preroll drops a {} packet at epoch {} (will re-read it next pass)",
                  seek.phase_ == SeekPreroll::Phase::kVideo ? "video" : "audio",
                  video ? "video" : "audio",
                  epoch);
        return TickResult::kContinue;
    }

    const auto digest = seek.packet_hasher_.Fingerprint(packet);

    if (seek.phase_ == SeekPreroll::Phase::kReplay) {
        auto& prefix = video ? seek.video_ : seek.audio_;

        // 已交付前缀必须逐包匹配；匹配成功后跳过，避免重复送入解码器
        if (prefix.compared_packet_count_ < prefix.delivered_packet_count_) {
            if (!prefix.MatchesDeliveredPrefix(digest)) {
                LOG_ERROR(
                    "Demuxer replay could not reproduce the {} packet prefix at epoch {} "
                    "({} of {} compared)",
                    video ? "video" : "audio",
                    epoch,
                    prefix.compared_packet_count_,
                    prefix.delivered_packet_count_);
                return Fail("Seek cannot reproduce the delivered packet prefix; refusing to drop unknown packets",
                            epoch);
            }

            return TickResult::kContinue;
        }

        // 已发送 EOS 的轨道不应再出现新包
        if (video ? seek.video_eos_sent_ : seek.audio_eos_sent_) {
            LOG_ERROR("Demuxer replay produced a new {} packet at epoch {} after its EOS was already sent",
                      video ? "video" : "audio",
                      epoch);
            return Fail("Seek replay produced new packets after an already delivered EOS", epoch);
        }
    }

    // 暂存当前包，下一轮通过 SendFor() 限时入队
    // 去向必须按本包的流号决定；原来写死成 kVideo，音频包会被塞进视频队列。
    packet.SetEpoch(epoch);
    seek.pending_track_ = video ? PlaybackTrack::kVideo : PlaybackTrack::kAudio;
    seek.pending_digest_ = digest;
    seek.pending_ = PacketStreamItem{std::in_place_type<Packet>, std::move(packet)};

    return TickResult::kContinue;
}
}  // namespace mediacore
/**
 * @file ffmpeg_demuxer_node.cpp
 * @brief FfmpegDemuxerNode 的读包、时间跳转（Seek）以及 EOF 挂起等待逻辑实现
 * @details
 * 作为 Pipeline 的数据源驱动节点，工作线程内部采用协作式循环驱动：
 * 1. 优先处理控制流：在每次读包前检查 HasPendingControl()，确保 Seek 请求具备高于普通数据读取的响应优先级；
 * 2. 数据流驱动：循环调用 FfmpegDemuxer::Read() 提取目标视频流 Packet，并通过 Link 阻塞推向下游；
 * 3. 流末尾挂起（EOF Parking）：遇到流末尾并向下游发送 EndOfStream 信号后，使用条件变量挂起自身工作线程，
 *    杜绝在文件末尾高频空转消耗 CPU，等待下一次 Seek 或 Stop 唤醒。
 */

#include "demux/ffmpeg_demuxer_node.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "core/inode.h"
#include "mediacore/logger.h"

namespace mediacore {

namespace {
constexpr const char* kNodeName = "Demuxer";
}

/**
 * @brief 构造函数：初始化解复用节点并接管已打开的解复用器实例
 * @param sync_ctx 跨节点共享的播放状态同步上下文
 * @param event_callback 用于向 Pipeline 控制层汇报错误与状态事件的回调集合
 * @param demuxer 已完成 Open 探测的解复用器对象
 * @throws std::invalid_argument 当 demuxer 为空时抛出异常
 */
FfmpegDemuxerNode::FfmpegDemuxerNode(std::shared_ptr<SyncContext> sync_ctx,
                                     PipelineEventCallbacks event_callback,
                                     std::unique_ptr<FfmpegDemuxer> demuxer)
    : INode(kNodeName, std::move(sync_ctx), std::move(event_callback)), demuxer_(std::move(demuxer)) {
    if (!demuxer_) {
        LOG_ERROR("FfmpegDemuxerNode requires an opened FfmpegDemuxer");
        throw std::invalid_argument("FfmpegDemuxerNode requires an opened FfmpegDemuxer");
    }

    // 获取并锚定初始的全局数据流版本世代号（Epoch）
    output_epoch_ = GetCurrentEpoch();
    LOG_INFO("FfmpegDemuxerNode constructed, active epoch {}", output_epoch_);
}

/**
 * @brief 异步登记最新的 Seek 请求，触发协作中断并唤醒可能正在等待的工作线程
 * @param epoch Pipeline 为本次 Seek 分配的新数据流世代号
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
 * @brief 解复用工作线程的主循环单次步进逻辑（Tick）
 * @return 执行状态结果：
 * - kContinue：本轮读取成功或顺利跳过，继续下一轮 Tick 处理；
 * - kStopped：因收到外部停止信号而正常退出线程；
 * - kError：遭遇不可恢复的底层错误并触发 Pipeline 报错。
 */
TickResult FfmpegDemuxerNode::Tick() {
    // 1. 若当前世代的数据已读到末尾且已向下游广播过 EOS，直接进入 EOF 条件变量挂起等待
    if (eos_sent_) {
        return WaitAtEof();
    }

    // 2. 读包前置检查：若恰好有新 Seek 到来，放弃从旧位置启动耗时 I/O，优先放行控制逻辑
    std::uint64_t read_epoch = 0;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);

        if (pending_seek_.has_value()) {
            return TickResult::kContinue;
        }

        // 锁定当前 Seek 请求编号，作为本次 Read 的中断比较基准
        demuxer_->PrepareNextIoForSeekInterrupt();

        // 锁定本次读取所属的世代号
        read_epoch = output_epoch_;
    }

    // 3. 执行单包读取与过滤
    Packet packet;
    FfmpegReadResult result = demuxer_->Read(packet);
    switch (result.status_) {
        case mediacore::FfmpegReadStatus::kPacket:
            // 校验世代：若读包阻塞期间全局 Epoch 已被新 Seek 刷新，直接丢弃该过时包
            if (read_epoch != GetCurrentEpoch()) {
                return TickResult::kContinue;
            }

            packet.SetEpoch(read_epoch);

            // 将有效视频压缩包推入输出端口 Link
            if (!SendOutput(PacketStreamItem{std::in_place_type<Packet>, std::move(packet)})) {
                if (IsStopRequested()) {
                    LOG_DEBUG(
                        "Demuxer stopped sending packet for epoch {}, "
                        "output link was interrupted by stop",
                        read_epoch);
                    return TickResult::kStopped;
                }

                LOG_ERROR(
                    "Demuxer could not send packet for epoch {}, "
                    "output link is unavailable",
                    read_epoch);
                return Fail("Demuxer output link is unavailable", read_epoch);
            }

            return TickResult::kContinue;

        case mediacore::FfmpegReadStatus::kSkipped:
            // 读到音频或字幕等非目标流，数据包内部已自动释放，继续下一个 Tick
            return TickResult::kContinue;

        case mediacore::FfmpegReadStatus::kSeekInterrupted:
            // 底层读包被更新的 Seek 中断打断，返回 Continue 让主循环优先执行 Seek
            LOG_DEBUG("Demuxer read for epoch {} was interrupted by a newer seek", read_epoch);
            return TickResult::kContinue;

        case mediacore::FfmpegReadStatus::kStopInterrupted:
            // 底层读包被外部停机指令打断
            LOG_INFO("Demuxer read for epoch {} was interrupted by stop", read_epoch);
            return TickResult::kStopped;

        case mediacore::FfmpegReadStatus::kEndOfStream:
            // 流末尾校验：如果已经发生新 Seek，忽略该过时的老流 EOF
            if (read_epoch != GetCurrentEpoch()) {
                LOG_DEBUG("Ignoring stale EndOfStream for epoch {}", read_epoch);
                return TickResult::kContinue;
            }

            // 向下游下发当前世代的流结束信号（EndOfStream）
            if (!SendOutput(PacketStreamItem{EndOfStream{read_epoch}})) {
                if (IsStopRequested()) {
                    LOG_DEBUG(
                        "Demuxer could not send EndOfStream for epoch {}, "
                        "output link was interrupted by stop",
                        read_epoch);
                    return TickResult::kStopped;
                }

                LOG_ERROR(
                    "Demuxer could not send EndOfStream for epoch {}, "
                    "output link is unavailable",
                    read_epoch);
                return Fail("Demuxer output link is unavailable while sending EndOfStream", read_epoch);
            }

            eos_sent_ = true;
            LOG_INFO(
                "Demuxer reached end of stream for epoch {}; "
                "waiting for seek or stop",
                read_epoch);
            return TickResult::kContinue;

        case mediacore::FfmpegReadStatus::kError:
            // 若报错属于已被弃用的历史世代，予以丢弃
            if (read_epoch != GetCurrentEpoch()) {
                LOG_DEBUG("Ignoring stale Demuxer read error for epoch {}", read_epoch);
                return TickResult::kContinue;
            }

            LOG_ERROR("Demuxer read error for epoch {}: {}", read_epoch, result.error_.Message());
            return Fail(result.error_, read_epoch);
    }

    LOG_ERROR("FfmpegDemuxer returned an unknown read status for epoch {}", read_epoch);
    return Fail("FfmpegDemuxer returned an unknown read status", read_epoch);
}

/**
 * @brief 外部中断触发钩子：响应停止通知，打断底层阻塞 I/O 并唤醒挂起线程
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
 * @brief 提取并执行外部暂存的最新 Seek 请求
 * @note 必须在工作线程执行 Tick 前优先调用，确保控制流优先于数据流
 */
TickResult FfmpegDemuxerNode::ProcessPendingControl() {
    PendingSeek request;
    {
        std::lock_guard<std::mutex> lock(control_mutex_);

        if (!pending_seek_) {
            return TickResult::kContinue;
        }

        // 提取请求参数并复位暂存槽
        request = pending_seek_.value();
        pending_seek_.reset();

        // 确认本轮 Seek 已经正式接管执行，重置中断比较基准
        demuxer_->PrepareNextIoForSeekInterrupt();
    }

    // 执行耗时的底层 Seek 时必须在锁外进行，避免阻塞外部线程继续登记新的 Seek
    return ExecuteSeek(request);
}

/**
 * @brief 当全流读取到达 EOF 后，使用条件变量阻塞工作线程，避免高频空转
 * @return 被新 Seek 唤醒时返回 kContinue；被 Stop 唤醒时返回 kStopped
 */
TickResult FfmpegDemuxerNode::WaitAtEof() {
    std::unique_lock<std::mutex> lock(control_mutex_);

    // 标记工作线程进入休眠状态
    eof_parked_.store(true, std::memory_order_release);
    LOG_DEBUG("Demuxer is waiting at EOF for a seek or stop request");

    // 挂起等待外部触发 Stop 或提交新的 Seek
    eof_cv_.wait(lock, [this]() { return IsStopRequested() || pending_seek_.has_value(); });

    const bool stop_requested = IsStopRequested();

    // 唤醒退出休眠
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
 * @brief 调用底层的 FfmpegDemuxer 执行实际时间跳转并在下游广播 StreamReset
 * @param request 包含目标时间与世代号的跳转请求参数
 * @return 执行状态结果
 */
TickResult FfmpegDemuxerNode::ExecuteSeek(PendingSeek request) {
    LOG_INFO("Demuxer executing seek to {} us, epoch {}", request.target_us_, request.epoch_);

    const FfmpegSeekResult seek_result = demuxer_->Seek(request.target_us_);

    if (seek_result.status_ == FfmpegSeekStatus::kSeekInterrupted) {
        LOG_DEBUG("Demuxer seek for epoch {} was superseded", request.epoch_);
        return TickResult::kContinue;
    }

    if (seek_result.status_ == FfmpegSeekStatus::kStopInterrupted) {
        LOG_DEBUG("Demuxer seek for epoch {} was interrupted by stop", request.epoch_);
        return TickResult::kStopped;
    }

    if (seek_result.status_ != FfmpegSeekStatus::kSucceeded) {
        // 若执行失败但该世代已经被后续的更新 Seek 取代，则忽略此失效错误
        if (request.epoch_ != GetCurrentEpoch()) {
            LOG_DEBUG("Ignoring failed superseded seek for epoch {}", request.epoch_);
            return TickResult::kContinue;
        }

        LOG_ERROR("Demuxer seek failed: {}", seek_result.error_.Message());
        return Fail(seek_result.error_, request.epoch_);
    }

    // 若 Seek 成功返回期间外部又有更新的 Seek 发生，不切换输出状态，直接交给后续 Seek 处理
    if (request.epoch_ != GetCurrentEpoch()) {
        LOG_DEBUG("Ignoring successful seek because a newer seek arrived for epoch {}", request.epoch_);
        return TickResult::kContinue;
    }

    // 本次 Seek 真正落地生效：更新允许输出的世代号，并重置 EOF 发送标记
    output_epoch_ = request.epoch_;
    eos_sent_ = false;

    // 向下游解码器投递流重置信令（StreamReset），要求下游冲刷清空老世代的残留缓存
    if (!SendOutput(PacketStreamItem{StreamReset{request.epoch_, request.target_us_}})) {
        if (IsStopRequested()) {
            LOG_DEBUG("Demuxer could not send StreamReset for epoch {}; output link was interrupted by stop",
                      request.epoch_);
            return TickResult::kStopped;
        }

        LOG_ERROR("Demuxer could not send StreamReset for epoch {}, output link is unavailable", request.epoch_);
        return Fail("Demuxer Send StreamReset failed", output_epoch_);
    }

    if (seek_result.best_effort_position_us_.has_value()) {
        LOG_INFO(
            "Demuxer completed seek for epoch {}; target {} us, demuxer-reported position {} us",
            request.epoch_,
            request.target_us_,
            *seek_result.best_effort_position_us_);
    } else {
        LOG_INFO(
            "Demuxer completed seek for epoch {}; target {} us; "
            "actual presentation position will be determined by decoded output",
            request.epoch_,
            request.target_us_);
    }

    return TickResult::kContinue;
}

}  // namespace mediacore
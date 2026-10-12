/**
 * @file ffmpeg_demuxer_node.h
 * @brief 媒体流水线中的解复用节点（视频数据源 Source Node）
 * @details
 * 该节点拥有独立的后台工作线程，作为整个流水线的数据驱动源头：
 * 1. 负责独占调度 FfmpegDemuxer 执行数据读取（Read）与时间定位（Seek）；
 * 2. 读出的 Packet 压缩数据包推入下游 Link；
 * 3. 定位成功后向下游广播 StreamReset 冲刷信令；
 * 4. 读到流末尾（EOF）时向下游投递 EndOfStream 信号，并利用条件变量（WaitAtEof）阻塞自身工作线程，杜绝 CPU 空转。
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

#include "av_types/packet.h"
#include "core/inode.h"
#include "core/iseek_source.h"
#include "core/stream_item.h"
#include "demux/ffmpeg_demuxer.h"

namespace mediacore {

/**
 * @brief 节点向下游输出的数据载体（Variant 变体类型）
 * @details 在任意时刻仅承载以下三种数据项之一：
 * 1. Packet：包含有效压缩码流数据的视频包；
 * 2. StreamReset：指示下游解码器清空内部残留缓存的重置控制信号；
 * 3. EndOfStream：通知下游全流数据已读取完毕的结束终止信号。
 */
using PacketStreamItem = StreamItem<Packet>;

/**
 * @class FfmpegDemuxerNode
 * @brief 负责从媒体文件读取音视频包并处理定位请求的源节点
 * @details
 * 本节点属于 Source Node，无上游输入端口，只向下游提供数据流输出。
 *
 * 线程模型设计：
 * FFmpeg 底层上下文严禁并发调用。外部控制线程（如 UI 或 Player 控制线程）发起 Seek 时，
 * 仅通过 RequestSeek() 记录参数并向底层发送协作中断信号；实际耗时的跳转操作（ExecuteSeek）
 * 统一委托给本节点专职的工作线程串行处理。
 */
class FfmpegDemuxerNode final : public INode<NoPort, PacketStreamItem>, public ISeekSource {
public:
    /**
     * @brief 构造函数：初始化解复用节点并接管已打开的解复用器所有权
     * @param sync 跨节点共享的播放控制与状态同步上下文
     * @param event_callbacks 用于向 Pipeline 控制层汇报错误与状态事件的回调集合
     * @param demuxer 已完成 Open 探测的解复用器实例（所有权转移）
     * @throws std::invalid_argument 当 sync 为空、demuxer 为空或缺少必要回调时抛出
     */
    FfmpegDemuxerNode(std::shared_ptr<SyncContext> sync,
                      PipelineEventCallbacks event_callbacks,
                      std::unique_ptr<FfmpegDemuxer> demuxer);

    /**
     * @brief 虚析构函数：释放节点资源
     */
    ~FfmpegDemuxerNode() override = default;

    // 禁用拷贝与移动，保证节点拓扑所有权唯一
    FfmpegDemuxerNode(const FfmpegDemuxerNode&) = delete;
    FfmpegDemuxerNode& operator=(const FfmpegDemuxerNode&) = delete;
    FfmpegDemuxerNode(FfmpegDemuxerNode&&) = delete;
    FfmpegDemuxerNode& operator=(FfmpegDemuxerNode&&) = delete;

    /**
     * @brief 获取当前打开媒体的元数据快照
     * @return 包含时长、编码、流配置等信息的 MediaInfo 引用
     */
    const MediaInfo& GetMediaInfo() const noexcept { return demuxer_->GetMediaInfo(); }

    /**
     * @brief 检查解复用工作线程是否正处于读到流末尾后的挂起阻塞状态
     * @return true 表示解复用线程已消费完全部 Packet 并休眠；收到新 Seek 或 Stop 时复位为 false
     * @note 仅反映数据源是否读完，不代表下游解码与渲染节点已全部消费呈现完毕
     */
    bool IsEofParked() const noexcept { return eof_parked_.load(std::memory_order_acquire); }

    /**
     * @brief 异步登记最新的 Seek 跳转请求，并打断当前正阻塞的 I/O 操作
     * @param epoch 本次跳转对应的唯一版本世代号
     * @param target_us 本次跳转的目标时间戳（微秒）
     * @details 非阻塞设计：仅将参数暂存至 pending_seek_ 并触发中断通知，由工作线程异步执行
     */
    void RequestSeek(std::uint64_t epoch, std::int64_t target_us) override;

private:
    /**
     * @brief 工作线程的单次主循环驱动函数
     * @return TickResult 执行结果：
     * - kContinue：继续执行下一次 Tick；
     * - kStopped：收到停止指令或下游连接已关闭，安全退出工作线程；
     * - kError：发生不可恢复的底层错误，上报 Pipeline。
     */
    TickResult Tick() override;

    /**
     * @brief 外部中断触发钩子：响应停止或管线暂停信号，强制打断阻塞中的底层 FFmpeg I/O 并唤醒挂起线程
     */
    void OnInterruptBlockingIo() noexcept override;

    /**
     * @brief 检查是否存在外部提交但尚未被工作线程处理的 Seek 控制请求
     * @return 存在待处理的请求时返回 true
     */
    bool HasPendingControl() const noexcept override;

    /**
     * @brief 提取并执行外部提交的 Seek 请求
     * @return 处理结果（kContinue、kStopped 或 kError）
     * @note 由工作线程在执行数据读取（Tick）前优先调用，保证控制指令的时效性高于数据流
     */
    TickResult ProcessPendingControl() override;

private:
    /**
     * @struct PendingSeek
     * @brief 待执行的 Seek 参数暂存结构体
     */
    struct PendingSeek {
        std::uint64_t epoch_{0};  ///< 跳转请求的世代版本号
        std::int64_t target_us_{0};  ///< 目标跳转时间（微秒）
    };

    /**
     * @brief 当底层数据流读到 EOF 时调用此函数阻塞工作线程，避免高频空转损耗 CPU
     * @return 线程被新 Seek、Flush 或 Stop 唤醒后返回 TickResult::kContinue
     */
    TickResult WaitAtEof();

    /**
     * @brief 执行底层跳转，并在成功后向下游推送 StreamReset 信令
     * @param request 包含世代号与目标时间的具体跳转请求
     * @return 执行状态结果：
     * - kContinue：Seek 成功或已被更新的后续请求替代；
     * - kStopped：收到停止通知或向下游推送信令失败；
     * - kError：跳转发生底层不可恢复异常。
     */
    TickResult ExecuteSeek(PendingSeek request);

    std::unique_ptr<FfmpegDemuxer> demuxer_;  ///< 底层解复用器实例（由本节点工作线程独占调用）
    std::uint64_t output_epoch_{0};  ///< 当前允许向下游输出的数据流所属世代号；仅在 Seek 成功后更新
    bool eos_sent_{false};  ///< 当前世代下是否已向下游发送过 EndOfStream 信号

    mutable std::mutex control_mutex_;  ///< 保护 pending_seek_ 并发读写的互斥锁
    std::optional<PendingSeek> pending_seek_;  ///< 待执行的最新 Seek 请求（覆盖更新，连续 Seek 仅保留最新一条）

    std::atomic<bool> eof_parked_{false};  ///< 工作线程当前是否休眠在 WaitAtEof() 中的状态标志
    std::condition_variable eof_cv_;  ///< 用于在出现新 Seek 或停止请求时唤醒挂起线程的条件变量
};

}  // namespace mediacore
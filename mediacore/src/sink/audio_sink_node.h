/**
 * @file audio_sink_node.h
 * @brief 音频播放流程的最后一个节点：把解码后的声音交给播放设备，并报告音频播放进度
 * @details
 * 1. 数据流向：
 * Demuxer -> Packet -> AudioDecoder -> PcmFrame -> AudioSinkNode -> IAudioSink
 * IAudioSink 的具体实现可以是 ALSA 等操作系统音频设备，节点负责暂停、播放、Seek、停止和 EOS处理
 * 2. Seek 时会先清空跳转前的音频，等待新的播放世代的 Reset 到达后再接收新的音频数据
 * EOS 到达后还要等待设备把已经缓冲区内吃音频播放完毕
 * 3. 节点会根据：已经交给设备的数据 - 设备中仍在播放的数据 估算出当前的播放进度，并把位置交给时间轴
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "audio_decoder/iaudio_sink.h"
#include "audio_decoder/pcm_frame.h"
#include "av_sync/playback_timeline.h"
#include "core/inode.h"
#include "core/stream_item.h"

namespace mediacore {

/**
 * @class AudioSinkNode
 * @brief 音频输出管线终点节点，负责非阻塞硬件写入、Seek 状态清理与时基发布。
 */
class AudioSinkNode final : public INode<StreamItem<PcmFrame>, NoPort> {
public:
    /**
     * @brief 构造音频输出节点。
     * @param sync_ctx 全局管线同步上下文
     * @param event_callbacks 用于向 Pipeline 上报错误与状态的事件回调
     * @param timeline 共享播放时间轴管理器
     * @param sink 独占的音频输出硬件接口（通过 unique_ptr 转移所有权）
     * @param max_buffer_ms 声卡待播缓冲区的逻辑时长预算（单位：毫秒，合法区间：[5, 2000]）
     * @throw std::invalid_argument 时间轴或底层设备为空，或缓冲时长超出合法区间
     * @throw std::overflow_error 初始时间转换采样帧序号时发生数值溢出
     */
    AudioSinkNode(std::shared_ptr<SyncContext> sync_ctx,
                  PipelineEventCallbacks event_callbacks,
                  std::shared_ptr<PlaybackTimeline> timeline,
                  std::unique_ptr<IAudioSink> sink,
                  int max_buffer_ms);

    ~AudioSinkNode() override = default;

    // 禁用拷贝与移动，保持管线节点的唯一生命周期
    AudioSinkNode(const AudioSinkNode&) = delete;
    AudioSinkNode& operator=(const AudioSinkNode&) = delete;
    AudioSinkNode(AudioSinkNode&&) = delete;
    AudioSinkNode& operator=(AudioSinkNode&&) = delete;

protected:
    /**
     * @brief 执行一轮音频播放工作
     * @return 执行状态结果（继续运行、退出或上报错误）
     * @details
     * 每次播放前需要先检查播放状态，再处理尚未播放的 PCM、读取新的 PCM/StreamReset/EndOfStream
     * 并更新播放位置，设备暂时无法接收数据时会等待下一轮，不会丢弃未写完的声音
     */
    TickResult Tick() override;

    /**
     * @brief 检查时间轴中是否有本节点尚未处理的播放控制变化
     * @return 播放、暂停、Seek 或停止状态发生变化且还未应用时返回 true
     */
    [[nodiscard]] bool HasPendingControl() const noexcept override;

    /**
     * @brief 在本节点的工作线程中执行新的播放控制
     * @return 控制信令处理后的执行状态
     * @details
     * - Seek（检测到世代变化）：立即 Flush 丢弃硬件与本地残留的旧音频，进入 await_reset_ 状态
     * - 暂停/恢复：调用底层 sink_->Pause() 冻结或恢复硬件播放，保留缓冲区数据不丢弃
     */
    TickResult ProcessPendingControl() override;

    /**
     * @brief 工作线程退出前清理音频设备
     * @details
     * Stop、处理错误或未处理异常都会使工作线程结束，基类随后调用本函数
     * 本函数会丢弃未播放的声音，让线程及时退出，正常的 EOS 播放尾音不依赖此函数完成
     */
    void OnThreadExit() noexcept override;

private:
    /**
     * @brief 在控制状态已经同步后，推进一轮实际的音频播放
     */
    TickResult ProcessTick();

    /**
     * @brief 第一次播放音频时，需要先打开 ALSA 音频设备，并按照 state 状态来决定是否播放
     * @param state 当前时间轴状态快照
     */
    void EnsureOpen(const PlaybackTimeline::Snapshot& state);

    /**
     * @brief 根据设备的待播放的数据量估算出当前的播放位置，并将其发送到共享时间轴
     * @details
     * 已经写入到设备的不等于已经被播放，需要用写入量 - 等待播放的量 估算实际的播放位置
     */
    void PublishClock();

    /**
     * @brief 向共享时间轴报告：本音频轨道已经准备好跳转后的声音
     * @param target_pts_us 当前音频轨道准备就绪的媒体起始时间戳（微秒）
     * @param eof 是否直接跳转到了文件末尾（无后续有效音频）
     */
    void ReportReady(std::int64_t target_pts_us, bool eof);

    /**
     * @brief 告诉时间轴：音频轨道已经播放完毕
     * @details
     * 上报的是音频轨道本身，时间轴还会等待视频轨道结束，只有所有实际存在的轨道都结束才算媒体结束
     */
    void ReportEnded();

    /**
     * @brief 处理一段待播放 PCM，并尽可能把它写入音频设备
     * @param state 当前时间轴状态快照
     * @details 会跳过 Seek 目标之前或已经写过的部分，暂停时会保留数据，时间戳出现空档时补零值静音
     * 并处理设备只接受一部分数据的情况，没有写完的尾部会保留在节点中，下一轮继续写
     */
    void ProcessFrame(const PlaybackTimeline::Snapshot& state);

    /**
     * @brief 把一段数据交给底层音频设备，并按设备实际接收量推进节点侧记账
     * @param samples 交错排列的数据，至少包含 frames 个采样帧
     * @param frames 请求交给设备的采样帧数，必须 > 0
     * @return 设备实际接收的采样帧数，范围为 [0, frames]
     * @throw std::runtime_error 设备返回了大于请求量的非法计数
     * @details 本函数不操作 ALSA，真正的设备写入由 IAduioSink 负责实现
     * 本节点只做两件事：
     * 1. 调用 sink_->Write()，再把 next_frame_ 前移实际接收量、在首次成功写入时置起音频时钟基准
     * 2. 补静音与写真实 PCM 两条路径共用它，保证设备接收多少，时钟就走多少
     */
    std::size_t SubmitFrames(const std::int16_t* samples, std::size_t frames);

    /**
     * @brief 清除一段声音相关的播放状态，准备接收新世代的数据
     * @details
     * Seek 和 StreamReset 都意味着旧世代的声音作废：丢弃未写完的数据块、复位写偏移
     * 并撤销就绪/排空/结束/时钟起始标志
     */
    void ClearStreamState();

    /**
     * @brief 接受一条匹配当前时间轴的 StreamReset，切换到新播放世代
     * @param epoch 新的世代号
     * @param target_us 要跳转的目标位置
     * @details 下一个写入位置和最远记录位置都重置到目标，真正的目标帧裁剪由随后的 PCM 完成
     */
    void ApplySeekReset(std::uint64_t epoch, std::int64_t target_us);

    /**
     * @brief 把媒体时间换算成音频时间轴上的采样帧位置
     * @param target_us 目标时间戳（微秒）
     * @details
     * 48000 Hz 表示 1 秒有 48000 个采样帧，结果用于确定 Seek 后从哪一帧开始接收声音
     * @return 对应的绝对采样帧序号
     */
    [[nodiscard]] static std::int64_t TargetSample(std::int64_t target_us);

private:
    /// 上游解码器一次最多交付的采样帧数（5 ms @ 48 kHz）；校验和补静音缓冲区共用。
    static constexpr std::size_t kMaxFramesPerBlock = PcmFrame::kMaxBlockFrames;

    std::shared_ptr<PlaybackTimeline> timeline_;  ///< 音频、视频和播放控制共同使用的时间轴
    std::unique_ptr<IAudioSink> sink_;  ///< 实际接收并播放 PCM 的音频设备接口
    const std::int64_t max_buffer_frames_{0};  ///< 允许提前交给设备、但还没有播放的声音上限（采样帧）

    std::optional<PcmFrame> pending_frame_;  ///< 已取出但尚未全部交给设备的一段声音
    std::size_t frame_offset_{0};  ///< pending_frame_ 中已经交给设备的采样帧数，下一轮从此处继续
    std::int64_t next_frame_{0};  ///< 下一帧写入位置（采样帧），包含补静音
    std::int64_t last_end_frame_{0};  ///< 已收到的声音中最远的结束位置，用于跳到音频末尾时报告位置
    std::array<std::int16_t, kMaxFramesPerBlock * PcmFrame::kChannels> slience_{};  ///< 补空档用的全零静音

    std::uint64_t active_epoch_{0};  ///< 当前允许向硬件输出的有效播放世代号
    std::uint64_t latest_epoch_{0};  ///< 本节点最近看到的时间轴世代号，用于发现新的 Seek
    std::uint64_t applied_version_{0};  ///< 本节点已经执行到的播放控制版本号

    bool is_opened_{false};  ///< 音频设备是否已经打开
    bool await_reset_{false};  ///< Seek 后旧声音已清理，正在等待新位置的 StreamReset
    bool seek_ready_{false};  ///< 本世代是否已向时间轴报告音频已准备好
    bool draining_{false};  ///< 已接收到 EOS，正在等待设备播放完剩余尾音
    bool ended_{false};  ///< 本世代是否已向时间轴报告音频轨道结束
    bool clock_ready_{false};  ///< 本世代是否已有声音交给设备，可用于估算音频播放进度
    std::chrono::steady_clock::time_point last_flow_stall_log_{};  ///< 流控卡死诊断日志上次打印时刻，用于限频
};

}  // namespace mediacore
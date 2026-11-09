/**
 * @file audio_sink_node.cpp
 * @brief 音频输出节点：将解码后的 PCM 数据送入音频设备，并向共享时间轴报告播放进度
 * @details
 * 上游音频解码器将音频解码并采重样为PCM，通过 Link 送到本节点
 * 本节点通过 IAudioSink 将 PCM 送到 ALSA，除了接收 PCM，还会接收 StreamReset 和 EndOfStream
 * 本节点负责执行播放、暂停、恢复播放、Seek 和 Stop 等控制，不负责其他操作
 * Seek 时会清除设备中尚未播放的音频数据，并丢弃节点中缓存的音频数据，收到 StreamReset 后处理新位置的音频
 * 收到 EndOfStream 后，还需要通过 Drain() 确认设备已经播放完此前接收到的所有音频数据
 */
#include "sink/audio_sink_node.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "audio_decoder/iaudio_sink.h"
#include "audio_decoder/pcm_frame.h"
#include "av_sync/playback_timeline.h"
#include "core/inode.h"
#include "mediacore/logger.h"

namespace mediacore {

namespace {
// 上游输入队列为空时单轮最多等待的时长；取自时间轴 Wait 的默认节奏，保证控制与时钟更新及时
const auto kInputWait = std::chrono::milliseconds(5);

constexpr const char* kModuleName = "audio_sink_node";
}  // namespace

/**
 * @brief 创建音频播放节点，并准备播放设备和时间抽
 * @param sync_ctx 全局管线同步上下文
 * @param event_callbacks 用于向 Pipeline 报告错误和状态的事件回调
 * @param playback_timeline 与视频和控制线程共享的播放时间轴
 * @param audio_output IAudioSink 用于和 ALSA 交互的播放后端
 * @param device_buffer_ms 音频输出实现请求的设备缓冲时长，也用于限制单次写入量
 * @throw std::invalid_argument 参数为空或设备缓冲请求超出 [5, 2000] ms
 */
AudioSinkNode::AudioSinkNode(std::shared_ptr<SyncContext> sync_ctx,
                             PipelineEventCallbacks evrnt_callbacks,
                             std::shared_ptr<PlaybackTimeline> timeline,
                             std::unique_ptr<IAudioSink> audio_output,
                             int device_buffer_ms)
    : INode(kModuleName, std::move(sync_ctx), std::move(evrnt_callbacks)),
      timeline_(std::move(timeline)),
      sink_(std::move(audio_output)),
      write_chunk_cap_frames_(static_cast<std::int64_t>(device_buffer_ms) * PcmFrame::kSampleRate / 1000) {
    if (!timeline_ || !sink_ || device_buffer_ms < 5 || device_buffer_ms > 2000) {
        LOG_ERROR(
            "Audio sink node requires a timeline, an output device and a device buffer in [5, 2000] ms; got {} ms",
            device_buffer_ms);
        throw std::invalid_argument("AudioSinkNode: invalid timeline/device/buffer duration");
    }

    // 以构造时刻的时间轴状态作为起点：之后只有版本号变化才会触发 Seek 清理
    const auto state = timeline_->Get();
    active_epoch_ = latest_epoch_ = state.epoch_;
    applied_version_ = state.state_version_;
    next_frame_ = TargetSample(state.seek_target_us_);
}

/**
 * @brief 将目标时间戳（微秒）转换为 48000 Hz 采样率下的绝对采样帧序号（向上取整）
 * @param target_us 目标时间戳（微秒）
 * @return 对应的绝对采样帧序号
 * @details
 * 负的时间戳按 0 处理，向上取整保证 Seek 后从目标之后的第一个采样帧开始播放，不会把目标之前的声音多播出来
 * 先算整秒再算余数，避免 target_us * 48000 溢出
 */
std::int64_t AudioSinkNode::TargetSample(std::int64_t targrt_us) {
    // 1. 如果 targrt_us 小于 0，就把它改成 0；如果大于或等于 0，就保持原值不变
    targrt_us = std::max<std::int64_t>(0, targrt_us);

    const auto whole = targrt_us / 1000000;
    if (whole > std::numeric_limits<std::int64_t>::max() / PcmFrame::kSampleRate) {
        throw std::overflow_error("AduioSinkNode: seek target exceeds sample clock range");
    }

    return whole * PcmFrame::kSampleRate + (targrt_us % 1000000 * PcmFrame::kSampleRate + 999999) / 1000000;
}

/**
 * @brief 检查共享时间轴是否有本节点尚未处理的控制更新
 * @return 时间轴中的播放、暂停、Seek 或停止状态比本节点记录的新时返回 true
 */
bool AudioSinkNode::HasPendingControl() const noexcept {
    return timeline_->GetStateVersion() != applied_version_;
}

/**
 * @brief 清除一段声音相关的播放状态，准备接收新世代的数据
 * @details
 * 旧世代的声音无论是否已经取到节点中，都在 Seek 处作废，丢弃未写完的数据块、复位块内偏移
 * 并撤销就绪/排空/结束/时钟跑起标志，调用方随后负责设置新的播放位置
 */
void AudioSinkNode::ClearStreamState() {
    pending_frame_.reset();
    frame_offset_ = 0;
    seek_ready_ = draining_ = ended_ = clock_ready_ = false;

    // 停滞计时属于"这一世代设备还愿不愿意收数据"的判断，必须随世代一起清空：
    // 旧世代积累的起始时刻若跨过 Seek 保留下来，新世代刚 Flush 完的第一轮
    // 就可能在设备只是短暂满缓冲时立刻触发看门狗误报。
    stall_since_.reset();
}

/**
 * @brief 在本节点的工作线程中执行新的播放控制（播放、暂停、恢复或 Seek 清理）
 * @return 控制信令处理后的执行状态：继续运行、停止或者不可恢复性错误退出
 */
TickResult AudioSinkNode::ProcessPendingControl() {
    try {
        // 1. 读取一个完整的状态快照，后续判断都以此为基准
        const auto state = timeline_->Get();

        // 2. 根据 epoch_ 判断是否为同一轮的播放，如果不是，则清理节点和设备缓冲区
        if (state.epoch_ != latest_epoch_) {
            LOG_DEBUG("Audio sink applaying seek: observed epoch_ {} -> {}, active epoch {}, target_{} us",
                      latest_epoch_,
                      state.epoch_,
                      active_epoch_,
                      state.seek_target_us_);

            // 2.1 Seek 由控制线程先行发起，立即丢弃设备中排队的旧声音，不等新 Reset 到达
            if (is_opened_) {
                sink_->Flush();
            }

            // 2.2 记录新世代，但此时还不能立即接收新 PCM
            latest_epoch_ = state.epoch_;
            await_reset_ = true;  // 在匹配的新世代 StreamReset 到达前，拒绝接收新 PCM
            ClearStreamState();

            // 2.3 将预期写入位置和最远位置 重置到跳转目标点
            next_frame_ = last_end_frame_ = TargetSample(state.seek_target_us_);
        }

        // 3. 把播放意图同步给硬件：跳转定位期间也必须保持静音，否则会播出旧位置的声音
        if (is_opened_) {
            // watchdog measures active playback time only. Hardware pause freezes queued audio;
            // software pause drops it and replays the preserved samples on resume. In both
            // cases no device consumption is expected while paused, so start a fresh stall
            // interval after every playback-control transition instead of counting that time.
            stall_since_.reset();
            sink_->Pause(!state.playing_ | state.seeking_ | state.stopped_);
        }

        // 4. 记录本次已经应用的控制版本，下一次 HasPendingControl() 发现版本号再次变化时，工作线程会重新进入此函数处理
        applied_version_ = state.state_version_;
        LOG_DEBUG("Audio sink applied control version {}, epoch {}, playing {}, seeking {}, stopped {}",
                  state.state_version_,
                  state.epoch_,
                  state.playing_,
                  state.seeking_,
                  state.stopped_);
        return TickResult::kContinue;
    } catch (const std::exception& error) {
        LOG_ERROR("Audio sink control failed at active epoch {}, observed epoch {}: {}",
                  active_epoch_,
                  latest_epoch_,
                  error.what());

        return Fail(error.what());
    }
}

/**
 * @brief 首次处理时打开底层音频设备，并对齐初始的播放/暂停状态
 * @param state 当前共享的播放状态
 * @details
 * 设备懒打开：只有真正开始处理音频时才申请硬件资源，直接停止的会话不会占用声卡
 * 打开后立即按时间轴状态决定是否暂停，避免在暂停状态下打开时先响一下
 */
void AudioSinkNode::EnsureOpen(const PlaybackTimeline::Snapshot& state) {
    if (is_opened_) {
        return;
    }

    // 暂停、定位等待或已停止时，设备一打开就保持静音，与 ProcessPendingControl 的判定保持一致。
    const bool paused = !state.playing_ || state.seeking_ || state.stopped_;

    sink_->Open();
    is_opened_ = true;
    sink_->Pause(paused);

    LOG_INFO("Audio sink device ready at epoch {}, write chunk cap {} frames ({} ms), initially {}",
             active_epoch_,
             write_chunk_cap_frames_,
             write_chunk_cap_frames_ * 1000 / PcmFrame::kSampleRate,
             paused ? "paused" : "playing");
}

/**
 * @brief 依据声卡排队延迟估算当前实际发声位置，并发布至全局时间轴（PublishAudio）
 * @details
 * 已写入位置 next_frame_ 减去设备仍排队的帧数，才是扬声器附近正在发声的位置，Seek 清理之后、
 * StreamReset 到达之前不发布，避免旧位置把时间轴拉回去
 */
void AudioSinkNode::PublishClock() {
    if (!is_opened_ || !clock_ready_ || await_reset_ || ended_) {
        return;
    }

    // 欠载（XRUN）后不要把 next_frame_ 往回减：
    // 设备缓冲区被播空才会欠载，已提交的帧确实发过声；AlsAudioSink 在 Recover(-EPIPE) 中
    // 删除 shadow 前缀后，下面 next_frame_ - DelayFrames() 已经等于真实播放位置。
    // 若再额外回退，ProcessFrame 会按"时间戳空档"补出一段静音，把后续音频整体后移。
    const auto delay = std::max<std::int64_t>(0, sink_->DelayFrames());

    // 起播阶段设备排队量可能超过已写入量，位置至少从 0 开始，不能发布负时间
    const auto played = std::max<std::int64_t>(0, next_frame_ - delay);
    timeline_->PublishAudio(active_epoch_, PcmFrame::SamplesToUs(played));
}

/**
 * @brief 登记跳转后的首包音频已就绪，向上层时间轴上报 MarkReady
 * @param target_pts_us 当前已确认可用的音频媒体位置，单位为微秒
 * @param eof 是否直接跳转到了文件末尾，跳转到末尾为 true
 * @details
 * 每个世代只上报一次，所有实际存在的轨道都就绪后，时间轴会返回聚合结果，由最后就绪的一路向上报告 Seek 完成
 */
void AudioSinkNode::ReportReady(std::int64_t target_pts_us, bool eof) {
    if (seek_ready_) {
        return;
    }

    seek_ready_ = true;
    const auto result = timeline_->MarkReady(PlaybackTrack::kAudio, active_epoch_, target_pts_us, eof);

    LOG_DEBUG(
        "Audio sink reported seek readiness at epoch {}, position {} us, EOF {}", active_epoch_, target_pts_us, eof);

    if (result) {
        LOG_INFO("All tracks ready after seek at epoch {}, position {} us, all tracks at EOF {}",
                 active_epoch_,
                 result->position_us_,
                 result->all_eof_);

        ReportSeekCompleted(active_epoch_, result->position_us_, result->all_eof_);
    }
}

/**
 * @brief 向上层时间轴上报本轨道播放彻底完结（MarkEnded）
 * @details
 * 正常播放时，必须等待设备中剩余的音频全部播放完毕后才能报告结束，如果 Seek 的目标已经到达了音频结尾
 * 那么可以在没有新的 PCM 输出的情况下报告音频轨道结束
 * 尾音已经由 Drain 播放完毕，这里只负责登记结束；若所有轨道都已结束才报告媒体结束
 */
void AudioSinkNode::ReportEnded() {
    // 1. 如果已经报告过结束
    if (ended_) {
        return;
    }

    ended_ = true;
    draining_ = false;

    LOG_INFO("Audio sink finished audio track at epoch {}", active_epoch_);

    // 2. 登记音频轨道结束
    if (timeline_->MarkEnded(PlaybackTrack::kAudio, active_epoch_)) {
        LOG_INFO("All tracks ended; audio sink reporting media end an epoch {}", active_epoch_);

        ReportMediaEnded(active_epoch_);
    }
}

/**
 * @brief 把一段音频数据交给底层的音频播放设备，并按照设备实际的接收量推进节点侧记录
 * @param samples 交错排列的 PCM 数据
 * @param frames 请求交给设备的采样帧数 > 0
 * @return 设备实际接收的采样帧数，范围为 [0,frames]
 * @details
 * 设备写入本身由 IAudioSink 完成，本节点只负责按实际接收数量记录：
 * next_frame_ 只能前移设备真正接收的帧数，否则时钟会跑在声音前面
 * 返回 0 表示设备暂时无法接收，调用方保留数据，下一轮继续
 * 首次成功写入后 clock_ready_ 置位，本世代音频时钟才有跑起基准
 */
std::size_t AudioSinkNode::SubmitFrames(const std::int16_t* samples, std::size_t frames) {
    const std::size_t written = sink_->Write(samples, frames);

    if (written > frames) {
        // 设备违反了 "不超过请求量" 的接口约定，按不可恢复性错误处理
        throw std::runtime_error("AduioSinkNode: device reported writing more samples than requested");
    }
    next_frame_ += static_cast<std::int64_t>(written);

    if (written != 0) {
        clock_ready_ = true;
    }

    return written;
}

/**
 * @brief 处理当前暂存的 PCM 音频块，执行对齐、补静音并向底层声卡写入
 * @param state 全局时间轴快照
 * @details
 * 这个函数只处理 pending_audio_frame_，不会主动读取下一块 PCM
 * 原因是，音频设备每次可能一次性接收当前的一块音频，在这块音频没有交付之前必须从同一个位置继续处理
 */
void AudioSinkNode::ProcessFrame(const PlaybackTimeline::Snapshot& state) {
    const auto& frame = *pending_frame_;
    const auto count = frame.FrameCount();

    // 1. 数据合法性校验：确保为成对的立体声采样点、单包未超限，且时间戳未溢出
    if (frame.samples_.size() % PcmFrame::kChannels != 0 || count > kMaxFramesPerBlock) {
        throw std::runtime_error("AudioSinkNode: invalid PCM block: frames=" + std::to_string(count) +
                                 ", max=240, samples=" + std::to_string(frame.samples_.size()));
    }

    const auto frame_count = static_cast<std::int64_t>(count);
    if (frame.start_sample_ > std::numeric_limits<std::int64_t>::max() - frame_count) {
        throw std::overflow_error("AudioSinkNode: PCM timestamp overflow");
    }

    // 2. 记录收到的最远结束位置，即使随后因为重复或 Seek 而没有播放，也要用它来报告音频末尾
    last_end_frame_ = std::max(last_end_frame_, frame.start_sample_ + frame_count);

    // 3. 整块都落在期望写入位置之前：属于已经播放过了或 Seek 时抛弃旧数据，直接丢弃
    if (frame.start_sample_ + frame_count <= next_frame_) {
        pending_frame_.reset();
        frame_offset_ = 0;
        return;
    }

    // 4. 块头与已写入如区域重叠（常见于 Seek 跨边界）：逃过前段，只保留尚未写入的部分
    if (frame.start_sample_ < next_frame_) {
        frame_offset_ = std::max(frame_offset_, static_cast<std::size_t>(next_frame_ - frame.start_sample_));
    }

    // 5. Seek 定位期间只向时间轴确认：本轨迹已经到达这里，不发声，等待全轨完成定位协调
    if (state.seeking_) {
        const auto ready_sample = frame.start_sample_ + static_cast<std::int64_t>(frame_offset_);

        ReportReady(PcmFrame::SamplesToUs(ready_sample), false);

        timeline_->Wait(state.state_version_);
        return;
    }

    // 6. 暂停时保留当前数据块与偏移，等待恢复后从原位置继续
    if (!state.playing_) {
        timeline_->Wait(state.state_version_);
        return;
    }

    // 7. 流控：本轮能写多少帧，取设备可写空间与单次写入上限的较小值
    //
    //    硬闸门只认 WritableFrames()（snd_pcm_avail_update），因为它与 writei
    //    能否接纳数据严格一致。绝不能用 DelayFrames()（snd_pcm_delay）来算可用量：
    //    PulseAudio / PipeWire 的 ALSA 插件会把服务器端缓冲也算进去，
    //    实测出现过"本对象只持有 9600 帧，device_delay 却报 19999 帧"，
    //    甚至在 Flush 清空之后 delay 依然不回落（见 Flush 里的诊断日志）。
    //    更关键的是 DelayFrames() 返回的影子缓冲长度只在
    //    "device_delay < 已提交量" 时才缩短，用 delay 当闸门会让可用量恒为 0
    //    且永不恢复：上层停写 -> 设备欠载 -> delay 依旧偏高 -> 整条音频链
    //    连同被它拖住的解封装/视频链一起永久停摆
    //    （"只有一瞬间有声、画面卡住"就是这个原因）。
    //
    //    设备缓冲请求值只作为单次写入上限，不约束累计在队量；插件实际缓冲可能更大。
    //
    //    delay 只进诊断日志（outstanding），不参与"是否写入"的判定。
    const auto outstanding = std::max<std::int64_t>(0, sink_->DelayFrames());
    const auto device_writable = sink_->WritableFrames();
    const auto available =
        std::max<std::int64_t>(0, std::min<std::int64_t>(device_writable, write_chunk_cap_frames_));

    if (available == 0) {
        const auto now = std::chrono::steady_clock::now();

        // 走到这里只可能是一种情况：device_writable == 0，即设备当前一点空间都没有。
        // 看门狗：设备迟迟不接受新数据，说明它已经停止消费。此时上游音频解码器会
        // 永久阻塞在无超时的 Link::Send 上，音频包队列被填满，解封装随之卡住，
        // 视频链也一起断供——整张图静默僵死且没有任何错误上报。
        // 与其静默等待，不如主动失败，让 Player 进入 kError 并把原因交给用户。
        if (!stall_since_.has_value()) {
            // 首次进入"设备满、写不进去"的状态，从此刻开始计时
            stall_since_ = now;
        } else if (now - *stall_since_ >= kDeviceStallTimeout) {
            const std::string detail = sink_->DescribeState();
            LOG_ERROR("Audio device stopped consuming: epoch {}, no write for {} ms, next_frame {}, "
                      "outstanding {} frames, writable {} frames, write chunk cap {} frames, device [{}]",
                      active_epoch_,
                      std::chrono::duration_cast<std::chrono::milliseconds>(now - *stall_since_).count(),
                      next_frame_,
                      outstanding,
                      device_writable,
                      write_chunk_cap_frames_,
                      detail);
            // ProcessFrame 返回 void，错误通过异常上抛；AudioSinkNode::Tick 会捕获并转成 Fail，
            // 与前面"PCM 块非法""时间戳溢出"两条错误路径保持同一处理方式。
            throw std::runtime_error("audio device stopped consuming audio data");
        }

        // 设备暂时不可写时限频记录状态，便于区分正常背压与设备停滞。
        if (now - last_flow_stall_log_ >= std::chrono::seconds(5)) {
            last_flow_stall_log_ = now;
            LOG_DEBUG("Audio sink flow control idle: epoch {}, next_frame {}, outstanding {} frames, "
                      "device writable {} frames, write chunk cap {} frames, block start {} +{} of {}, pending {}, device [{}]",
                     active_epoch_,
                     next_frame_,
                     outstanding,
                     device_writable,
                     write_chunk_cap_frames_,
                     frame.start_sample_,
                     frame_offset_,
                     count,
                     pending_frame_.has_value() ? "yes" : "no",
                     sink_->DescribeState());
        }

        timeline_->Wait(state.state_version_);
        return;
    }

    // 8. 本块剩余的可写数据量，以及当前数据在媒体时间轴上的位置
    const auto frame_letf = count - frame_offset_;
    const auto frame_position = frame.start_sample_ + static_cast<std::int64_t>(frame_offset_);

    std::size_t written = 0;
    if (frame_position > next_frame_) {
        // 9. 时间戳出现空档：设备期望的位置早于本块起点，先补静音把位置推倒本块起点，单轮最多补一块（240帧）
        // 避免一次补几秒静音占住主工作线程，未补完的空档下一轮继续
        const auto gap = std::min<std::int64_t>(
            {static_cast<std::int64_t>(kMaxFramesPerBlock), available, frame_position - next_frame_});
        written = SubmitFrames(slience_.data(), static_cast<std::size_t>(gap));
    } else {
        // 10. 写入真实的 PCM：只请求设备当前能接收的数量，短写时保留剩余部分
        const auto requested = std::min<std::size_t>(frame_letf, static_cast<std::size_t>(available));

        written = SubmitFrames(frame.samples_.data() + frame_offset_ * PcmFrame::kChannels, requested);
        frame_offset_ += written;
    }

    // 11. 本块全部交给设备后释放它，否则保留音频数据供下一轮继续
    if (frame_offset_ >= count) {
        pending_frame_.reset();
        frame_offset_ = 0;
    }

    // 12. 只要本次写进去了数据，就说明设备仍在消费，清零停滞计时
    if (written != 0) {
        stall_since_.reset();
    }

    // 13. 设备此刻一点也接收不了数据：让出本轮等待下一轮继续
    if (written == 0) {
        timeline_->Wait(state.state_version_);
    }
}

/**
 * @brief 接受一条匹配当前时间轴的 StreamReset，切换到新播放世代
 * @param epoch 新世代号
 * @param target_us 要跳转的目标位置
 */
void AudioSinkNode::ApplySeekReset(std::uint64_t epoch, std::int64_t target_us) {
    active_epoch_ = epoch;
    await_reset_ = false;
    ClearStreamState();
    next_frame_ = last_end_frame_ = TargetSample(target_us);
    LOG_DEBUG("Audio sink accepted StreamReset at epoch {}, target {} us", active_epoch_, target_us);
}

/**
 * @brief 执行单轮音频输出调用
 * @return 音频输出调用的执行结果：继续下一轮、停止或不可恢复性错误
 */
TickResult AudioSinkNode::ProcessTick() {
    // 1. 先检查是否停止
    if (IsStopRequested()) {
        return TickResult::kStopped;
    }

    const auto state = timeline_->Get();

    // 2. 如果有新的控制请求变化：播放、暂停、Seek 或停止时，先让出一轮，等基类把控制应用完
    // 直接返回 kContinue 而不是在此处执行，是为了让控制处理和媒体处理保持单一执行路径
    if (state.state_version_ != applied_version_) {
        return TickResult::kContinue;
    }

    // 3. 打开音频设备，并把初始播放/暂停状态对齐到时间轴,如果已经打开了音频设备会直接返回
    EnsureOpen(state);

    // 4. 已报告结束后不再发布设备时钟。Drain 会把设备 delay 清零，继续发布会将
    //    next_frame_ 一次性推进整个尾部缓冲，造成 AVSync 把视频帧误判为严重迟到。
    if (ended_) {
        timeline_->Wait(state.state_version_);
        return TickResult::kContinue;
    }

    // 5. 如果已经收到上游 EOS，先排空设备。成功时不能再发布清零后的 delay，
    //    否则 next_frame_ 会一步跳到尾部并让 AVSync 丢弃视频帧。
    if (draining_) {
        if (!state.playing_ || state.seeking_) {
            // 暂停或 Seek 时，不排空，应该停留在原处，跳转则会直接 Flush 丢弃尾音
            timeline_->Wait(state.state_version_);
        } else if (sink_->Drain()) {
            // ReportEnded() 从最后一次平滑的音频位置切换到单调时钟，供视频收尾。
            LOG_INFO("Audio sink finished draining device at epoch {}", active_epoch_);
            ReportEnded();
        } else {
            // 尾音仍在播放：更新时钟并稍后继续轮询
            PublishClock();
            timeline_->Wait(state.state_version_);
        }

        return TickResult::kContinue;
    }

    // 6. 设备尚未进入 EOS 排空阶段时，按待播量更新音频主时钟
    PublishClock();

    // 7. 优先消费未写完的当前块，防止短写后新数据插队导致声音顺序错乱
    if (pending_frame_) {
        ProcessFrame(state);
        PublishClock();
        return TickResult::kContinue;
    }

    // 8. 当前块全部写完，则向上游索取新的数据，短暂超时返回以便更新设备时钟与响应控制
    auto item = ReceiveInputFor(kInputWait);
    if (!item) {
        return TickResult::kContinue;
    }

    const auto latest = timeline_->Get();

    // 9. 处理流重置：新世代的 Reset 是开始播放新位置声音的前提
    if (auto* reset = std::get_if<StreamReset>(&*item)) {
        if (reset->epoch_ != latest.epoch_) {
            // epoch 过期，直接忽略
            return TickResult::kContinue;
        }

        // Reset 先于控制应用到达时，先补 Seek 清理，保证设备中的旧声音都已经被丢掉
        if (latest_epoch_ != latest.epoch_) {
            const auto result = ProcessPendingControl();
            if (result != TickResult::kContinue) {
                return result;
            }
        }

        ApplySeekReset(reset->epoch_, reset->target_us_);
        return TickResult::kContinue;
    }

    // 10. 拦截新旧数据混杂：Seek 已经发生、新的 Reset 尚未到达时，丢弃期间到来的 PCM
    if (await_reset_ || active_epoch_ != latest_epoch_) {
        return TickResult::kContinue;
    }

    // 11 .分发正常的媒体数据
    if (auto* frame = std::get_if<PcmFrame>(&*item)) {
        // 旧世代的 PCM 留在队列里来不及清理也会走到这里，需要按世代号过滤
        if (frame->epoch_ == active_epoch_) {
            pending_frame_ = std::move(*frame);
            frame_offset_ = 0;
        }
    } else if (const auto* eos = std::get_if<EndOfStream>(&*item)) {
        // 如果收到了 EOS
        if (eos->epoch_ != active_epoch_) {
            return TickResult::kContinue;
        }

        LOG_INFO("AudioSinkNode received EOS at epoch {}, {}",
                 active_epoch_,
                 latest.seeking_ ? "seek reached audio end" : "waiting for device to finished remaining audio");

        if (latest.seeking_) {
            // Seek 直接命中流末尾：没有声音可播，直接上报就绪并结束，无须排空
            ReportReady(PcmFrame::SamplesToUs(last_end_frame_), true);

            ReportEnded();
        } else {
            // 正常播放到末尾
            draining_ = true;
        }
    }

    return TickResult::kContinue;
}

/**
 * @brief 执行一轮音频播放工作
 * @details
 * 设备错误、PCM 校验失败和数值溢出都会返回错误，应有基类按当前世代上报给 Pipeline，避免异常逃出工作线程
 */
TickResult AudioSinkNode::Tick() {
    try {
        return ProcessTick();
    } catch (const std::exception& error) {
        LOG_ERROR("AudioSinkNOde Processing failed at active epoch {}, latest epoch {}: {}",
                  active_epoch_,
                  latest_epoch_,
                  error.what());

        return Fail(error.what());
    }
}

/**
 * @brief 工作线程正常退出时清理
 * @details
 * 正常退出、错误退出和 Stop 退出都会执行此函数，强制 Flush 丢弃设备中剩余的音频，避免 Join 被等声音播放完毕
 * 拖住，日志函数本身在 noexcept 路径上，全部包含在 try 中
 */
void AudioSinkNode::OnThreadExit() noexcept {
    if (!is_opened_) {
        return;
    }
    try {
        sink_->Flush();
    } catch (const std::exception& error) {
        try {
            LOG_WARN("Audio sink could not discard remaining audio on thread exit at epoch {}: {}",
                     active_epoch_,
                     error.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_WARN("Audio sink could not discard remaining audio on thread exit at epoch {}: unknown exception",
                     active_epoch_);
        } catch (...) {
        }
    }
}

}  // namespace mediacore

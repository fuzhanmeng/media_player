/**
 * @file ffmpeg_audio_decoder_node
 * @brief 音频解码节点的实现：负责 接收来自 Demuxer 的 Packet、解码为 AVFrame(PCM)、重采样为
 * PcmFrame、转发给AudioSinkNode
 * @details
 * 上游 Demuxer 解复用出 Packet / 触发 Seek 发送 StreamReset / 流结束 发送 EndOfStream
 * 本节点 处理以上三种 数据，然后将解码后的 PCM 重采样后转发给 AudioSinkNode
 */

#include "audio_decoder/ffmpeg_audio_decoder_node.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace mediacore {

namespace {
constexpr const char* kModuleName = "ffmpeg_audio_decoder";
}

/**
 * @brief 构造函数：接管打开成功的音频解码器，记录当前播放世代号并构建音频播放节点
 * @param sync_ctx 全局状态同步上下文
 * @param event_callbacks 向 Pipeline 汇报错误、状态的回调函数
 * @param audio_decoder std::unique_ptr 类型的 音频解码器
 * @throw std::invalid_argument sync_ctx 或 audio_decoder 为空时抛出
 */
FfmpegAudioDecoderNode::FfmpegAudioDecoderNode(std::shared_ptr<SyncContext> sync_ctx,
                                               PipelineEventCallbacks event_callbacks,
                                               std::unique_ptr<FfmpegAudioDecoder> audio_decoder)
    : INode(kModuleName, std::move(sync_ctx), std::move(event_callbacks)), audio_decoder_(std::move(audio_decoder)) {
    // 因为 sync 已经由基类检查过了，音频节点只需要额外检查自己使用的 audio_decoder_
    if (!audio_decoder_) {
        LOG_ERROR("Audio decoder node requires an opened decoder");
        throw std::invalid_argument("Audio decoder node requires an opened decoder");
    }

    output_epoch_ = GetCurrentEpoch();

    LOG_INFO("Audio decoder node created at epoch {}", output_epoch_);
}

/**
 * @brief 将一块 PCM 放到连接 AudioSinkNode 的输出队列
 * @param frame 独立拥有数据 PCM 块，当前最多为 5毫秒，最后一块可能不足 5 ms
 * @return 输入成功入队且 epoch 有效时返回 true，否则 返回 false
 * @details
 * 入队前需要检查是否停止和当前的 epoch 编号，队列满时阻塞等待，等待期间可能发生 Stop / Seek
 * 所以需要入队后再次检查状态
 */
bool FfmpegAudioDecoderNode::ForWard(PcmFrame frame) {
    // 先保存 move 之后仍需使用的字段
    const std::uint64_t frame_epoch = frame.epoch_;

    // 1. 检查状态
    if (IsStopRequested() || !IsCurrentEpoch(frame.epoch_)) {
        return false;
    }

    // 2. 发送数据
    if (!SendOutput(PcmFrameStreamItem(std::move(frame)))) {
        return false;
    }

    // 3. 入队后再次检查状态
    return !IsStopRequested() && IsCurrentEpoch(frame_epoch);
}

/**
 * @brief 回调或通知发送无法继续的时候，需要判断线程接下来如何处理
 * @return
 * 如果是正常停止，返回 kStopped
 * 如果是 Seek 触发的 epoch 变化，返回 kContinue
 * 如果是 不可恢复性错误，返回 kError
 */
TickResult FfmpegAudioDecoderNode::CancelledResult() {
    return FailIfEpochIsCurrent(
        MediaError::FromInternal(kModuleName, "Aduio decoder output Link unavailable without stop"), output_epoch_);
}

/**
 * @brief 校验错误是否属于当前正在播放的批次 epoch
 * @param epoch 触发这次错误时数据的版本号
 * @param media_error 捕获到的媒体错误对象
 * @return
 * kStopped：播放正常停止，线程正常退出
 * kContinue：属于已跳转作废的旧数据错误，直接忽略
 * kError：属于当前播放批次的真实错误，记录日志，并向上层抛出错误
 * @details
 * 当发生 Seek 跳转时，全局 epoch 立即 +1，但是解码线程可能还在处理旧位置残余的数据包
 * 本函数用于过滤那些不属于本次播放批次的错误
 */
TickResult FfmpegAudioDecoderNode::FailIfEpochIsCurrent(MediaError media_error, std::uint64_t epoch) {
    if (IsStopRequested()) {
        LOG_DEBUG("Audio decoder node interrupted or failed operation from stale epoch {}", epoch);

        return TickResult::kStopped;
    }

    if (!IsCurrentEpoch(epoch)) {
        LOG_DEBUG("Audio decoder node ignored an interrupted or failed operation from stale epoch {}", epoch);

        return TickResult::kContinue;
    }

    if (!media_error.HasError()) {
        media_error = MediaError::FromInternal(kModuleName, "Audio decoder returned kError without MediaError");
    }

    // 节点知道用户是否停止或跳转，如果不是这两种操作就是真的出错了
    LOG_ERROR("Audio decoder node failed at epoch {}: {}", epoch, media_error.ToString());

    return Fail(std::move(media_error), epoch);
}

/**
 * @brief 节点的主循环处理
 * @return 本次循环的处理结果（继续、正常停止、错误退出）
 */
TickResult FfmpegAudioDecoderNode::Tick() {
    // 1. 从输入队列中获取一项数据 StreamItme(Packet/StreamReset/EndOfStream)
    auto item = ReceiveInput();

    if (!item) {
        // 输入队列关闭或异常不可用，没有接收到数据
        return FailIfEpochIsCurrent(
            MediaError::FromInternal(kModuleName, "Audio decoder input Link unavailable without Stop"), output_epoch_);
    }

    // 2. 根据接收到的数据类型来进行处理
    if (const auto* reset = std::get_if<StreamReset>(&*item)) {
        return HandleStreamReset(*reset);
    }

    if (const auto* packet = std::get_if<Packet>(&*item)) {
        return HandlePacket(*packet);
    }

    if (const auto* eos = std::get_if<EndOfStream>(&*item)) {
        return HandleEndOfStream(*eos);
    }

    return FailIfEpochIsCurrent(MediaError::FromInternal(kModuleName, "Audio decoder received an unknown stream item"),
                                output_epoch_);
}

/**
 * @brief 处理 Seek 操作发送过来的 StreamReset
 * @param reset 携带了目标时间和播放世代号的对象
 * @return 处理状态
 */
TickResult FfmpegAudioDecoderNode::HandleStreamReset(const StreamReset& reset) {
    // 1. 检查 reset 是否过时（可能期间又发生了新的 Seek）
    if (!IsCurrentEpoch(reset.epoch_)) {
        return TickResult::kContinue;
    }

    LOG_DEBUG("Audio decoder node resetting form epoch {} to epoch {}, target {} us",
              output_epoch_,
              reset.epoch_,
              reset.target_us_);

    // 2. 重置 FFmpeg 解码器与重采样器，清理缓存
    audio_decoder_->Flush();

    // 3. 将 Reset 标志发送到下游 AudioSinkNode，确保下游在接收到新数据的时候先清空播放缓冲区

    if (!SendOutput(StreamItem<PcmFrame>{reset})) {
        return CancelledResult();
    }

    // 4. 更新本地输出世代并重置 EOS 标记
    output_epoch_ = reset.epoch_;
    eos_forwarded_ = false;

    LOG_DEBUG("Audio decoder node forwarded Reset for epoch {}, target {} us", reset.epoch_, reset.target_us_);

    return TickResult::kContinue;
}

/**
 * @brief 处理正常的音频压缩包
 * @param packet Demuxer 发送过来的音频压缩包
 * @return 处理结果
 */
TickResult FfmpegAudioDecoderNode::HandlePacket(const Packet& packet) {
    // 1. 判断传过来的是否是当前世代包
    if (!IsCurrentEpoch(packet.GetEpoch())) {
        return TickResult::kContinue;
    }

    // 2. 时序合法性检验：数据包的批次必须与当前生效的 epoch 一致，且不能出现在 EOS 之后
    if (packet.GetEpoch() != output_epoch_ || eos_forwarded_) {
        return FailIfEpochIsCurrent(
            MediaError::FromState(kModuleName, "Audio packet arrived before reset or after EOS"), packet.GetEpoch());
    }

    // 3. 构造同步输出回调：解码器每切出一块 PCM，就同步调用 ForWard转发至下游 AudioSinkNode
    const auto output = [this](PcmFrame frame) { return ForWard(std::move(frame)); };

    // 4. 触发解码与重采样流程
    const auto decoder_result = audio_decoder_->Decode(packet, output);

    switch (decoder_result.state_) {
        case FfmpegAudioDecodeStatus::kOk:
            return TickResult::kContinue;
        case FfmpegAudioDecodeStatus::kCancelled:
            return CancelledResult();
        case FfmpegAudioDecodeStatus::kError:
            return FailIfEpochIsCurrent(decoder_result.media_error_, packet.GetEpoch());
    }

    return FailIfEpochIsCurrent(
        MediaError::FromInternal(kModuleName, "Audio decoder returned unknown Decode state"), packet.GetEpoch());
}

/**
 * @brief 处理流结束指令 EndOfStream
 * @param eos 流结束标记对象
 * @return 处理结果状态
 */
TickResult FfmpegAudioDecoderNode::HandleEndOfStream(const EndOfStream& eos) {
    // 1. 判断是否为过时的 EOS 指令
    if (!IsCurrentEpoch(eos.epoch_)) {
        return TickResult::kContinue;
    }

    // 2. 时序合法性检验：EOS 批次必须匹配当前输出批次，且同一批次不可重复接收
    if (eos.epoch_ != output_epoch_ || eos_forwarded_) {
        return FailIfEpochIsCurrent(
            MediaError::FromState(kModuleName, "Audio EOS arrived before Reset or was duplicated"), eos.epoch_);
    }

    LOG_INFO("Audio decoder node received EOS for epoch {}, draining remaining audio", eos.epoch_);

    // 3. 排空解码器与重采样器内部的所有音频数据
    const auto output = [this](PcmFrame frame) { return ForWard(std::move(frame)); };

    const auto drain_result = audio_decoder_->Drain(eos.epoch_, output);

    switch (drain_result.state_) {
        case FfmpegAudioDecodeStatus::kOk:
            break;
        case FfmpegAudioDecodeStatus::kCancelled:
            return CancelledResult();
        case FfmpegAudioDecodeStatus::kError:
            return FailIfEpochIsCurrent(drain_result.media_error_, eos.epoch_);
        default:
            return FailIfEpochIsCurrent(
                MediaError::FromInternal(kModuleName, "Audio decoder returned unknown Drain state"), eos.epoch_);
    }

    // 4. 排空期间可能发生阻塞，退出后需要再次检查关系是否被停止或被 Seek 打断
    if (IsStopRequested()) {
        return TickResult::kStopped;
    }

    if (!IsCurrentEpoch(eos.epoch_)) {
        return TickResult::kContinue;
    }

    // 5. 全部音频发送后，需要向下游发送 EOS 通知，告知音频解码器已经发送全部的音频
    if (!SendOutput(StreamItem<PcmFrame>(eos))) {
        return CancelledResult();
    }

    eos_forwarded_ = true;
    LOG_INFO("Audio decoder node finished draining and forward EOS for epoch {}", eos.epoch_);

    return TickResult::kContinue;
}

}  // namespace mediacore

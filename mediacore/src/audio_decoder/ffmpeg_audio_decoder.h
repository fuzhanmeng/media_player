/**
 * @file ffmpeg_audio_decoder.h
 * @brief 音频解码实现，将 AAC 解码为 PCM
 * @details
 * 本类将接收到的来自 Demuxer 解复用后的 Packet 送入解码器，解码为 PCM 并转换为 统一格式的 PcmFrame
 * 输出格式为每秒 48000 个采样帧、16 位有符号整数、双声道，按左、右声道交错存放
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "audio_decoder/pcm_frame.h"
#include "av_types/audio_codec_parameters.h"
#include "av_types/packet.h"
#include "mediacore/av_types/stream_info.h"
#include "mediacore/media_error.h"

namespace mediacore {

/**
 * @brief 一次 Decode 或 Drain 的结果
 * @details 正常结束和下游取消要和真实错误区分
 */
enum class FfmpegAudioDecodeStatus : std::uint8_t {
    kOk,  ///< 本次处理完成，Decode 可能输出 0-N 块 PCM，Drain 表示暂存声音已经全部交给下游
    kCancelled,  ///< 下游回调返回 false，请求中断，没有错误信息，继续处理前需要 Flush
    kError,  ///< 底层发生不可恢复的解码输入错误
};

/**
 * @brief 解码结果：返回错误状态和错误详情
 * @return 返回本次解码的的结果，包含解码状态和结构化MediaError，没有错误时 MediaError 为空
 */
struct FfmpegAudioDecodeResult final {
    FfmpegAudioDecodeStatus state_{FfmpegAudioDecodeStatus::kError};
    MediaError media_error_{};  ///< 仅 kError 携带错误详情，其他状态没有错误

    /**
     * @brief 状态为 kOk 且没有错误信息时 返回 true
     */
    [[nodiscard]] bool Successed() const noexcept {
        return state_ == FfmpegAudioDecodeStatus::kOk && !media_error_.HasError();
    }
};

// 向前声明
class FfmpegAudioDecoder;

/**
 * @brief 打开解码器的结果，成功时返回解码器对象，失败时返回错误详情
 * @note audio_decoder_ 使用 std::unique_ptr 表示独占所有权
 */
struct FfmpegAudioDecoderOpenResult final {
    std::unique_ptr<FfmpegAudioDecoder> audio_decoder_{};
    MediaError media_error_{};

    /**
     * @brief 成功创建解码器且没有错误返回
     */
    [[nodiscard]] bool Successed() const noexcept { return audio_decoder_ != nullptr && !media_error_.HasError(); }
};

/**
 * @class FfmpegAudioDecoder
 * @brief 管理解码和重采样资源，把压缩音频帧转换为 PCM
 * @details
 * 采用 Pimpl（把实现藏在指针后）：头文件只声明 Impl，FFmpeg 的具体接结构和调用留在 .cpp
 * Open 操作可由控制线程完成，再把对象交给音频解码节点，之后的 Decode / Drain / Seek 必须由同一个工作
 * 线程调用
 */
class FfmpegAudioDecoder {
public:
    /**
     * @brief 接收一块 PCM 的可调用对象类型
     */
    using Output = std::function<bool(PcmFrame)>;

    /**
     * @brief 每块输出最多包含的采样帧数
     * @details 选择了 240 帧， 240/48000 = 5ms
     */
    static constexpr int kMaxOutputFrames = PcmFrame::kMaxBlockFrames;

    /**
     * @brief 根据编码参数创建音频解码器，重采样器在收到第一帧时才创建
     * @param parameters 独立持有的编码参数副本，在 Demuxer 解复用后得到
     * @param time_base 音频所使用的时间基
     * @return 成功时返回一个解码器，失败时由 error 说明错误原因
     */
    [[nodiscard]] static FfmpegAudioDecoderOpenResult Open(AudioCodecParameters parameters, Rational time_base);

    /**
     * @brief 析构函数：释放解码器
     */
    ~FfmpegAudioDecoder();

    /**
     * @brief 解码器独占内部资源，不能通过复制创建第二个共享相同状态的对象
     */
    FfmpegAudioDecoder(const FfmpegAudioDecoder&) = delete;
    FfmpegAudioDecoder& operator=(const FfmpegAudioDecoder) = delete;

    /**
     * @brief 可转移内部资源的所有权移动后源对象不可再解码
     */
    FfmpegAudioDecoder(FfmpegAudioDecoder&&) noexcept;
    FfmpegAudioDecoder& operator=(FfmpegAudioDecoder&&) noexcept;

    /**
     * @brief 处理接收到的音频 Packet，并在处理期间通过回调交付 PCM
     * @param packet 接收到的来自 Demuxer 的音频 packet
     * @param output 每生成一块 PCM 就调用一次的接收函数，将数据通过 Link 传给 AudioSinkNode
     * @return 返回处理状态和错误详情，返回值本身不包含音频数据
     * @details
     * 一个 packet 可能产生 0-N 帧，每帧可能拆成多块 PCM，kOk 仅表示本次 Decode 完成，不代表声音播放成功
     */
    [[nodiscard]] FfmpegAudioDecodeResult Decode(const Packet& packet, const Output& output);

    /**
     * @brief 输入结束时，把解码器和重采样器暂存的声音继续交给下游
     * @param epoch seek 世代号
     * @param output 接收尾部 PCM 的同步回调
     * @return kOk 表示所有暂存声音都已经交付，节点此时才可以 发送 EOS
     * @details
     * 先发送 nullptr，通知 FFmpeg 进入 Drain 模式，不再接收新的输入
     */
    [[nodiscard]] FfmpegAudioDecodeResult Drain(std::uint64_t epoch, const Output& output);

    /**
     * @brief Flush 操作，Seek 时执行清理 解码器内部暂存的声音和时间跟踪状态
     * @details
     * 与 Drain 不同，Flush 直接丢弃旧数据，Seek 后不应该把旧位置的声音播放出来
     */
    void Flush() noexcept;

private:
    struct Impl;  ///< 前向声明：只告诉编译器“有这么个结构体，具体长啥样去 .cpp 里看”
    explicit FfmpegAudioDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_{nullptr};  ///< 唯一的数据成员：指向内部真正实现的智能指针};
};
}  // namespace mediacore

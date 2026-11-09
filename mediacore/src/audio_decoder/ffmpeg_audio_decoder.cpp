/**
 * @file ffmpeg_audio_decoder.cpp
 * @brief 音频解码器的具体实现，接收来自 Demuxer 的 Packet，解码为 PCM 并发送到 AudioSinkNode
 */

#include "audio_decoder/ffmpeg_audio_decoder.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "av_types/audio_codec_parameters.h"
#include "av_types/packet.h"
#include "mediacore/logger.h"
#include "mediacore/media_error.h"
#include "utils/ffmpeg_error.h"  ///<  use  ffmpeg_detail

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace mediacore {

namespace {

constexpr const char* kModuleName = "FfmpegAudioDecoder";

/**
 * @class AudioDecodeException
 * @brief 在内部多层调用间传递完整错误信息的异常对象
 * @details
 * 继承了 std::runtime_error，使普通异常处理也能够取得文字说明，本对象只保存错误信息，不拥有解码器资源
 */
class AudioDecodeException final : public std::runtime_error {
public:
    /**
     * @brief 用结构化错误构造异常，同时为基类保存可读的文字说明
     */
    explicit AudioDecodeException(MediaError error)
        : std::runtime_error(error.Message()),  // 保存错误的文字说明
          media_error_(std::move(error)) {}  // 保存完整的结构化错误

    /**
     * @brief 借用异常内保存的错误详情，引用不能超过本异常对象的生命周期
     */
    [[nodiscard]] const MediaError& Error() const noexcept { return media_error_; }

private:
    MediaError media_error_;
};

/**
 * @brief 创建状态错误：对象当前不允许执行该操作，例如 Drain 后未 Flush 就再次 Decode
 */
MediaError MakeStateError(std::string message) {
    return MediaError::FromState(kModuleName, std::move(message));
}

/**
 * @brief 创建内部错误：发现正常流程不应出现的情况，例如 send 和 receive 都无法继续
 */
MediaError MakeInternalError(std::string message) {
    return MediaError::FromInternal(kModuleName, std::move(message));
}

/**
 * @brief 将操作名称和 FFmpeg 风格的错误码整理为 MediaError，保留原始码并生成可读说明
 */
MediaError MakeFfmpegError(const char* operation, int code) {
    return ffmpeg_detail::MakeFfmpegMediaError(kModuleName, code, operation);
}

/**
 * @brief 输出 PCM 的时间基（1/48000 秒），用于将源帧的时间戳直接换算为 48kHz 下的采样帧编号
 */
constexpr AVRational kOutputTimeBase{1, PcmFrame::kSampleRate};

/**
 * @brief 单个解码帧 AVFrame 的 nb_sample 上限，即每声道采样书，超过后拒绝后续的转换
 * @details nb_samples 这一帧音频里，单声道包含了多少个采样点
 */
constexpr int kMaxInputFrameSamples = 1 << 20;

/**
 * @brief 创建错误详情抛出 AudioDecodeException，使内部调用立即转到入口的错误处理处
 * @param operation 出错的 FFmpeg 具体操作名称
 * @param code FFmpeg 风格的错误码
 * @note [[noreturn]] 表示函数不会正常返回；此处通过 throw 抛出异常对象
 */
[[noreturn]] void ThrowFfmpeg(const char* operation, int code) {
    throw AudioDecodeException(MakeFfmpegError(operation, code));
}

/**
 * @brief 解码上下文的自定义删除器，由 std::unique_ptr 在销毁或替换资源时调用
 * @details
 * operator() 重载了函数调用运算符，使得 CodecDeleter(){nullptr} 像函数一样调用
 * FFmpeg 资源必须用对应的 API 释放，不可以直接 delete
 */
struct CodecDeleter {
    void operator()(AVCodecContext* avcodec_ctx) const noexcept {
        if (avcodec_ctx != nullptr) {
            avcodec_free_context(&avcodec_ctx);
        }
    }
};

/**
 * @brief std::unique_ptr 的 AVFrame 自定义删除器：释放帧对象及它仍持有的声音数据引用，不能用 delete
 */
struct FrameDeleter {
    void operator()(AVFrame* avframe) const noexcept {
        if (avframe != nullptr) {
            av_frame_free(&avframe);
        }
    }
};

/**
 * @brief std::unique_ptr 的 SwrContext 自定义删除器：释放转换状态和内部暂存采样的时候，不可以 delete
 */
struct SwrDeleter {
    void operator()(SwrContext* swr_ctx) const noexcept {
        if (swr_ctx != nullptr) {
            swr_free(&swr_ctx);
        }
    }
};

/**
 * @brief FFmpeg 声道布局的 RAII 栈上管理类
 * @details 内部包含了 AVChannelLayout ，离开作用域时 自动调用 av_channel_layout_uninit 释放
 */
struct ChannelLayout final {
    AVChannelLayout value_{};

    ~ChannelLayout() { av_channel_layout_uninit(&value_); }
};

}  // namespace

/**
 * @struct FfmpegAudioDecoder::Impl
 * @brief 定义 PImpl 内部实现类
 * @details
 * 这是 Pimpl 的具体实现，避免了在头文件中定义向调用方暴露 FFmpeg 内部参数
 * 由 FfmpegAudioDecoder 的 std::unique_ptr 独占，析构时各个删除器自动的释放资源
 */
struct FfmpegAudioDecoder::Impl final {
    // 1. FFmpeg 核心资源（采用 RAII 智能指针独占管理）
    std::unique_ptr<AVCodecContext, CodecDeleter> avcodec_ctx_;  ///< 解码器上下文句柄
    std::unique_ptr<AVFrame, FrameDeleter> frame_;  ///< 重复复用的解码暂存帧缓冲
    std::unique_ptr<SwrContext, SwrDeleter> resampler_;  ///< 音频重采样器上下文句柄

    // 2. 音频格式与时间轴状态
    ChannelLayout input_layout_;  ///< 当前重采样器绑定的输入声道布局快照
    AVRational time_base_{};  ///< 解复用器传入的源流时间基
    int input_rate_{0};  ///< 输入采样率（Hz）
    AVSampleFormat input_format_{AV_SAMPLE_FMT_NONE};  ///< 输入采样点格式（如 AV_SAMPLE_FMT_FLTP）
    std::int64_t next_output_sample_{0};  ///< 下一块输出 PCM 应当从 48kHz 媒体时间轴的哪个绝对采样帧开始
    std::int64_t expected_input_sample_{0};  ///< 预计下一输入帧的位置，以输入采样率计数，用于检测时间戳跳变

    bool have_timeline_{false};  ///< 标记输出时间轴是否已被首帧锚定初始化
    bool drained_{false};  ///< 标记解码器与重采样器是否已被完全排空
    bool needs_flush_{false};  ///< 发生取消或错误后置位，强制要求下次输入前必须先执行 Flush

    /**
     * @brief 根据实际解码声音的规格，创建格式转换器
     * @param input 借用的 AVFrame，提供采样率、数值格式和声道布局
     * @details
     * 等首帧到达才创建，使用实际解码声音的规格作为 PCM 转换输入，而不是按照头文件预先配置
     * 格式转换器将 PCM 统一输出为 48000 Hz、16位有符号整数和双声道的交错布局数据，下游不需再处理源格式差异
     */
    void CreateResampler(const AVFrame& input) {
        // 1. 检查采样率、nb_channels 声道数量、声道布局合法性校验
        if (input.sample_rate <= 0 || input.ch_layout.nb_channels <= 0 ||
            av_channel_layout_check(&input.ch_layout) == 0) {
            ThrowFfmpeg("validata sudio frame sample rate/channel layout", AVERROR_INVALIDDATA);
        }

        // 2. default 为两个声道指定标准的左右布局
        ChannelLayout output_layout;
        av_channel_layout_default(&output_layout.value_, PcmFrame::kChannels);

        // 3. 配置输入与输出规格，0 表示配置成功，负数表示配置失败
        SwrContext* raw = nullptr;
        const int configured = swr_alloc_set_opts2(&raw,
                                                   &output_layout.value_,
                                                   AV_SAMPLE_FMT_S16,
                                                   PcmFrame::kSampleRate,
                                                   &input.ch_layout,
                                                   static_cast<AVSampleFormat>(input.format),
                                                   input.sample_rate,
                                                   0,
                                                   nullptr);

        // 4. 将配置器交给 std::unique_ptr
        resampler_.reset(raw);
        if (configured < 0) {
            ThrowFfmpeg("swr_alloc_set_opts2", configured);
        }

        if (!resampler_) {
            ThrowFfmpeg("swr_alloc_set_opts2", AVERROR(ENOMEM));
        }

        // 5. 配置之后初始化：0 为成功，负值为失败
        const int initialized = swr_init(resampler_.get());
        if (initialized < 0) {
            ThrowFfmpeg("swr_init", initialized);
        }

        // 6. 先释放上一份布局，再复制新的布局，copy 为内部数据简历独立副本，不只拷贝指针
        // 因为采重样器可能会被多次调用
        av_channel_layout_uninit(&input_layout_.value_);

        // 7. 拷贝声道布局的配置信息
        const int copied = av_channel_layout_copy(&input_layout_.value_, &input.ch_layout);

        if (copied < 0) {
            ThrowFfmpeg("av_channel_layout_copy", copied);
        }

        // 保存源音频的采样率和格式
        input_rate_ = input.sample_rate;
        input_format_ = static_cast<AVSampleFormat>(input.format);
        const char* input_format_name = av_get_sample_fmt_name(input_format_);
        const char* output_format_name = av_get_sample_fmt_name(AV_SAMPLE_FMT_S16);
        LOG_INFO("[AudioDecoder][Resampler] input_rate={} input_channels={} input_format={} "
                 "output_rate={} output_channels={} output_format={} initialized=true",
                 input_rate_, input.ch_layout.nb_channels,
                 input_format_name != nullptr ? input_format_name : "unknown",
                 PcmFrame::kSampleRate, PcmFrame::kChannels,
                 output_format_name != nullptr ? output_format_name : "unknown");
    }

    /**
     * @brief 将转换乘目标 PCM 的音频拆成小块，并逐块调用下游的接收函数进行发送
     * @param samples 指向交错声音数值数组的只读指针，存放顺序为左0、右0、左1、右1……
     * @param count 总采样帧数：双声道下的数组含有 count * 2 个总采样帧
     * @param epoch 当前播放的世代号
     * @param output 同步执行的下游接收回调，将分块好的 PCM 发送出去
     * @return 全部的 PCM 块发送成功，回调中断会返回 false
     */
    bool Emit(const std::int16_t* samples, int count, std::uint64_t epoch, const Output& output) {
        // 1. 循环拆分
        for (int offset = 0; offset < count;) {
            // 每块输出最多包含的采样帧数 kMaxOutputFrames = 240
            const int frames = std::min(FfmpegAudioDecoder::kMaxOutputFrames, count - offset);

            if (next_output_sample_ > std::numeric_limits<std::int64_t>::max() - frames) {
                ThrowFfmpeg("audio output timestamp overflow", AVERROR(ERANGE));
            }
            PcmFrame chunk;
            chunk.epoch_ = epoch;
            chunk.start_sample_ = next_output_sample_;

            // assign 复制[起点, 终点) 内的数据，采样帧数 * 声道数 才是数组元素数
            // chunk 独立拥有这块数据
            chunk.samples_.assign(samples + offset * PcmFrame::kChannels,
                                  samples + (offset + frames) * PcmFrame::kChannels);

            next_output_sample_ += frames;
            offset += frames;

            // 将 PCM 交付给后续的 AudioSinkNode
            if (!output(std::move(chunk))) {
                needs_flush_ = true;
                return false;
            }
        }

        return true;
    }

    /**
     * @brief 输出重采样器暂存的尾部声音，避免流结束的时候遗漏最后一段
     * @param epoch 当前播放的世代号
     * @param output 接收尾部 PCM 的同步回调
     * @return 没有更多输出时返回 true，回调请求中断时 返回 false
     */
    bool DrainResampler(std::uint64_t epoch, const Output& output) {
        if (!resampler_) {
            return true;
        }

        // 240 帧 × 2 声道 × sizeof(int16_t) = 960 字节
        std::int16_t samples[FfmpegAudioDecoder::kMaxOutputFrames * PcmFrame::kChannels]{};

        for (;;) {
            auto* output_data = reinterpret_cast<std::uint8_t*>(samples);

            // 空输入表示没有更多声音，继续取尾部的数据，返回值为生成的采样帧数，负值是错误
            // 输出容量必须与上面的缓冲一致（240 帧）；
            // 若继续传 kMaxInputFrameSamples（1<<20），swr_convert 会按 100 万帧写入，
            // 一旦 drain 残留超过 240 帧就会踩穿这块 960 字节的栈缓冲
            const int produced = swr_convert(
                resampler_.get(), &output_data, FfmpegAudioDecoder::kMaxOutputFrames, nullptr, 0);

            if (produced < 0) {
                ThrowFfmpeg("swr_convert(drain)", produced);
            }

            if (produced == 0) {
                return true;
            }

            if (!Emit(samples, produced, epoch, output)) {
                return false;
            }
        }
    }

    /**
     * @brief 把刚接收的 AVFrame 转换成统一 PCM 格式，并通过 Emit 发送
     * @param epoch 当前播放的世代号
     * @param output 下游接收回调
     * @return 处理完成时返回 true，下游请求中断时返回 false；错误抛给外部入口处理
     * @details
     */
    bool ConvertFrame(std::uint64_t epoch, const Output& output) {
        if (frame_->nb_samples < 0 || frame_->nb_samples > kMaxInputFrameSamples) {
            ThrowFfmpeg("validate audio frame sample count", AVERROR_INVALIDDATA);
        }
        if (frame_->nb_samples == 0) {
            return true;
        }

        if (!resampler_) {
            CreateResampler(*frame_);
        } else if (frame_->sample_rate != input_rate_ || frame_->format != input_format_ ||
                   av_channel_layout_compare(&frame_->ch_layout, &input_layout_.value_) != 0) {

            // 格式变化（HE-AAC 的 SBR/PS、拼接流）：先交付旧段尾部，再按新格式重建重采样器
            LOG_WARN("Audio format changed mid-stream: {} Hz/{} -> {} Hz/{}; rebuilding resampler",
                     input_rate_,
                     static_cast<int>(input_format_),
                     frame_->sample_rate,
                     static_cast<int>(frame_->format));

            if (!DrainResampler(epoch, output)) {
                return false;
            }

            // expected_input_sample_ 是按旧采样率累计的，而 CreateResampler 会把 input_rate_ 换成新值。
            // 若不换算，下面用新采样率计算的 input_sample 会与它差出 (新-旧) × 已播时长，
            // 必然误判为时间戳跳变，再走一次 drain + 重建并把时间轴锚点回退。
            if (frame_->sample_rate != input_rate_ && input_rate_ > 0) {
                expected_input_sample_ = av_rescale_q(expected_input_sample_,
                                                      AVRational{1, input_rate_},
                                                      AVRational{1, frame_->sample_rate});
            }

            CreateResampler(*frame_);
        }

        // 先用 FFmpeg 根据输入信息推算的 best_effort_timestamp，再用原始 pts；
        // AV_NOPTS_VALUE 表示没有时间戳，不能当作普通时间参与换算。
        const std::int64_t timestamp =
            (frame_->best_effort_timestamp != AV_NOPTS_VALUE) ? frame_->best_effort_timestamp : frame_->pts;
        const bool has_timestamp = (timestamp != AV_NOPTS_VALUE);

        // av_rescale_q 按两个时间基换算：这里得到以输入采样率计数的媒体位置。
        // 没有时间戳时沿用预计的连续位置，不凭每次解码花了多久来确定声音时间。
        const std::int64_t input_sample =
            has_timestamp ? av_rescale_q(timestamp, time_base_, AVRational{1, input_rate_}) : expected_input_sample_;

        // 源时间戳可能较粗，例如一个刻度为 1 毫秒；换算后有几采样帧误差不一定是跳变。
        // 以一个源时间戳刻度对应的采样帧数作为容差，向上取整且至少为 1。
        const std::int64_t tolerance =
            std::max<std::int64_t>(1, av_rescale_q_rnd(1, time_base_, AVRational{1, input_rate_}, AV_ROUND_UP));

        // 用无符号减法求距离，避免两个极端 int64_t 时间戳直接相减时超出有符号范围。
        const std::uint64_t difference = (input_sample >= expected_input_sample_)
            ? static_cast<std::uint64_t>(input_sample) - static_cast<std::uint64_t>(expected_input_sample_)
            : static_cast<std::uint64_t>(expected_input_sample_) - static_cast<std::uint64_t>(input_sample);

        // 只有已确定起点、当前帧也有时间戳且偏差超过容差时，才认为声音位置不连续。
        const bool discontinuity =
            have_timeline_ && has_timestamp && (difference > static_cast<std::uint64_t>(tolerance));

        if (discontinuity) {
            // 先交付旧段转换时暂存的声音，再重建转换器，避免用旧段的相邻采样计算新段。
            if (!DrainResampler(epoch, output)) {
                return false;
            }
            CreateResampler(*frame_);
            have_timeline_ = false;
        }

        // 第一帧或跳变后的第一帧确定新的输出位置；没有源时间戳时从 0 开始。
        // 这里记录媒体位置，不读取物理时钟，也不决定声卡何时播放。
        if (!have_timeline_) {
            next_output_sample_ = has_timestamp ? av_rescale_q(timestamp, time_base_, kOutputTimeBase) : 0;
            expected_input_sample_ = input_sample;
            have_timeline_ = true;
        }

        // 下一帧的预计位置等于本段累计采样帧数；先检查递增是否超出 int64_t 范围。
        if (expected_input_sample_ > std::numeric_limits<std::int64_t>::max() - frame_->nb_samples) {
            ThrowFfmpeg("audio input timestamp overflow", AVERROR(ERANGE));
        }
        expected_input_sample_ += frame_->nb_samples;

        // swr_get_out_samples 估计本次转换的输出采样帧数上限，包含内部暂存数据；
        // 实际生成数量由 swr_convert 返回，二者不一定相等。负值表示失败。
        const int capacity = swr_get_out_samples(resampler_.get(), frame_->nb_samples);

        // 只有负值才是真正的错误；把 capacity 本身当作错误码传进去会得到误导信息
        if (capacity < 0) {
            ThrowFfmpeg("swr_get_out_samples", capacity);
        }

        // 输出量可以合法地超过输入量（重采样升频，例如 8 kHz -> 48 kHz 约 6 倍），
        // 因此上限按输入上限的 16 倍设置，而不是直接和 nb_samples 的上限比较——
        // 后者会把合法的升频结果判成错误，也让这条检查本身永远无法命中。
        constexpr int kMaxConvertedSamples = 16 * kMaxInputFrameSamples;
        if (capacity > kMaxConvertedSamples) {
            ThrowFfmpeg("validate audio resampling output size", AVERROR(EINVAL));
        }

        // 每个输出采样帧有两个 int16_t 数值；数组采用主机字节序，本机小端时为 S16LE。
        std::vector<std::int16_t> samples(static_cast<std::size_t>(capacity) * PcmFrame::kChannels);
        auto* output_data = reinterpret_cast<std::uint8_t*>(samples.data());

        // Planar（分平面）为每个声道使用一段数组；Packed（交错）把所有声道放在同一数组。
        // extended_data 提供这些数组的借用指针，复制指针不复制声音；AVFrame 仍持有数据引用。
        const int planes = av_sample_fmt_is_planar(input_format_) ? input_layout_.value_.nb_channels : 1;
        std::vector<const std::uint8_t*> input_data(frame_->extended_data, frame_->extended_data + planes);

        // swr_convert 借用输入数组，将结果写到临时输出数组；返回生成的采样帧数，负值是错误。
        // 它不接管 AVFrame 或临时输出数组的所有权，输出为 0 也可以是正常的转换延迟。
        const int produced =
            swr_convert(resampler_.get(), &output_data, capacity, input_data.data(), frame_->nb_samples);
        if (produced < 0) {
            ThrowFfmpeg("swr_convert", produced);
        }

        return Emit(samples.data(), produced, epoch, output);
    }

    /**
     * @brief 循环接收解码器吐出的所有原始音频帧，逐帧完成重采样和分块交付
     * @param epoch 当前输入的播放世代编号（用于 Seek 后识别并抛弃过时数据）
     * @param output 接收最终标准 PCM 块的同步回调函数
     * @param draining 是否处于排空收尾阶段（true 表示正在索取最后的尾音）
     * @return 成功处理的 AVFrame 数量；如果下游回调请求中断返回 -1；发生内部错误则抛出异常
     * @details
     * FFmpeg 的解码器像个吞吐机：送入一个压缩包后，可能吐出 0 帧、1 帧甚至多帧
     * avcodec_receive_frame 返回 0 表示取得一帧，EAGAIN 表示需要更多压缩输入，EOF 表示解码器已排空
     */
    int Receive(std::uint64_t epoch, const Output& output, bool draining) {
        int received = 0;  // 记录本次成功接收并处理了多少个 AVFrame

        for (;;) {
            // 1. 在收新帧之前，先清空/解绑上一轮旧帧对底层内存的引用
            av_frame_unref(frame_.get());

            // 2. 尝试从 FFmpeg 解码器里拿出一帧解压好的原始音频
            const int result = avcodec_receive_frame(avcodec_ctx_.get(), frame_.get());

            // 3. 情况 A：需要先 avcodec_send_packet （EAGAIN）
            if (result == AVERROR(EAGAIN)) {
                // 如果本来就已经进入排空阶段了，却又要求avcodec_send_packet，这属于逻辑矛盾，直接报错
                if (draining) {
                    throw AudioDecodeException(MakeInternalError("Audio receive returned EAGAIN after drain accepted"));
                }
                return received;  // 正常返回目前已经收到的帧数
            }

            // 4. 情况 B：EOF
            if (result == AVERROR_EOF) {
                // 如果正规解码还没走完，就提前报 EOF，说明流程不对，报错拦截
                if (!draining) {
                    throw AudioDecodeException(MakeInternalError("Audio codec reached EOF without ordered drain"));
                }
                return received;  // 正常结束接收
            }

            // 5. 情况 C：返回其他负数，说明解码出了严重错误，直接抛出异常
            if (result < 0) {
                ThrowFfmpeg("avcodec_receive_frame(audio)", result);
            }

            // 6. 成功拿到了一帧音频！计数器加 1
            ++received;

            // 7. 马上把刚拿到的这一帧交个 ConvertFrame 去做重采样、断点检查和分块交付
            const bool converted = ConvertFrame(epoch, output);

            // 8. 转换完成后，立即释放当前帧对内存的引用（用完即焚，不占内存）
            // （注：如果在 ConvertFrame 期间抛出异常，外层的 catch 也会安全执行 unref）
            av_frame_unref(frame_.get());

            // 9. 如果下游回调返回 false（代表队列满了想停止、或者用户点了跳转），立刻终止循环返回 -1
            if (!converted) {
                return -1;
            }
        }
    }
};

/**
 * @brief 构造函数
 */
FfmpegAudioDecoder::FfmpegAudioDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

/**
 * @brief 析构函数
 */
FfmpegAudioDecoder::~FfmpegAudioDecoder() = default;

/**
 * @brief 默认的移动构造函数和移动赋值运算符
 */
FfmpegAudioDecoder::FfmpegAudioDecoder(FfmpegAudioDecoder&&) noexcept = default;
FfmpegAudioDecoder& FfmpegAudioDecoder::operator=(FfmpegAudioDecoder&&) noexcept = default;

/**
 * @brief 创建并初始化音频解码器实例
 * @param parameters 解复用获取的音频流编码参数
 * @param time_base 容器流的时间基准
 * @return 包含初始化完成的解码器对象或错误信息的结果体
 */
FfmpegAudioDecoderOpenResult FfmpegAudioDecoder::Open(AudioCodecParameters parameters, Rational time_base) {
    FfmpegAudioDecoderOpenResult open_result;

    // 1. 校验时间基分子分母及参数对象有效性，防止转换为 int 时发生数值溢出
    if (!parameters.IsValid() || !time_base.IsValid() || time_base.denominator_ > INT_MAX ||
        time_base.numerator_ > INT_MAX) {
        open_result.media_error_ = MakeFfmpegError("validate audio stream codec parameters", AVERROR(EINVAL));

        return open_result;
    }

    // 2. 确认流类型为音频流，防止传入视频流参数
    if (parameters.Get()->codec_type != AVMEDIA_TYPE_AUDIO) {
        open_result.media_error_ = MakeFfmpegError("validate audio stream codec parameters", AVERROR(EINVAL));

        return open_result;
    }

    // 3. 根据编码 ID 检索 FFmpeg 全局静态解码器结构体
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) {
        open_result.media_error_ = MakeFfmpegError("avcodec_find_decoder", AVERROR_DECODER_NOT_FOUND);

        return open_result;
    }

    try {
        // 4. 创建内部实现对象，并分配解码器上下文与工作帧缓存
        auto impl = std::make_unique<Impl>();

        // 申请解码器上下文
        impl->avcodec_ctx_.reset(avcodec_alloc_context3(codec));

        // 申请 AVframe，用于接收解码后的音频
        impl->frame_.reset(av_frame_alloc());
        if (!impl->avcodec_ctx_ || !impl->frame_) {
            open_result.media_error_ = MakeFfmpegError("allocate audio decoder context/frame", AVERROR(ENOMEM));

            return open_result;
        }

        // 5. 将流参数（包括 extradata、采样率、声道配置等）深拷贝至解码器上下文
        const int copied = avcodec_parameters_to_context(impl->avcodec_ctx_.get(), parameters.Get());

        if (copied < 0) {
            open_result.media_error_ = MakeFfmpegError("avcodec_parameters_to_context(auido)", copied);

            return open_result;
        }

        // 6. 保存源时间基
        impl->time_base_ = AVRational{static_cast<int>(time_base.numerator_), static_cast<int>(time_base.denominator_)};

        impl->avcodec_ctx_->pkt_timebase = impl->time_base_;

        // 7. 打开解码器上下文以加载编码器参数并准备解码运行
        const int opened = avcodec_open2(impl->avcodec_ctx_.get(), codec, nullptr);

        if (opened < 0) {
            open_result.media_error_ = MakeFfmpegError("avcodec_open2(audio)", opened);

            return open_result;
        }

        // 8. 构造外部管理对象并移交所有权
        open_result.audio_decoder_ = std::unique_ptr<FfmpegAudioDecoder>(new FfmpegAudioDecoder(std::move(impl)));
    } catch (const std::bad_alloc&) {
        open_result.media_error_ = MediaError::FromSystem(kModuleName, ENOMEM, "alloce audio decoder");
    } catch (const AudioDecodeException& error) {
        open_result.media_error_ = error.Error();
    } catch (const std::exception& error) {
        open_result.media_error_ = MakeInternalError(error.what());
    }

    return open_result;
}

/**
 * @brief 送入待解压的音频压缩包，驱动解码并分块回调输出转换后的 PCM 数据
 * @param packet 接收到的来自 Demuxer 的packet
 * @param output 调用方提供的声音接收函数，每得到一块可交付的声音，就通过它送给下游
 * @return 返回处理状态和错误描述：声音通过 output 交付
 * @details
 * 音频流转： Packet -> 解码后的 AVFrame -> 重采样统一格式 -> PcmFrame -> output
 */
FfmpegAudioDecodeResult FfmpegAudioDecoder::Decode(const Packet& packet, const Output& output) {
    auto error_result = [](MediaError error) {
        return FfmpegAudioDecodeResult{FfmpegAudioDecodeStatus::kError, std::move(error)};
    };

    // 1. 校验内部实例是否存在
    if (!impl_) {
        return error_result(MakeStateError("Audio decoder was moved from"));
    }

    // 2. 解码器若已排空或处于异常中断状态，必须先通过 Flush 重置状态后方可再次解码
    if (impl_->drained_ || impl_->needs_flush_) {
        return error_result(MakeStateError("Audio decoder requires Flush before Decode"));
    }

    // 3. 校验接收回调与输入包的有效性
    if (!output) {
        impl_->needs_flush_ = true;
        return error_result(MakeFfmpegError("validata audio output callback", AVERROR(EINVAL)));
    }

    // 检查音频包是否带有未解码的数据：空包或无效包会作为输入错误返回
    // 正常结束的时候，由节点收到的 EndOfStream 后调用 Drain
    if (!packet.IsValid() || packet.GetAVPacket()->data == nullptr || packet.GetAVPacket()->size < 0) {
        impl_->needs_flush_ = true;

        return error_result(MakeFfmpegError("avcodec_send_packet is invalid", AVERROR(EINVAL)));
    }

    try {
        for (;;) {
            // 1. 将 packet 送入 FFmpeg 解码器
            const int send_result = avcodec_send_packet(impl_->avcodec_ctx_.get(), packet.GetAVPacket());

            // 返回 EAGAIN 表示解码器输入缓冲区已满，需先通过 Receive 取出已解码数据以释放空间
            if (send_result == AVERROR(EAGAIN)) {
                const int received = impl_->Receive(packet.GetEpoch(), output, false);

                // 下游要求停止接收，例如用户停止或跳转，当前处理也应随之结束
                if (received < 0) {
                    return {FfmpegAudioDecodeStatus::kCancelled, {}};
                }

                // 未能取出任何解码帧说明内部状态无进展，判定为死锁异常
                if (received == 0) {
                    impl_->needs_flush_ = true;
                    return error_result(MakeInternalError("Audio send and receive both return EAGAIN"));
                }

                // 空间释放后重试提交当前包
                continue;
            }

            if (send_result < 0) {
                ThrowFfmpeg("avcodec_send_packet(audio)", send_result);
            }

            // 数据包成功提交后，拉取当前产生的所有解码帧并执行格式转换与输出
            const int received = impl_->Receive(packet.GetEpoch(), output, false);

            if (received < 0) {
                return {FfmpegAudioDecodeStatus::kCancelled, {}};
            }

            // 本次能解码完成，可以继续接收下一包
            return {FfmpegAudioDecodeStatus::kOk, {}};
        }
    } catch (const AudioDecodeException& error) {
        // 失败前可能已经送出部分声音，因此不能从头重做同一包，否则可能重复播放
        // 先结束本次处理并报告原因；继续使用前需要清理旧状态，已送出的声音由下游另行处理
        impl_->needs_flush_ = true;
        av_frame_unref(impl_->frame_.get());
        return error_result(error.Error());
    } catch (const std::bad_alloc&) {
        // 存放声音所需内部不足，结束本次处理
        impl_->needs_flush_ = true;
        av_frame_unref(impl_->frame_.get());
        return error_result(MediaError::FromSystem(kModuleName, ENOMEM, "allocate audio PCM buffer"));
    } catch (const std::exception& error) {
        impl_->needs_flush_ = true;
        av_frame_unref(impl_->frame_.get());
        return error_result(MakeInternalError(error.what()));
    } catch (...) {
        impl_->needs_flush_ = true;
        av_frame_unref(impl_->frame_.get());
        return error_result(MakeInternalError("Unknown audio decode/output callback exception"));
    }
}

/**
 * @brief 通知解码器输入结束，排空解码器及重采样器内部暂存的残余数据并完成输出
 * @param epoch 当前播放批次编号
 * @param output 接收 PCM 数据的回调函数
 * @details
 * Drain 模式表示本轮的音频播放已结束，需要将所有的声音播放完毕，因为解码和格式转换都可能暂存少量数据
 * 最后一个音频包处理完，并不代表声音已全部输出，本函数先取完解码器里的声音，再取完格式转换中剩下的部分
 */
FfmpegAudioDecodeResult FfmpegAudioDecoder::Drain(std::uint64_t epoch, const Output& output) {
    auto error_result = [](MediaError error) {
        return FfmpegAudioDecodeResult{FfmpegAudioDecodeStatus::kError, std::move(error)};
    };

    // 1. 实例有效性与前置状态检查
    if (!impl_) {
        return error_result(MakeStateError("Audio decoder was moved from"));
    }

    // 2. 如果解码器已被排空或状态不对
    if (impl_->drained_ || impl_->needs_flush_) {
        return error_result(MakeStateError("Audio decoder required Flush before Drain"));
    }

    // 3. 接收解码后的 PCM 回调函数是否有效
    if (!output) {
        impl_->needs_flush_ = true;
        return error_result(MakeFfmpegError("validata audio drain output callback", AVERROR(EINVAL)));
    }

    try {
        for (;;) {
            // 向解码器发送空指针表示输入结束，进入排空流程
            const int send_result = avcodec_send_packet(impl_->avcodec_ctx_.get(), nullptr);

            // 解码器缓冲区满时同样会返回 EAGAIN，需先收取已解码数据腾出缓冲，再重试发送结束通知
            if (send_result == AVERROR(EAGAIN)) {
                const int received = impl_->Receive(epoch, output, false);

                if (received < 0) {
                    return {FfmpegAudioDecodeStatus::kCancelled, {}};
                }

                if (received == 0) {
                    impl_->needs_flush_ = true;
                    return error_result(MakeInternalError("video drain send/received make no progress"));
                }

                continue;
            }

            if (send_result < 0) {
                ThrowFfmpeg("avcodec_send_packet(audio_drain)", send_result);
            }

            break;
        }

        // 顺序排空：接收解码器内所有的音频数据直到 EOD，再提取重采样器内部暂存数据
        if (impl_->Receive(epoch, output, true) < 0 || !impl_->DrainResampler(epoch, output)) {
            return {FfmpegAudioDecodeStatus::kCancelled, {}};
        }

        // 排空完成
        impl_->drained_ = true;

        return {FfmpegAudioDecodeStatus::kOk, {}};
    } catch (const AudioDecodeException& error) {
        impl_->needs_flush_ = true;
        av_frame_unref(impl_->frame_.get());
        return error_result(error.Error());
    } catch (const std::bad_alloc&) {
        impl_->needs_flush_ = true;
        av_frame_unref(impl_->frame_.get());
        return error_result(MediaError::FromSystem(kModuleName, ENOMEM, "allocate audio drain PCM buffer"));
    } catch (const std::exception& error) {
        impl_->needs_flush_ = true;
        av_frame_unref(impl_->frame_.get());
        return error_result(MakeInternalError(error.what()));
    } catch (...) {
        impl_->needs_flush_ = true;
        av_frame_unref(impl_->frame_.get());
        return error_result(MakeInternalError("Unknown audio drain/output callback exception"));
    }
}

/**
 * @brief 重置解码器状态与重采样上下文（用于 Seek 跳转或错误恢复）
 */
void FfmpegAudioDecoder::Flush() noexcept {
    if (!impl_) {
        return;
    }

    // 丢弃解码器内部所有尚未读取的数据并解绑当前帧引用
    avcodec_flush_buffers(impl_->avcodec_ctx_.get());
    av_frame_unref(impl_->frame_.get());

    // 释放旧的重采样器实例与缓存的声道布局信息
    impl_->resampler_.reset();
    av_channel_layout_uninit(&impl_->input_layout_.value_);
    impl_->input_format_ = AV_SAMPLE_FMT_NONE;

    // 清零时间轴位置，并在后续到达首帧重新锚定基准
    impl_->next_output_sample_ = 0;
    impl_->expected_input_sample_ = 0;
    impl_->have_timeline_ = false;
    impl_->needs_flush_ = false;
    impl_->drained_ = false;
}

}  // namespace mediacore

/**
 * @file audio_decoder.cpp
 * @brief 音频解码与重采样模块核心实现
 * @details 基于 C++11 多线程模型构建，负责压缩位流 (Bitstream) 的提取、
 * 基于 FFmpeg 的硬件/软件解码调度，以及无锁上下文中跨采样率/声道数的重采样 (Resampling)
 */
#include "audio_decoder.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "define/av_frame.h"
#include "define/av_packet.h"
#include "define/stream_info.h"
#include "interface/iaudio_decoder.h"
#include "logger.h"
#include "playback_sync.h"
#include "utils/thread_safe_queue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
}

namespace av {

std::unique_ptr<IAudioDecoder> IAudioDecoder::Create() {
    LOG_INFO("AudioDecoder instance created.");
    return std::make_unique<AudioDecoder>();
}

AudioDecoder::AudioDecoder() {}

AudioDecoder::~AudioDecoder() {
    // 严格遵循时序：先终止 OS 线程，确认无并发访问后再释放底层资源
    Stop();
    Close();
}

/**
 * @brief 注册解码器输出事件监听器
 */
void AudioDecoder::SetListener(Listener* listener) {
    LOG_INFO("AudioDecoder Listener registered.");
    listener_ = listener;
}

/**
 * @brief 初始化音频解码器硬件/软件实例及后处理管线
 * @param info 纯净的音频流媒体数据传输对象 (DTO)
 * @return true 初始化成功；false 初始化失败
 */
bool AudioDecoder::Init(const StreamInfo& info) {
    // 1. 初始化前确保释放历史资源，防止反复 Open 导致的内存泄漏
    Close();

    std::lock_guard<std::mutex> lock(codec_mutex_);

    // 2. 校验入参流元数据 (Stream Metadata) 的有效性
    if (info.codec_id_ <= 0 || info.sample_rate_ <= 0) {
        LOG_ERROR("Invalid StreamInfo: codec_id or sample_rate is missing");
        return false;
    }

    // 3. 确立时间基 (Timebase)，用于后续 PTS/DTS 的绝对时间戳换算
    time_base_.num = info.timebase_num_;
    time_base_.den = info.timebase_den_;

    // 4. 寻址并匹配对应的音频解码器组件
    const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(info.codec_id_));
    if (codec == nullptr) {
        LOG_ERROR("Failed to find codec for CodecID: {}", info.codec_id_);
        return false;
    }

    // 5. 分配解码器上下文实例
    av_codec_ctx_ = avcodec_alloc_context3(codec);
    if (!av_codec_ctx_) {
        LOG_ERROR("Failed to allocate AVCodecContext");
        return false;
    }

    // 6. 显式参数注入与 OOB (Out-of-Band) 全局序列头挂载
    av_codec_ctx_->sample_rate = info.sample_rate_;
    av_codec_ctx_->channels = info.channels_;
    av_codec_ctx_->channel_layout = av_get_default_channel_layout(info.channels_);

    if (!info.extra_data_.empty()) {
        av_codec_ctx_->extradata_size = static_cast<int>(info.extra_data_.size());

        // 使用 FFmpeg 专属内存分配器，并追加 AV_INPUT_BUFFER_PADDING_SIZE 字节的内存安全边界
        // 目的：防范底层 SIMD 汇编优化指令在内存对齐读取时发生越界崩溃 (Segmentation Fault)
        av_codec_ctx_->extradata =
            static_cast<uint8_t*>(av_mallocz(info.extra_data_.size() + AV_INPUT_BUFFER_PADDING_SIZE));

        if (!av_codec_ctx_->extradata) {
            LOG_ERROR("Failed to allocate memory for extradata");
            avcodec_free_context(&av_codec_ctx_);
            return false;
        }
        memcpy(av_codec_ctx_->extradata, info.extra_data_.data(), info.extra_data_.size());
    }

    // 7. 激活解码器实例，触发底层内部状态机与缓存的初始化
    if (avcodec_open2(av_codec_ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&av_codec_ctx_);
        LOG_ERROR("Failed to open AVCodec");
        return false;
    }

    // 8. 构建音频重采样管线 (Resample Pipeline)
    int64_t in_ch_layout = static_cast<int64_t>(av_codec_ctx_->channel_layout);
    if (in_ch_layout == 0) {
        in_ch_layout = av_get_default_channel_layout(av_codec_ctx_->channels);
    }
    int64_t out_ch_layout = av_get_default_channel_layout(out_channels_);

    // 配置 SwrContext，确立采样率转换、位深转换及通道混合矩阵映射
    swr_ctx_ = swr_alloc_set_opts(nullptr, out_ch_layout, out_sample_fmt_, out_sample_rate_, in_ch_layout,
                                  av_codec_ctx_->sample_fmt, av_codec_ctx_->sample_rate, 0, nullptr);

    if (!swr_ctx_) {
        LOG_ERROR("Failed to allocate SwrContext");
        avcodec_free_context(&av_codec_ctx_);
        return false;
    }

    if (swr_init(swr_ctx_) < 0) {
        LOG_ERROR("Failed to initialize SwrContext");
        swr_free(&swr_ctx_);
        avcodec_free_context(&av_codec_ctx_);
        return false;
    }

    LOG_INFO("Audio Decoder Init Success. Resampler pipeline: {}Hz, {} channels.", out_sample_rate_, out_channels_);
    return true;
}

/**
 * @brief 接收上游投递的压缩数据包
 */
void AudioDecoder::PushPacket(std::shared_ptr<Packet> packet) {
    if (!packet) {
        LOG_ERROR("NO Recv AAC Frame  from Demuxer");
        return;
    }
    packet_queue_.Push(std::move(packet));
}

/**
 * @brief 通知解码器数据流物理结束 (EOF)
 */
void AudioDecoder::SendEOF() {
    LOG_INFO("Audio decoder received EOF signal, enqueuing empty packet.");
    // 构造空数据包 (Empty Packet) 投递至队列尾部
    // 底层 avcodec_send_packet 接收到空包时，将触发解码器的 Drain (排空) 模式
    auto empty_packet = std::make_shared<Packet>();
    packet_queue_.Push(std::move(empty_packet));
}

/**
 * @brief 启动异步解码工作线程
 */
void AudioDecoder::Start(std::shared_ptr<SyncContext> sync_ctx) {
    // 1. 物理线程状态校验：防止对活动线程句柄进行覆盖赋值触发 std::terminate
    if (decode_thread_.joinable()) {
        LOG_WARN("Audio Decoder thread is already running.");
        return;
    }

    // 2. 清空历队列包
    packet_queue_.Clear();
    packet_queue_.Start();

    // 3. 生命周期状态复位
    is_abort_requested_.store(false);

    // 4. 保存全局状态同步上下文
    sync_ctx_ = std::move(sync_ctx);

    // 5. 实例化并启动 OS 级解码线程
    decode_thread_ = std::thread(&AudioDecoder::DecodeLoop, this);
    LOG_INFO("Audio Decoder OS thread spawned successfully.");
}

/**
 * @brief 终止解码线程
 */
void AudioDecoder::Stop() {
    // 1. 若已处于终止请求状态，避免重复操作
    if (is_abort_requested_.load()) {
        return;
    }

    // 2. 下发中断带外信令
    is_abort_requested_.store(true);

    // 3. 解除可能由全局状态机 kPaused 引起的条件变量挂起
    if (sync_ctx_ && sync_ctx_->IsValid()) {
        sync_ctx_->state_cv_.notify_all();
    }

    // 4. 强行中断可能阻塞在数据队列 Push/Pop 操作上的线程
    packet_queue_.Abort();

    // 5. 同步等待 OS 线程指令流执行完毕并回收句柄
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    // 6. 状态复位与残余数据清理
    is_abort_requested_.store(false);
    packet_queue_.Clear();
    sync_ctx_.reset();

    LOG_INFO("AudioDecoder OS thread joined safely.");
}

/**
 * @brief 冲洗解码器内部参考帧缓存
 */
void AudioDecoder::Flush() {
    // 1. 废弃所有尚未送入解码器的压缩包
    packet_queue_.Clear();

    // 2. 冲洗 FFmpeg 内部缓存，防范时间轴跳转 (Seek) 后的状态污染
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (av_codec_ctx_) {
        avcodec_flush_buffers(av_codec_ctx_);
    }
    LOG_INFO("Audio Decoder internal buffers flushed.");
}

/**
 * @brief 释放 FFmpeg 解码器与重采样上下文物理内存
 */
void AudioDecoder::Close() {
    std::lock_guard<std::mutex> lock(codec_mutex_);

    if (av_codec_ctx_) {
        avcodec_free_context(&av_codec_ctx_);
        av_codec_ctx_ = nullptr;
    }
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
    packet_queue_.Clear();
}

/**
 * @brief 音频解码与重采样核心事件循环 (Event Loop)
 */
void AudioDecoder::DecodeLoop() {
    LOG_INFO("AudioDecoder DecodeLoop activated.");

    // 分配解码输出的复用物理内存载体，避免循环内高频内存分配
    AVFrame* av_frame = av_frame_alloc();
    if (!av_frame) {
        LOG_ERROR("Failed to allocate raw AVFrame container.");
        return;
    }

    while (!is_abort_requested_.load()) {
        // [阶段 A]: 并发挂起与状态机同步控制
        {
            std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
            sync_ctx_->state_cv_.wait(lock, [this]() {
                // 条件屏障：响应终止请求，或在播放状态下持续运转
                return is_abort_requested_.load() || sync_ctx_->state_.load() == PlaybackState::kPlaying;
            });
        }

        // 唤醒后的生命周期确认，防止处于挂起时被下发终止信令
        if (is_abort_requested_.load()) {
            break;
        }

        // [阶段 B]: 阻塞式提取比特流 (Blocking Bitstream Extraction)
        std::shared_ptr<Packet> packet;
        if (!packet_queue_.Pop(packet)) {
            LOG_INFO("Audio packet queue aborted. Exiting DecodeLoop.");
            break;
        }

        if (!packet) {
            continue;
        }

        // [阶段 C]: 带内信令处理 (In-band Control Signaling)
        if (packet->HasFlag(av::AVFrameFlag::kFlush)) {
            LOG_INFO("Received In-band Flush packet, executing codec synchronization.");
            std::lock_guard<std::mutex> lock(codec_mutex_);
            avcodec_flush_buffers(av_codec_ctx_);
            continue;
        }

        // [阶段 D]: 投递压缩载荷 (Submit Payload)
        int ret = 0;
        {
            std::lock_guard<std::mutex> lock(codec_mutex_);
            ret = avcodec_send_packet(av_codec_ctx_, packet->GetRawPacket());
        }

        // 放行 AVERROR_EOF，允许循环抽干内部缓存
        if (ret < 0 && ret != AVERROR_EOF) {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> err_buf{};
            av_strerror(ret, err_buf.data(), err_buf.size());
            LOG_ERROR("avcodec_send_packet failed: {}", err_buf.data());
            continue;
        }

        // [阶段 E]: 贪婪模式抽取 (Greedy Extraction)
        while (true) {
            {
                std::lock_guard<std::mutex> lock(codec_mutex_);
                ret = avcodec_receive_frame(av_codec_ctx_, av_frame);
            }

            // 评估解码引擎状态机反馈
            if (ret == AVERROR(EAGAIN)) {
                // 内部参考帧不足，要求补充新 AVPacket
                break;
            } else if (ret == AVERROR_EOF) {
                // 解码管线彻底排空
                if (listener_) {
                    LOG_INFO("Audio decoding reached physical EOF.");
                    listener_->OnNotifyAudioEOF();
                }
                break;
            } else if (ret < 0) {
                // 底层硬件或解码库发生不可恢复异常
                std::array<char, AV_ERROR_MAX_STRING_SIZE> err_buf{};
                av_strerror(ret, err_buf.data(), err_buf.size());
                LOG_ERROR("avcodec_receive_frame failed: {}", err_buf.data());
                break;
            }

            // [阶段 F]: 执行格式适配与重采样 (Resampling)
            ProcessAndResample(av_frame);

            // [阶段 G]: 剥离并释放内存引用，备用下一轮抽取
            av_frame_unref(av_frame);
        }
    }

    // 销毁物理内存载体，防范堆内存泄漏
    av_frame_free(&av_frame);

    LOG_INFO("AudioDecoder DecodeLoop exited safely.");
}

/**
 * @brief 执行音频重采样与所有权移交
 */
void AudioDecoder::ProcessAndResample(AVFrame* av_frame) {
    if (!av_frame || !swr_ctx_) {
        LOG_ERROR("Invalid input frame or uninitialized SwrContext.");
        return;
    }

    // 1. 评估重采样延时，预估目标内存容量
    int64_t delay = swr_get_delay(swr_ctx_, av_frame->sample_rate);
    int64_t out_nb_samples =
        av_rescale_rnd(delay + av_frame->nb_samples, out_sample_rate_, av_frame->sample_rate, AV_ROUND_UP);

    // 2. 分配目标物理帧 (Target Physical Frame)
    AVFrame* resampled_frame = av_frame_alloc();
    if (!resampled_frame) {
        LOG_ERROR("Failed to allocate resampled AVFrame.");
        return;
    }

    // 注入目标声卡拓扑结构签名
    resampled_frame->sample_rate = out_sample_rate_;
    resampled_frame->format = out_sample_fmt_;
    resampled_frame->channels = out_channels_;
    resampled_frame->channel_layout = av_get_default_channel_layout(out_channels_);
    resampled_frame->nb_samples = static_cast<int>(out_nb_samples);

    if (av_frame_get_buffer(resampled_frame, 0) < 0) {
        LOG_ERROR("Failed to allocate data buffer for resampled AVFrame.");
        av_frame_free(&resampled_frame);
        return;
    }

    // 3. 驱动重采样引擎执行转换
    int actual_out_samples = swr_convert(swr_ctx_, resampled_frame->data, resampled_frame->nb_samples,
                                         (const uint8_t**)av_frame->data, av_frame->nb_samples);

    if (actual_out_samples < 0) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> err_buf{};
        av_strerror(actual_out_samples, err_buf.data(), err_buf.size());
        LOG_ERROR("swr_convert failed: {}", err_buf.data());
        av_frame_free(&resampled_frame);
        return;
    }

    // 4. 校正实际样本数并继承呈现时间戳 (PTS)
    resampled_frame->nb_samples = actual_out_samples;
    resampled_frame->pts = av_frame->pts;

    // 5. 跨系统边界所有权移交：将 C 指针移交至 RAII 智能资产容器，交由下游接管生命周期
    auto final_frame = std::make_shared<av::Frame>(resampled_frame);

    if (listener_) {
        listener_->OnNotifyAudioFrame(std::move(final_frame));
    }
}

} // namespace av
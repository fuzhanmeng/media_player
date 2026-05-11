/**
 * @file audio_decoder.cpp
 * @brief 音频解码与重采样模块的实现
 */
#include "audio_decoder.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "data/av_frame.h"
#include "data/av_packet.h"
#include "interface/iaudio_decoder.h"
#include "logger.h"
#include "utils/thread_safe_queue.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
}

namespace av {
class Frame;
class Packet;

AudioDecoder::AudioDecoder() { LOG_INFO("Call AudioDecoder::AudioDecoder"); }

AudioDecoder::~AudioDecoder() { Stop(); }

// 注册监听器，用于接收解压后的原始音频数据
void AudioDecoder::SetListener(Listener* listener) { audio_decoder_listener_ = listener; }

// 初始化解码器，传入从 Demuxer 获取的音频流信息
bool AudioDecoder::Init(AVStream* stream) {
    std::lock_guard<std::mutex> lock(codec_mutex_);

    // 1. 判断传过来的stream info 是否有问题
    if (!stream || !stream->codecpar) {
        LOG_ERROR("The stream is null");
        return false;
    }

    // 2. 获取时间基
    time_base_ = stream->time_base;

    // 3. 找到音频解码器
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        LOG_ERROR("find codec fail");
        return false;
    }

    // 4. 创造解码器上下文
    av_codec_ctx_ = avcodec_alloc_context3(codec);
    if (!av_codec_ctx_) {
        LOG_ERROR("Create av_codec_ctx fail");
        return false;
    }

    // 5. 从媒体文件里读出来的流属性拷贝解码器上下文
    if (avcodec_parameters_to_context(av_codec_ctx_, stream->codecpar) < 0) {
        avcodec_free_context(&av_codec_ctx_);
        LOG_ERROR("copy stream codecpar fail");
        return false;
    }

    // 6. 打开解码器
    if (avcodec_open2(av_codec_ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&av_codec_ctx_);
        LOG_ERROR("open codec fail");
        return false;
    }

    // 7. Resample 配置
    // 计算原始数据的声道布局
    int64_t in_ch_layout = static_cast<int64_t>(av_codec_ctx_->channel_layout);
    if (in_ch_layout == 0) {
        in_ch_layout = av_get_default_channel_layout(av_codec_ctx_->channels);
    }

    // 计算我们目标输出的声道布局
    int64_t out_ch_layout = av_get_default_channel_layout(out_channels_);

    swr_ctx_ =
        swr_alloc_set_opts(nullptr, out_ch_layout, out_sample_fmt_, out_sample_rate_, // 我们的目标 (出水管)
                           in_ch_layout, av_codec_ctx_->sample_fmt, av_codec_ctx_->sample_rate, // FFmpeg解码后 (进水管)
                           0, nullptr);

    if (!swr_ctx_) {
        LOG_ERROR("Failed to allocate SwrContext");
        avcodec_free_context(&av_codec_ctx_);
        return false;
    }

    // 真正初始化重采样器
    if (swr_init(swr_ctx_) < 0) {
        LOG_ERROR("Failed to initialize SwrContext");
        swr_free(&swr_ctx_);
        avcodec_free_context(&av_codec_ctx_);
        return false;
    }

    LOG_INFO("Audio Decoder Init Success! Resampler configured to {}Hz, {} channels.", out_sample_rate_, out_channels_);

    return true;
}

// 接收Demuxer发送过来的压缩数据包
void AudioDecoder::PushPacket(std::shared_ptr<Packet> packet) {
    if (!packet) {
        return;
    }
    LOG_INFO("recv a packet");
    packet_queue_.Push(std::move(packet));
}

/**
 * @brief 通知解码器输入结束，输出缓冲区中剩余的所有帧
 */
void AudioDecoder::SendEOF() {
    LOG_INFO("Audio decoder received EOF signal, pushing EOF packet to queue");

    // 不要直接跨线程操作 FFmpeg
    // 制造一个"空包裹"(底层 AVPacket 没有实际数据),把它塞进任务队列的尾部排队
    // 等 DecodeLoop 按顺序处理到这个空包裹时，它自己会在安全的线程里去调用 avcodec_send_packet

    auto eof_packet = std::make_shared<Packet>();

    packet_queue_.Push(std::move(eof_packet));
}
// 启动 / 恢复解码线程
void AudioDecoder::Start() {
    if (is_running_.load()) {
        // 如果已经在运行，且处于暂停状态，则唤醒它 (恢复播放)
        if (is_paused_.load()) {
            {
                std::lock_guard<std::mutex> lock(pause_mutex_);
                is_paused_.store(false);
            }
            pause_cv_.notify_all(); // 换醒 DecodeLoop 中沉睡的线程
            LOG_INFO("Audio Decoder resumed from pause.");
        } else {
            LOG_WARN("Audio Decoder is already running!");
        }
        return;
    }

    // 首次启动逻辑
    is_running_.store(true);
    is_paused_.store(false);

    // 启动真正的解码线程
    decode_thread_ = std::thread(&AudioDecoder::DecodeLoop, this);
    LOG_INFO("Audio Decoder thread started successfully.");
}

// 暂停解码
void AudioDecoder::Pause() {
    std::lock_guard<std::mutex> lock(pause_mutex_);
    is_paused_.store(true);
    LOG_INFO("Audio Decoder paused.");
}

// 停止解码线程并释放相关资源
void AudioDecoder::Stop() {
    // 1. 改变运行状态，让 DecodeLoop 知道该退出了
    is_running_.store(false);

    // 2. 解除可能的暂停状态，防止线程死锁在 pause_cv_.wait 里出不来
    {
        std::lock_guard<std::mutex> lock(pause_mutex_);
        is_paused_.store(false);
    }
    pause_cv_.notify_all(); // 唤醒可能正在暂停休眠的线程

    // 3. 强行唤醒阻塞在任务队列里的线程
    packet_queue_.Abort();

    // 4. 等待子线程安全退出
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    // 5. 释放 FFmpeg 内存
    if (av_codec_ctx_) {
        avcodec_free_context(&av_codec_ctx_);
    }
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
    }
    if (audio_decoder_listener_) {
        audio_decoder_listener_ = nullptr;
    }

    LOG_INFO("Audio Decoder stopped and resources freed.");
}

// 清洗解码器内部的缓存 (通常在 Seek 跳转时调用)
void AudioDecoder::Flush() {
    // 1. 清空任务队列
    packet_queue_.Clear();

    // 2. 清空 FFmpeg 解码器内部的隐藏缓存
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (av_codec_ctx_) {
        avcodec_flush_buffers(av_codec_ctx_);
    }
    LOG_INFO("Audio Decoder flushed. All internal buffers cleared.");
}

// 解码与重采样的后台死循环，运行在 decode_thread_ 中
void AudioDecoder::DecodeLoop() {
    LOG_INFO("Start AudioDecoder DecodeLoop");

    // 1. 申请一个空的av_frame用于接收解码后的frame
    AVFrame* av_frame = av_frame_alloc();

    if (av_frame) {
        LOG_ERROR("Failed to allocate AVFrame");
    }

    // 2. 外层循环
    while (is_running_.load()) {
        // 阶段 A：暂停状态挂起
        {
            std::unique_lock<std::mutex> lock(pause_mutex_);
            pause_cv_.wait(lock, [this]() {
                // 当处于非暂停状态，或者线程被请求终止时，解除阻塞
                return !is_paused_.load() || !is_running_.load();
            });
        }

        // 唤醒后再次检查运行状态，防止因 Stop() 被唤醒而继续执行
        if (!is_running_.load()) {
            break;
        }

        // 阶段 B：阻塞式提取数据包
        std::shared_ptr<Packet> packet; // 声明空智能指针作为接收载体
        if (!packet_queue_.Pop(packet)) {
            LOG_INFO("Packet queue aborted, exiting DecodeLoop");
            break;
        }

        // 阶段 C：处理跳转 (Seek) 同步信号
        if (packet->HasFlag(av::AVFrameFlag::kFlush)) {
            LOG_INFO("Received Flush packet, clearing codec buffers");

            std::lock_guard<std::mutex> lock(codec_mutex_);
            avcodec_flush_buffers(av_codec_ctx_);
            continue;
        }

        // 阶段 D：将压缩数据提交至解码器上下文
        int ret = 0;
        {
            std::lock_guard<std::mutex> lock(codec_mutex_);
            // 提取底层 AVPacket*，如果是 EOF 标志包，GetRawPacket() 可能返回 nullptr
            avcodec_send_packet(av_codec_ctx_, packet->GetRawPacket());
        }

        if (ret < 0) {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> err_buf{};
            av_strerror(ret, err_buf.data(), err_buf.size());
            LOG_ERROR("avcodec_send_packet failed: {}", err_buf.data());
            continue;
        }

        // 阶段 E：内层循环，提取所有解码完成的帧
        while (ret >= 0) {
            {
                std::lock_guard<std::mutex> lock(codec_mutex_);
                ret = avcodec_receive_frame(av_codec_ctx_, av_frame);
            }

            // 状态机判断：解码器的输出缓冲区已空，或者解码彻底结束(文件结束)
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                if (ret == AVERROR_EOF && audio_decoder_listener_) {
                    LOG_INFO("Audio decoding reached EOF");
                    audio_decoder_listener_->OnNotifyAudioEOF();
                }
                break;
            } else if (ret < 0) {
                std::array<char, AV_ERROR_MAX_STRING_SIZE> err_buf{};
                av_strerror(ret, err_buf.data(), err_buf.size());
                break;
            }
            // 阶段 F：处理成功解码的 AVFrame (重采样与抛出)
            ProcessAndResample(av_frame);

            av_frame_unref(av_frame);
        }
    }
    // 3. 循环结束，释放结构体内存
    av_frame_free(&av_frame);
    LOG_INFO("AudioDecoder DecodeLoop exited safely");
}

void AudioDecoder::ProcessAndResample(AVFrame* decodec_frame) {
    // 先判断传过来的frame是否为空, swr_ctx_ 采重样是否为空
    if (!decodec_frame || !swr_ctx_) {
        return;
    }

    // 步骤 1：计算重采样后的目标样本数 (Target Sample Count)
    // 因为输入和输出的采样率不同 (例如 48kHz -> 44.1kHz)，样本数量会发生变化
    // swr_get_delay 获取的就是当前重采样器内部缓存了多少个尚未输出的滞留样本点
    int64_t delay = swr_get_delay(swr_ctx_, decodec_frame->sample_rate);

    // av_rescale_rnd 执行防溢出的分数乘法: (delay + in_samples) * out_sample_rate / in_sample_rate AV_ROUND_UP(向上取整模式)
    // av_rescale_rnd这个函数用于预估，在完成采样率转换后，我们需要为目标 PCM 帧分配能容纳多少个 nb_samples（样本数）的内存空间
    int64_t out_nb_samples =
        av_rescale_rnd(delay + decodec_frame->nb_samples, out_sample_rate_, decodec_frame->sample_rate, AV_ROUND_UP);

    // 步骤 2：分配输出的 AVFrame 及其数据缓冲区,resampled_frame存放重采样（格式转换）完成后的目标 PCM 数据
    AVFrame* resampled_frame = av_frame_alloc();
    if (!resampled_frame) {
        LOG_ERROR("Failed to allocate resampled AVFrame");
        return;
    }

    // 设置目标帧的参数规格
    resampled_frame->sample_rate = out_sample_rate_;  // 目标采样率
    resampled_frame->format = out_sample_fmt_; // 目标采样格式
    resampled_frame->channels = out_channels_; // 目标通道数
    resampled_frame->channel_layout = av_get_default_channel_layout(out_channels_);

    // 这个目标帧，准备存放刚刚用 av_rescale_rnd 计算出来的那么多个样本点
    resampled_frame->nb_samples = static_cast<int>(out_nb_samples);

    if (av_frame_get_buffer(resampled_frame, 0) < 0) {
        LOG_ERROR("Failed to allocate data buffer for resampled AVFrame");
        av_frame_free(&resampled_frame);
        return;
    }

    // 步骤 3：执行核心重采样运算
    // swr_convert 将源格式的 PCM 数据转化为目标格式，并写入 resampled_frame->data
    int actual_out_samples = swr_convert(swr_ctx_, resampled_frame->data, resampled_frame->nb_samples,
                                         (const uint8_t**)decodec_frame->data, decodec_frame->nb_samples);

    if (actual_out_samples < 0) {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> err_buf{};
        av_strerror(actual_out_samples, err_buf.data(), err_buf.size());
        LOG_ERROR("swr_convert failed: {}", err_buf.data());
        av_frame_free(&resampled_frame);
    }

    // 步骤 4：更新实际样本数与时间戳传递
    // 实际转换出的样本数可能少于我们预估的最大容量，必须修正它，否则下游播放会产生杂音
    resampled_frame->nb_samples = actual_out_samples;

    // 继承源音频帧的时间戳 (PTS)，供音视频同步使用
    resampled_frame->pts = decodec_frame->pts;

    // 步骤 5：移交内存所有权，封装并推给下游
    auto final_frame = std::make_shared<av::Frame>(resampled_frame);

    if (audio_decoder_listener_) {
        audio_decoder_listener_->OnNotifyAudioFrame(final_frame);
    }
}

std::unique_ptr<IAudioDecoder> IAudioDecoder::Create() { return std::make_unique<AudioDecoder>(); }
} // namespace av
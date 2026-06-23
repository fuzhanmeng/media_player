/**
 * @file audio_encoder.cpp
 * @brief 音频编码实现
 * @note av_encodec_ctx_      编码器             把 FLTP 原始帧 → AAC 压缩包
         swr_ctx_             格式转换器          把 S16 原始数据 → FLTP 原始数据
         fltp_frame_          内存容器（缓冲区）   暂存转换后的 FLTP 数据，供编码器消费

    audio_frame (PCM/S16)
        │
        ▼  swr_convert(swr_ctx_, ...)
    fltp_frame_ (FLTP数据填充进来)
        │
        ▼  avcodec_send_frame(av_encodec_ctx_, fltp_frame_)  fltp_frame_输入的原始帧
    编码器内部缓冲
        │
        ▼  avcodec_receive_packet(av_encodec_ctx_, pkt) pkt盒子存放AAC
    AVPacket (AAC压缩数据)
 */

#include "audio_encoder.h"

#include "define/av_frame.h"
#include "define/av_packet.h"
#include "encode/interface/iaudio_encoder.h"
#include "logger.h"

namespace av {
/**
 * @brief 创建编码器实例
 */
std::unique_ptr<IAudioEncoder> IAudioEncoder::Create() { return std::make_unique<AudioEncoder>(); }

/**
 * @brief 构造函数
 */
AudioEncoder::AudioEncoder() {}

/**
 * @brief 析构函数
 */
AudioEncoder::~AudioEncoder() { Close(); }

/**
 * @brief 设置监听回调
 */
void AudioEncoder::SetListener(Listener* listener) { listener_ = listener; }

/**
 * @brief 编码器初始化
 */
bool AudioEncoder::Init(int sample_rate, int channels, int bit_rate) {
    // 幂等：若已初始化（重复录制场景），先释放上一轮上下文，避免 AVCodecContext/SwrContext 泄漏
    Close();

    // 1. 查找 AAC 编码器
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        LOG_ERROR("Find Audio Encoder failed");
        return false;
    }

    // 2. 申请编码器上下文
    av_encodec_ctx_ = avcodec_alloc_context3(codec);
    if (!av_encodec_ctx_) {
        LOG_ERROR("alloc av_codec_ctx failed");
        return false;
    }

    // 3. 设置参数
    av_encodec_ctx_->codec_id = AV_CODEC_ID_AAC;
    av_encodec_ctx_->sample_rate = sample_rate; // 采样率，即每秒采集多少个音频采样点
    av_encodec_ctx_->channels = channels;
    av_encodec_ctx_->channel_layout = av_get_default_channel_layout(channels); // 声道布局
    av_encodec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP; // 采样格式 FLTP，平面式，各声道分开存储
    av_encodec_ctx_->bit_rate = bit_rate;

    // time_base 必须显式设为 {1, sample_rate}：Encoder() 的 PTS 以"采样点累计数"为单位自增，
    // Muxer 依赖此 time_base 做 av_packet_rescale_ts 换算。若不设置，部分编码器留空 {0,1}，
    // 导致写入 MP4 的音频时间戳错乱。
    av_encodec_ctx_->time_base = {1, sample_rate};

    // 4. 打开编码器
    if (avcodec_open2(av_encodec_ctx_, codec, nullptr) < 0) {
        LOG_ERROR("open codec failed");
        avcodec_free_context(&av_encodec_ctx_);
        return false;
    }

    // 5. 建立SwrContext，SwrContext是一个音频格式转换器，本身不保存音频数据，只保存转换规则
    swr_ctx_ = swr_alloc_set_opts(nullptr, // 新建转化器
                                           // 输出规则
                                  static_cast<int64_t>(av_encodec_ctx_->channel_layout), // 输出声道布局
                                  AV_SAMPLE_FMT_FLTP, // 输出格式 FLTP
                                  av_encodec_ctx_->sample_rate, // 输出采样率
                                  // 输入规则
                                  av_get_default_channel_layout(channels), // 输入声道布局
                                  AV_SAMPLE_FMT_S16, // 输入格式 S16（SDL给的PCM）
                                  sample_rate, // 输入采样率
                                  0, nullptr);

    if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
        LOG_ERROR("swr init failed");
        avcodec_free_context(&av_encodec_ctx_);
        return false;
    }

    pts_ = 0;

    // 6. 创建 FLTP 目标帧（成员变量，在每次 Encoder() 调用中复用，Close() 统一释放）
    fltp_frame_ = av_frame_alloc();
    if (!fltp_frame_) {
        LOG_ERROR("av_frame_alloc failed");
        avcodec_free_context(&av_encodec_ctx_);
        swr_free(&swr_ctx_);
        return false;
    }
    fltp_frame_->sample_rate = av_encodec_ctx_->sample_rate;
    fltp_frame_->format = AV_SAMPLE_FMT_FLTP;
    fltp_frame_->channels = av_encodec_ctx_->channels;
    fltp_frame_->channel_layout = av_encodec_ctx_->channel_layout;
    fltp_frame_->nb_samples = av_encodec_ctx_->frame_size;
    if (av_frame_get_buffer(fltp_frame_, 0) < 0) {
        LOG_ERROR("av_frame_get_buffer failed");
        av_frame_free(&fltp_frame_);
        avcodec_free_context(&av_encodec_ctx_);
        swr_free(&swr_ctx_);
        return false;
    }

    return true;
}

/**
 * @brief 编码一帧 PCM：S16 重采样为 FLTP，送入编码器，输出 AAC 包经 Listener 回调
 * @param audio_frame 输入 PCM 帧（S16 交错格式），不得为空
 * @note 与 Flush() 共用 encoder_muxter_ 串行化对 av_encodec_ctx_/swr_ctx_ 的访问
 */
void AudioEncoder::Encoder(std::shared_ptr<Frame> audio_frame) {
    std::lock_guard<std::mutex> lock(encoder_muxter_);

    if (!av_encodec_ctx_ || !audio_frame) {
        return;
    }

    // 1. 复用 fltp_frame_，写入前确保缓冲可写（引用计数保护）
    av_frame_make_writable(fltp_frame_);

    // swr_convert 执行 S16 → FLTP 格式转换，结果写入 fltp_frame_->data
    swr_convert(swr_ctx_,
                fltp_frame_->data, // 输出：转换结果写入 fltp_frame_->data
                fltp_frame_->nb_samples, // 输出采样上限
                (const uint8_t**)audio_frame->GetRawFrame()->data, // 输入：S16 原始 PCM
                audio_frame->GetRawFrame()->nb_samples // 输入采样数
    );

    // 2. 设置 PTS（内部自增的采样点累计数，不依赖输入帧 PTS，保证 seek 后录制时间轴连续）
    fltp_frame_->pts = pts_;
    pts_ += fltp_frame_->nb_samples; // 累加本帧采样数

    // 3. 送入编码器
    avcodec_send_frame(av_encodec_ctx_, fltp_frame_);

    // 4. 取出编码后的包；返回 EAGAIN 表示编码器需更多输入帧，退出等待下一帧
    while (true) {
        AVPacket* raw_pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(av_encodec_ctx_, raw_pkt);
        if (ret == 0) {
            // 成功取出一个包，Packet 接管 raw_pkt 所有权
            auto packet = std::make_shared<Packet>(raw_pkt);
            if (listener_) {
                listener_->OnAudioEncoderNotifyPacket(packet);
            }
        } else {
            // EAGAIN：编码器缓冲不足，属正常现象，退出等待下一帧
            av_packet_free(&raw_pkt);
            break;
        }
    }
}

/**
 * @brief 冲洗编码缓冲区
 * @note 触发drain模式：发送nullptr让编码器输出所有缓冲中的剩余包，录制结束时调用
 */
void AudioEncoder::Flush() {
    std::lock_guard<std::mutex> lock(encoder_muxter_);

    // 发送nullptr触发drain模式，编码器不再接受新帧，开始输出剩余缓冲
    avcodec_send_frame(av_encodec_ctx_, nullptr);

    // 循环取出剩余的AAC包，直到AVERROR_EOF表示彻底排空
    while (true) {
        AVPacket* raw_pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(av_encodec_ctx_, raw_pkt);
        if (ret == 0) {
            auto packet = std::make_shared<Packet>(raw_pkt);
            if (listener_) {
                listener_->OnAudioEncoderNotifyPacket(packet);
            }
        } else {
            // AVERROR_EOF：编码器彻底排空，退出
            av_packet_free(&raw_pkt);
            break;
        }
    }

    if (listener_) {
        listener_->OnAudioEncoderNotifyFinished();
    }
}

/**
 * @brief 关闭编码器，释放资源
 */
void AudioEncoder::Close() {
    if (av_encodec_ctx_) {
        avcodec_free_context(&av_encodec_ctx_);
    }

    if (swr_ctx_) {
        swr_free(&swr_ctx_);
    }

    if (fltp_frame_) {
        av_frame_free(&fltp_frame_);
    }

    av_encodec_ctx_ = nullptr;
    swr_ctx_ = nullptr;
    fltp_frame_ = nullptr;
}

/**
 * @brief 获取编码器上下文，供Muxer通过avcodec_parameters_from_context写入MP4文件头
 */
AVCodecContext* AudioEncoder::GetCodecContext() const { return av_encodec_ctx_; }
} // namespace av

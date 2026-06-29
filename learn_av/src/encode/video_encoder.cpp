/**
 * @file video_encoder.cpp
 * @brief 视频编码实现
 * @note 数据流转：
 *   VideoFrame (YUV420P裸数据)
 *       │
 *       ▼  包装为 AVFrame 壳子（不拷贝像素数据，直接复用指针）
 *   AVFrame* yuv_frame
 *       │
 *       ▼  avcodec_send_frame(av_encodec_ctx_, yuv_frame)
 *   编码器内部缓冲
 *       │
 *       ▼  avcodec_receive_packet(av_encodec_ctx_, pkt)
 *   AVPacket (H.264压缩数据)
 */

#include "video_encoder.h"

#include <mutex>

#include "logger.h"
#include "video_frame.h"

extern "C" {
#include <libavutil/opt.h>
}

namespace av {

/**
 * @brief 创建视频编码实例
 */
std::unique_ptr<IVideoEncoder> IVideoEncoder::Create() { return std::make_unique<VideoEncoder>(); }

/**
 * @brief 构造函数
 */
VideoEncoder::VideoEncoder() {}

/**
 * @brief 析构函数，调用Close统一释放资源
 */
VideoEncoder::~VideoEncoder() { Close(); }

/**
 * @brief 设置监听回调
 */
void VideoEncoder::SetListener(Listener* listener) { listener_ = listener; }

/**
 * @brief 初始化视频编码器
 * @note AV_CODEC_FLAG_GLOBAL_HEADER：MP4格式要求SPS/PPS写入extradata而非嵌在包里
 *       av_opt_set preset：编码速度与压缩率的平衡，必须在avcodec_open2之前设置
 */
bool VideoEncoder::Init(int width, int height, int fps, int bit_rate) {
    // 幂等：若已初始化（重复录制场景），先释放上一轮上下文，避免 AVCodecContext 泄漏
    Close();
    pts_ = 0; // 复位 PTS 计数器，保证新一轮录制从 0 开始

    // 1. 查找 H.264 编码器
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        LOG_ERROR("Find H264 Codec failed");
        return false;
    }

    // 2. 申请编码器上下文
    av_encodec_ctx_ = avcodec_alloc_context3(codec);
    if (av_encodec_ctx_ == nullptr) {
        LOG_ERROR("avcodec alloc failed");
        return false;
    }

    // 3. 设置编码参数
    av_encodec_ctx_->codec_id = AV_CODEC_ID_H264;
    av_encodec_ctx_->width = width;
    av_encodec_ctx_->height = height;
    av_encodec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    av_encodec_ctx_->bit_rate = bit_rate;
    av_encodec_ctx_->framerate = {fps, 1}; // 帧率
    av_encodec_ctx_->time_base = {1, fps}; // PTS单位，必须是framerate的倒数
    av_encodec_ctx_->gop_size = 12; // 每隔多少帧插入一个关键帧（I帧）
    av_encodec_ctx_->max_b_frames = 0; // 禁用B帧，降低录制延迟

    // 4. MP4格式要求SPS/PPS存于extradata，必须在avcodec_open2前设置
    av_encodec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 5. x264私有参数：编码速度与压缩率平衡，必须在avcodec_open2前通过priv_data设置
    if (av_opt_set(av_encodec_ctx_->priv_data, "preset", "medium", 0) < 0) {
        LOG_ERROR("set x264 preset failed");
    }

    // 6. 打开编码器（avcodec_open2会读取上方所有参数完成初始化）
    if (avcodec_open2(av_encodec_ctx_, codec, nullptr) < 0) {
        LOG_ERROR("AVCodec Open failed");
        avcodec_free_context(&av_encodec_ctx_);
        return false;
    }

    return true;
}

/**
 * @brief 编码一帧 YUV420P：包装为 AVFrame 送入编码器，输出 H.264 包经 Listener 回调
 * @param video_frame 输入 YUV420P 视频帧，不得为空
 * @note 构造的 AVFrame 仅引用 VideoFrame 的像素指针（零拷贝），不调用 av_frame_get_buffer，
 *       因此 buf[] 为 null，av_frame_free 只释放结构体不释放像素数据
 *       与 Flush() 共用 encoder_muxter_ 串行化对 av_encodec_ctx_ 的访问
 */
void VideoEncoder::Encoder(std::shared_ptr<VideoFrame> video_frame) {
    std::lock_guard<std::mutex> lock(encoder_muxter_);

    if (!av_encodec_ctx_ || !video_frame) {
        return;
    }

    // 1. 构造 AVFrame：引用 VideoFrame 的像素指针，零拷贝
    AVFrame* yuv_frame = av_frame_alloc();
    if (!yuv_frame) {
        LOG_ERROR("av_frame_alloc failed");
        return;
    }

    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = video_frame->width_;
    yuv_frame->height = video_frame->height_;

    // 直接指向 VideoFrame 的 YUV 平面，不分配缓冲，避免内存拷贝
    yuv_frame->data[0] = video_frame->data_[0]; // Y 分量
    yuv_frame->data[1] = video_frame->data_[1]; // U 分量
    yuv_frame->data[2] = video_frame->data_[2]; // V 分量
    yuv_frame->linesize[0] = video_frame->linesize_[0];
    yuv_frame->linesize[1] = video_frame->linesize_[1];
    yuv_frame->linesize[2] = video_frame->linesize_[2];

    // 2. 内部自增 PTS（以 time_base={1,fps} 为单位，每帧 +1）
    //    不依赖输入帧 PTS，保证 seek 后录制时间轴连续
    yuv_frame->pts = pts_++;

    // 3. 送入编码器（buf[] 为 null 时编码器内部复制一份帧数据）
    if (avcodec_send_frame(av_encodec_ctx_, yuv_frame) < 0) {
        LOG_ERROR("avcodec_send_frame failed");
        av_frame_free(&yuv_frame);
        return;
    }

    // 释放 AVFrame 结构体（不释放像素数据，因 buf[] 为 null）
    av_frame_free(&yuv_frame);

    // 4. 取出编码后的 H.264 包；返回 EAGAIN 表示需更多输入帧，退出等待下一帧
    while (true) {
        AVPacket* raw_pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(av_encodec_ctx_, raw_pkt);
        if (ret == 0) {
            // 成功取出一个包，Packet 接管 raw_pkt 所有权
            auto packet = std::make_shared<Packet>(raw_pkt);
            if (listener_) {
                listener_->OnVideoEncoderNotifyPacket(packet);
            }
        } else {
            // EAGAIN：编码器需更多输入帧，属正常现象
            av_packet_free(&raw_pkt);
            break;
        }
    }
}

/**
 * @brief 冲洗编码内部缓冲区
 * @note 触发drain模式：发送nullptr让编码器输出所有缓冲中的剩余H.264包，录制结束时调用
 *       编码方向：avcodec_send_frame → avcodec_receive_packet
 *       解码方向：avcodec_send_packet → avcodec_receive_frame（方向不可混用）
 */
void VideoEncoder::Flush() {
    std::lock_guard<std::mutex> lock(encoder_muxter_);

    // 发送nullptr触发drain模式，编码器不再接收新帧，开始输出缓冲区剩余包
    avcodec_send_frame(av_encodec_ctx_, nullptr);

    // 循环取出剩余的H264包，直到AVERROR_EOF表示彻底排空
    while (true) {
        AVPacket* raw_pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(av_encodec_ctx_, raw_pkt);
        if (ret == 0) {
            auto packet = std::make_shared<Packet>(raw_pkt);
            if (listener_) {
                listener_->OnVideoEncoderNotifyPacket(packet);
            }
        } else {
            // AVERROR_EOF：编码器彻底排空，退出
            av_packet_free(&raw_pkt);
            break;
        }
    }

    if (listener_) {
        listener_->OnVideoEncoderNotifyFinished();
    }
}

/**
 * @brief 释放编码器资源
 */
void VideoEncoder::Close() {
    if (av_encodec_ctx_) {
        avcodec_free_context(&av_encodec_ctx_);
        av_encodec_ctx_ = nullptr;
    }
}

/**
 * @brief 返回视频编码器上下文，供Muxer通过avcodec_parameters_from_context写入MP4文件头
 */
AVCodecContext* VideoEncoder::GetCodecContext() const { return av_encodec_ctx_; }

} // namespace av

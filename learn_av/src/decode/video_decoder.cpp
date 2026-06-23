/**
 * @file video_decoder.cpp
 * @brief 视频解码模块实现
 * @details 独立解码线程：从队列取压缩 AVPacket，经 avcodec_send_packet/avcodec_receive_frame 解码，
 *          非 YUV420P 像素格式经 SwsContext 转换，拷贝为紧凑步长的 VideoFrame 推送下游渲染
 */
#include "video_decoder.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "define/av_packet.h"
#include "define/stream_info.h"
#include "interface/ivideo_decoder.h"
#include "logger.h"
#include "playback_state.h"
#include "playback_sync.h"
#include "utils/thread_safe_queue.h"
#include "video_frame.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace av {

/**
 * @brief 构造函数
 */
VideoDecoder::VideoDecoder() {}

/**
 * @brief 析构函数
 * @details 严格遵循时序：先终止 OS 线程，后清理物理内存，防止段错误
 */
VideoDecoder::~VideoDecoder() {
    RequestStop();
    WaitForStop();
    Close();
}

/**
 * @brief 解码器创建
 */
std::unique_ptr<IVideoDecoder> IVideoDecoder::Create() { return std::make_unique<VideoDecoder>(); }

/**
 * @brief 设置下游监听器
 */
void VideoDecoder::SetListener(Listener* listener) { listener_ = listener; }

/**
 * @brief 初始化解码线程与上下文
 * @param info 纯净视频流元数据 DTO
 * @return 初始化是否成功
 */
bool VideoDecoder::Init(const StreamInfo& info) {
    // 1. 初始化前确保释放历史资源，防止反复 Open 导致的内存泄漏
    Close();

    std::lock_guard<std::mutex> lock(codec_mutex_);

    // 2. 校验入参 StreamInfo 的有效性 (兼容无 extra_data 的码流)
    if (info.codec_id_ <= 0) {
        LOG_ERROR("Invalid StreamInfo: codec_id is missing");
        return false;
    }

    // 3. 获取时间基
    // 采用分数形式 (AVRational) 表述时钟频率，用于后续 PTS 的绝对时间换算
    time_base_.den = info.timebase_den_;
    time_base_.num = info.timebase_num_;

    // 4. 通过编码格式寻找对应的视频解码器
    const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(info.codec_id_));
    if (codec == nullptr) {
        LOG_ERROR("find codec fail for CodecID: {}", info.codec_id_);
        return false;
    }

    // 5. 分配解码器上下文实例
    av_codec_ctx_ = avcodec_alloc_context3(codec);
    if (av_codec_ctx_ == nullptr) {
        LOG_ERROR("alloc av_codec_ctx failed");
        return false;
    }

    // 6. 把 stream 里的核心物理参数拷贝给上下文
    av_codec_ctx_->width = info.width_;
    av_codec_ctx_->height = info.height_;

    // 7. 挂载 OOB (Out-of-Band) 全局序列头信息 (SPS/PPS 等)
    if (!info.extra_data_.empty()) {
        av_codec_ctx_->extradata_size = static_cast<int>(info.extra_data_.size());

        // 使用 FFmpeg 专属内存分配器，并追加 AV_INPUT_BUFFER_PADDING_SIZE 字节的内存安全垫
        // 目的：防范底层 SIMD (单指令多数据流) 汇编优化指令在内存对齐读取时发生越界崩溃 (Segfault)
        av_codec_ctx_->extradata =
            static_cast<uint8_t*>(av_mallocz(info.extra_data_.size() + AV_INPUT_BUFFER_PADDING_SIZE));

        if (!av_codec_ctx_->extradata) {
            LOG_ERROR("Failed to allocate memory for extradata");
            avcodec_free_context(&av_codec_ctx_);
            return false;
        }

        memcpy(av_codec_ctx_->extradata, info.extra_data_.data(), info.extra_data_.size());
    }

    // 8. 打开解码器
    if (avcodec_open2(av_codec_ctx_, codec, nullptr) < 0) {
        avcodec_free_context(&av_codec_ctx_);
        LOG_ERROR("open codec failed");
        return false;
    }

    LOG_INFO("Video Decoder Init Success! Video resolution: {}x{}", info.width_, info.height_);
    return true;
}

/**
 * @brief 将来自解复用器的packet推入阻塞队列
 * @param packet 包含 H.264/HEVC 压缩数据的智能指针
 */
void VideoDecoder::PushPacket(std::shared_ptr<Packet> packet) {
    if (!packet) {
        LOG_ERROR("No Recv H264 Frame from Demuxer");
        return;
    }

    if (packet->HasFlag(av::AVFrameFlag::kFlush)) {
        packet_queue_.Clear();
    }
    
    // 隐式背压：队列容量满载时将阻塞 Demuxer 线程
    packet_queue_.Push(std::move(packet));
}

/**
 * @brief 播放完毕，发送空包通知解码器输入结束
 * @details 触发底层 avcodec_send_packet 排空内部残余的 B/P 帧
 */
void VideoDecoder::SendEOF() {
    LOG_INFO("Video decoder received EOF signal, pushing EOF packet to queue");
    // 创造一个空包并入队
    auto empty_packet = std::make_shared<Packet>();
    packet_queue_.Push(std::move(empty_packet));
}

/**
 * @brief 启动解码线程
 * @param sync_ctx 注入全局状态同步上下文
 */
void VideoDecoder::Start(std::shared_ptr<SyncContext> sync_ctx) {
    // 1. 防止线程重入
    if (!is_abort_requested_.load()) {
        LOG_WARN("Video Decoder thread is already running.");
        return;
    }

    // 2. 回收上次可能遗留的线程句柄
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    // 3. 设置线程标志
    is_abort_requested_.store(false);
    awaiting_flush_.store(false);

    // 4. 清空&&启动队列
    packet_queue_.Clear();
    packet_queue_.Start();

    // 5. 保存全局同步状态
    sync_ctx_ = std::move(sync_ctx);

    // 6. 启动真正的解码线程
    decode_thread_ = std::thread(&VideoDecoder::ThreadLoop, this);

    LOG_INFO("Video Decoder OS thread start successfully...");
}

/**
 * @brief 阶段一：下发异步终止信令 --非阻塞
 */
void VideoDecoder::RequestStop() {
    // 1. 若已处于终止请求状态，避免重复操作
    if (is_abort_requested_.load()) {
        return;
    }

    // 2. 下发终止信令
    is_abort_requested_.store(true);

    // 3. 唤醒阻塞在 state_cv_ 上的线程（阶段A）
    if (sync_ctx_) {
        sync_ctx_->state_cv_.notify_all();
    }

    // 4. 唤醒阻塞在 packet_queue_.Pop() 上的线程（阶段B）
    packet_queue_.Abort();
}

/**
 * @brief 阶段二：同步等待回收线程 --阻塞
 */
void VideoDecoder::WaitForStop() {
    // 1. 同步等待 OS 线程指令流执行完毕并回收句柄
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
}

/**
 * @brief 执行物理层面的解码器内部状态冲刷
 * @note 仅允许在解码工作线程 (ThreadLoop) 内部捕获到 kFlush 信令包时调用
 */
void VideoDecoder::Flush() {
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (av_codec_ctx_) {
        // 清空 FFmpeg 内部的视频帧，确保新画面不花屏
        avcodec_flush_buffers(av_codec_ctx_);
    }
    LOG_INFO("Video Decoder internal codec buffers flushed...");
}

/**
 * @brief 清空队列，唤醒阻塞
 * @details Seek 触发：置位丢帧门闩，直到处理完随后的 kFlush 包才恢复放行，
 *          杜绝"is_seeking_ 已清零但 Flush 包尚未抵达"窗口内的旧帧漏帧
 */
void VideoDecoder::AbortAndClearQueue() {
    awaiting_flush_.store(true);
    packet_queue_.AbortAndClear();
}

/**
 * @brief 启动队列
 */
void VideoDecoder::StartQueue() { packet_queue_.Start(); }

/**
 * @brief 释放 FFmpeg 解码器物理内存
 * @note 必须在确认 OS 线程已停止 (Stop 调用后) 才能执行
 */
void VideoDecoder::Close() {
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (av_codec_ctx_) {
        avcodec_free_context(&av_codec_ctx_);
        av_codec_ctx_ = nullptr;
    }
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
        sws_src_w_ = 0;
        sws_src_h_ = 0;
        sws_src_fmt_ = -1;
    }
    packet_queue_.Clear();
}

/**
 * @brief 视频解码核心调度循环
 * @details 从队列取压缩包、驱动解码引擎、拷贝为紧凑 VideoFrame 并投递下游。
 */
void VideoDecoder::ThreadLoop() {
    if (!sync_ctx_) {
        return;
    }

    // [阶段 0]: 申请一个 AVFrame 临时变量作为输出容器
    AVFrame* av_frame = av_frame_alloc();
    if (!av_frame) {
        LOG_ERROR("Failed to allocate AVFrame container.");
        return;
    }

    // 主事件循环：生命周期校验
    while (!is_abort_requested_.load()) {
        // [阶段 A]: 线程调度与管线挂起控制
        //   1. 收到终止请求（is_abort_）
        //   2. 发生跳转（is_seeking_），需要处理 Flush 包
        //   3. 处于播放状态（kPlaying）
        {
            std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
            sync_ctx_->state_cv_.wait(lock, [this]() {
                return is_abort_requested_.load() || sync_ctx_->is_seeking_.load() ||
                    sync_ctx_->state_.load() == PlaybackState::kPlaying;
            });
        }

        // 唤醒后的生命周期确认，防止处于挂起时被下发终止信令
        if (is_abort_requested_.load()) {
            break;
        }

        // [阶段 B]: 阻塞式提取视频
        std::shared_ptr<Packet> packet;
        if (!packet_queue_.Pop(packet)) {
            if (is_abort_requested_.load()) {
                break;
            }
            continue;
        }

        if (!packet) {
            continue;
        }

        // [阶段 C]: 处理带内控制信令
        if (packet->HasFlag(av::AVFrameFlag::kFlush)) {
            LOG_DEBUG("Video Decoder Received In-band Flush packet, executing codec synchronization.");
            Flush();

            // Flush 包已抵达：跳转后的新数据从此开始，解除丢帧门闩
            awaiting_flush_.store(false);

            // 向下游发送带Flush标志的空帧
            auto video_frame = std::make_shared<VideoFrame>(0, 0);
            video_frame->AddFlag(av::AVFrameFlag::kFlush);
            if (listener_) {
                listener_->OnNotifyVideoFrame(std::move(video_frame));
            }
            continue;
        }

        // [阶段 D]: packet送入解码器
        int ret = 0;
        {
            std::lock_guard<std::mutex> lock(codec_mutex_);
            ret = avcodec_send_packet(av_codec_ctx_, packet->GetRawPacket());
        }

        // 校验投递状态 (过滤正常输入和 EOF 冲刷空包)
        if (ret < 0 && ret != AVERROR_EOF) {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> err_buf{};
            av_strerror(ret, err_buf.data(), err_buf.size());
            LOG_ERROR("avcodec_send_packet failed: {}", err_buf.data());
            continue;
        }

        // [阶段 E]: 提取视频帧
        while (true) {
            {
                std::lock_guard<std::mutex> lock(codec_mutex_);
                ret = avcodec_receive_frame(av_codec_ctx_, av_frame);
            }

            if (ret == AVERROR(EAGAIN)) {
                // 内部参考帧不足，需继续供给 AVPacket 才能输出画面
                break;
            } else if (ret == AVERROR_EOF) {
                // 解码管线彻底排空，向下游发送 EOF 空帧
                if (listener_) {
                    LOG_INFO("Video decoding reached physical EOF.");
                    auto eof_frame = std::make_shared<VideoFrame>(0, 0);
                    eof_frame->AddFlag(AVFrameFlag::kEOF);
                    listener_->OnNotifyVideoFrame(std::move(eof_frame));
                }
                break;
            } else if (ret < 0) {
                // 底层硬件或解码库发生不可恢复异常
                std::array<char, AV_ERROR_MAX_STRING_SIZE> err_buf{};
                av_strerror(ret, err_buf.data(), err_buf.size());
                LOG_ERROR("avcodec_receive_frame failed: {}", err_buf.data());
                break;
            }

            // [阶段 F]: 处理解码出的裸帧
            ProcessAndPack(av_frame);

            // [阶段 G]: 释放由 FFmpeg 在内部分配的图像堆内存，将 av_frame 容器复位以供下一圈复用
            av_frame_unref(av_frame);
        }
    }

    // [阶段 0 的闭环]: 销毁物理内存载体，防范堆内存泄漏
    av_frame_free(&av_frame);

    LOG_INFO("VideoDecoder ThreadLoop exited safely.");
}

/**
 * @brief 将解码裸帧拷贝为紧凑步长的 YUV420P VideoFrame 并分发下游
 * @param frame FFmpeg 原生裸帧（各平面含对齐 Padding，步长为 linesize）
 * @details 非 YUV420P（如 NV12/YUV422P/YUV444P）先经 SwsContext 转换；
 *          再按行拷贝去除 Padding，色度尺寸向上取整 (w+1)/2 兼容奇数分辨率
 */
void VideoDecoder::ProcessAndPack(AVFrame* frame) {
    // 1. 校验 frame
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        LOG_ERROR("ProcessAndPack receive a bad frame.");
        return;
    }

    // 2. 像素格式归一化：非 YUV420P 一律转换为 YUV420P
    AVFrame* yuv = frame; // 实际用于剥离的源帧
    AVFrame* converted = nullptr; // 转换产物，需在函数末尾释放
    if (frame->format != AV_PIX_FMT_YUV420P) {
        converted = ConvertToYUV420P(frame);
        if (!converted) {
            LOG_ERROR("Pixel format conversion to YUV420P failed (src fmt={}).", frame->format);
            return;
        }
        yuv = converted;
    }

    const int width = yuv->width;
    const int height = yuv->height;

    // 3. 实例化 DTO，内部预分配完全紧凑的内存
    auto video_frame = std::make_shared<av::VideoFrame>(width, height);

    // 4. 内存对齐剥离算法：源步长 linesize 含 Padding，目标步长完全紧凑
    //    色度尺寸统一采用向上取整 (width+1)/2，与 VideoFrame 内部分配保持一致，兼容奇数分辨率

    // 4.1 拷贝 Luma (Y 分量)
    for (int i = 0; i < height; ++i) {
        ptrdiff_t dst_offset = static_cast<ptrdiff_t>(i) * width;
        ptrdiff_t src_offset = static_cast<ptrdiff_t>(i) * yuv->linesize[0];
        memcpy(video_frame->data_[0] + dst_offset, yuv->data[0] + src_offset, width);
    }

    // 4.2 拷贝 Chroma (U & V 分量)，步长与高度均向上取整
    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;

    for (int i = 0; i < chroma_height; ++i) {
        ptrdiff_t dst_offset = static_cast<ptrdiff_t>(i) * chroma_width;
        ptrdiff_t src_u_offset = static_cast<ptrdiff_t>(i) * yuv->linesize[1];
        ptrdiff_t src_v_offset = static_cast<ptrdiff_t>(i) * yuv->linesize[2];

        memcpy(video_frame->data_[1] + dst_offset, yuv->data[1] + src_u_offset, chroma_width);
        memcpy(video_frame->data_[2] + dst_offset, yuv->data[2] + src_v_offset, chroma_width);
    }

    // 5. 转换产物已拷贝完毕，立即释放，避免堆内存泄漏
    if (converted) {
        av_frame_free(&converted);
    }

    // 6. 显示时间戳 (PTS) 换算（始终取自原始帧）
    int64_t actual_pts = (frame->pts == AV_NOPTS_VALUE) ? frame->best_effort_timestamp : frame->pts;
    if (actual_pts != AV_NOPTS_VALUE) {
        AVRational us_q = {1, 1000000};
        int64_t time_us = av_rescale_q(actual_pts, time_base_, us_q);
        video_frame->SetTimestamp(time_us);
    }

    // 7. 跳转期间丢弃旧帧：is_seeking_ 锁定期 或 尚未处理 Flush 包(awaiting_flush_) 均丢弃
    if ((sync_ctx_ && sync_ctx_->is_seeking_.load()) || awaiting_flush_.load()) {
        LOG_DEBUG("Discarding pre-seek video frame, timestamp: {} us", video_frame->GetTimestampUs());
        return;
    }

    // 8. 将封装完毕的 YUV420P 帧传到下游监听器
    if (listener_) {
        listener_->OnNotifyVideoFrame(std::move(video_frame));
    }
}

/**
 * @brief 将任意像素格式的裸帧统一转换为 YUV420P
 */
AVFrame* VideoDecoder::ConvertToYUV420P(AVFrame* src) {
    // 1. 惰性创建 / 按源参数变化重建 SwsContext
    if (!sws_ctx_ || sws_src_w_ != src->width || sws_src_h_ != src->height || sws_src_fmt_ != src->format) {
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
            sws_ctx_ = nullptr;
        }
        sws_ctx_ = sws_getContext(src->width, src->height, static_cast<AVPixelFormat>(src->format), src->width,
                                  src->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            LOG_ERROR("sws_getContext failed for src fmt={}", src->format);
            return nullptr;
        }
        sws_src_w_ = src->width;
        sws_src_h_ = src->height;
        sws_src_fmt_ = src->format;
        LOG_INFO("SwsContext created: convert pix_fmt {} -> YUV420P ({}x{})", src->format, src->width, src->height);
    }

    // 2. 分配目标 YUV420P 帧
    AVFrame* dst = av_frame_alloc();
    if (!dst) {
        return nullptr;
    }
    dst->width = src->width;
    dst->height = src->height;
    dst->format = AV_PIX_FMT_YUV420P;
    if (av_frame_get_buffer(dst, 0) < 0) {
        LOG_ERROR("av_frame_get_buffer failed for converted YUV420P frame.");
        av_frame_free(&dst);
        return nullptr;
    }

    // 3. 执行像素格式转换
    sws_scale(sws_ctx_, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
    return dst;
}

} // namespace av
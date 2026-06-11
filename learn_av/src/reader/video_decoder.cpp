/**
 * @file video_decoder.cpp
 * @brief 视频解码类的核心实现
 * @details 负责从 Demuxer 接收压缩比特流，驱动底层硬/软件解码引擎，
 * 提取 YUV420P 裸数据并封装至 DTO 中，交由下游 GPU 管线进行色彩空间转换与渲染
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
std::unique_ptr<IVideoDecoder> IVideoDecoder::Create() {
    return std::make_unique<VideoDecoder>();
}

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
    LOG_INFO("========== VideoDecoder::Start ==========");
    // 1. 判断要不要启动新的解码线程 (看物理 OS 线程是否存在)
    if (decode_thread_.joinable()) {
        LOG_WARN("Video Decoder thread is already running.");
        return;
    }

    // 2. 清空历史脏包，启动队列
    packet_queue_.Clear();
    packet_queue_.Start();

    // 3. 保存全局同步状态
    sync_ctx_ = std::move(sync_ctx);

    // 4. 业务生命周期状态复位
    is_abort_requested_.store(false);

    // 5. 启动真正的解码线程
    decode_thread_ = std::thread(&VideoDecoder::DecodeLoop, this);

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

    // 2. 下发中断带外信令
    is_abort_requested_.store(true);

    // 3. 通知其他模块视频解码线程停止，唤醒可能挂起的条件变量
    if (sync_ctx_) {
        sync_ctx_->state_cv_.notify_all();
    }

    // 4. 强行中断可能阻塞在数据队列 Push/Pop 操作上的线程
    packet_queue_.Abort();

    LOG_INFO("========== VideoDecoder::RequestStop ==========");
}

/**
 * @brief 阶段二：同步等待回收线程 --阻塞
 */
void VideoDecoder::WaitForStop() {
    // 1. 同步等待 OS 线程指令流执行完毕并回收句柄
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    // 2. 状态复位与残余数据清理
    packet_queue_.Clear();
    is_abort_requested_.store(false);
    sync_ctx_.reset();

    LOG_INFO("========== VideoDecoder::WaitForStop ==========");
}

/**
 * @brief 执行物理层面的解码器内部状态冲刷
 * @note 仅允许在解码工作线程 (DecodeLoop) 内部捕获到 kFlush 信令包时调用
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
 */
void VideoDecoder::AbortAndClearQueue() { packet_queue_.AbortAndClear(); }

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
    packet_queue_.Clear();
    LOG_INFO("========== VideoDecoder::Close ==========");
}

/**
 * @brief 视频解码核心调度循环 (Video Decoding Event Loop)
 * @details 负责从 Demuxer 接收压缩比特流，驱动底层硬件/软件解码引擎，
 * 并在无锁环境下执行内存剥离对齐与下游路由投递。
 */
void VideoDecoder::DecodeLoop() {
    LOG_INFO("VideoDecoder DecodeLoop started...");

    // [阶段 0]: 申请一个 AVFrame 临时变量作为输出容器
    AVFrame* av_frame = av_frame_alloc();
    if (!av_frame) {
        LOG_ERROR("Failed to allocate AVFrame container.");
        return;
    }

    // 主事件循环：生命周期校验
    while (!is_abort_requested_.load()) {
        // [阶段 A]: 线程调度与管线挂起控制
        {
            std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);

            // 1. 线程停止
            // 2. 发生了跳转
            // 3. 处于播放状态
            // 4. 全局Stop
            sync_ctx_->state_cv_.wait(lock, [this]() {
                return is_abort_requested_.load() || sync_ctx_->is_seeking_.load() ||
                    sync_ctx_->state_.load() == PlaybackState::kPlaying ||
                    sync_ctx_->state_.load() == PlaybackState::kStopped;
            });
        }

        // 唤醒后的生命周期确认，防止处于挂起时被下发终止信令
        if (is_abort_requested_.load()) {
            break;
        }

        // [阶段 B]: 阻塞式提取视频
        std::shared_ptr<Packet> packet;
        if (!packet_queue_.Pop(packet)) {
            if (is_abort_requested_.load() || sync_ctx_->state_.load() == PlaybackState::kStopped) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (!packet) {
            continue;
        }

        // [阶段 C]: 处理带内控制信令
        if (packet->HasFlag(av::AVFrameFlag::kFlush)) {
            LOG_DEBUG("Video Decoder Received In-band Flush packet, executing codec synchronization.");
            Flush();

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
                // 解码管线彻底排空
                if (listener_) {
                    LOG_INFO("Video decoding reached physical EOF.");
                    listener_->OnNotifyVideoEOF();
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

    LOG_INFO("VideoDecoder DecodeLoop exited safely.");
}

/**
 * @brief 处理解码出的裸帧，打包 YUV 分量数据并分发给 OpenGL 渲染器
 * @param frame FFmpeg 原生裸帧指针 (存在内存 Padding)
 */
void VideoDecoder::ProcessAndPack(AVFrame* frame) {
    // 1. 校验 frame
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        LOG_ERROR("ProcessAndPack receive a bad frame.");
        return;
    }

    // 2. 实例化 DTO (Data Transfer Object)，内部预分配完全紧凑的内存
    auto video_frame = std::make_shared<av::VideoFrame>(frame->width, frame->height);

    // 3. 内存对齐剥离算法 (Memory Alignment Stripping)
    // 结合 ptrdiff_t 强制转换，杜绝 32 位整数溢出风险

    // 1. 拷贝 Luma (Y 分量)
    for (int i = 0; i < frame->height; ++i) {
        // 目标结构：完全紧凑，步长严格等于 width
        ptrdiff_t dst_offset = static_cast<ptrdiff_t>(i) * frame->width;
        // 源数据：存在对齐填充 Padding，步长为 linesize[0]
        ptrdiff_t src_offset = static_cast<ptrdiff_t>(i) * frame->linesize[0];

        memcpy(video_frame->data_[0] + dst_offset, frame->data[0] + src_offset, frame->width);
    }

    // 2. 拷贝 Chroma (U & V 分量)
    int chroma_width = frame->width / 2;
    int chroma_height = frame->height / 2;

    for (int i = 0; i < chroma_height; ++i) {
        // 目标结构：完全紧凑，步长为 width / 2 (chroma_width)
        ptrdiff_t dst_offset = static_cast<ptrdiff_t>(i) * chroma_width;

        // 源数据 U：步长为 linesize[1]
        ptrdiff_t src_u_offset = static_cast<ptrdiff_t>(i) * frame->linesize[1];
        // 源数据 V：步长为 linesize[2]
        ptrdiff_t src_v_offset = static_cast<ptrdiff_t>(i) * frame->linesize[2];

        memcpy(video_frame->data_[1] + dst_offset, frame->data[1] + src_u_offset, chroma_width);
        memcpy(video_frame->data_[2] + dst_offset, frame->data[2] + src_v_offset, chroma_width);
    }

    // 4. 显示时间戳 (PTS) 的换算
    // AV_NOPTS_VALUE 表示无显示时间戳值，需回退使用 best_effort_timestamp
    int64_t actual_pts = (frame->pts == AV_NOPTS_VALUE) ? frame->best_effort_timestamp : frame->pts;

    int64_t time_us = 0;

    if (actual_pts != AV_NOPTS_VALUE) {
        AVRational us_q = {1, 1000000};

        // 将时钟滴答数转换为绝对时间 (秒)
        time_us = av_rescale_q(actual_pts, time_base_, us_q);
        video_frame->SetTimestamp(time_us);
    }

    if (sync_ctx_ && sync_ctx_->is_seeking_.load()) {
        // 正处于跳转状态
        LOG_DEBUG("Discarding pre-seek audio frame, timestamp: {} us", video_frame->GetTimestampUs());
        return;
    }

    // 5. 将封装完毕的 YUV420P 帧传到下游监听器
    if (listener_) {
        listener_->OnNotifyVideoFrame(std::move(video_frame));
    }
}

} // namespace av
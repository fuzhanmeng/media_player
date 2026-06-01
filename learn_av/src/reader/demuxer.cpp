/**
 * @file demuxer.cpp
 * @brief 媒体解复用引擎核心实现
 * @details 基于单后台线程的异步解封装架构，支持 I/O 阻塞超时控制、条件变量背压限流以及绝对时间轴 Seek 操作。
 */

#include "demuxer.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "define/av_packet.h"
#include "interface/idemuxer.h"
#include "logger.h"
#include "playback_sync.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace av {

std::unique_ptr<IDemuxer> IDemuxer::Create() {
    LOG_INFO("Demuxer Create...");
    return std::make_unique<Demuxer>();
}

// 1. 构造/析构
Demuxer::Demuxer() {}

Demuxer::~Demuxer() {
    // 1. 停止解复用线程
    Stop();

    // 2. 释放内部资源
    Close();

    LOG_INFO("Demuxer destroyed.");
}

/**
 * @brief 注册事件监听器 (Observer Pattern)
 * @param listener 注册监听器
 */
void Demuxer::SetListener(Listener* listener) { listener_ = listener; }

/**
 * @brief 执行容器解析与流元数据探测
 * @param url 本地文件路径或网络流媒体 URI
 * @return 探测并构建物理流映射是否成功
 * @note 阻塞式同步 I/O 操作，涉及 FFmpeg 的 avformat_open_input 与 avformat_find_stream_info
 */
bool Demuxer::Open(const std::string& url) {
    // 1. 关闭资源
    Close();

    // 2. 初始化ffmpeg 上下文
    std::lock_guard<std::mutex> lcok(avformat_mutex_);

    if (avformat_open_input(&avformat_ctx_, url.c_str(), nullptr, nullptr) < 0) {
        LOG_ERROR("Failed to open input media:  {} ", url.c_str());
        return false;
    }

    // 3. 探测容器级元数据与流参数
    if (avformat_find_stream_info(avformat_ctx_, nullptr) < 0) {
        LOG_ERROR("Failed to extract stream metadata from: {}", url.c_str());

        avformat_close_input(&avformat_ctx_);
        avformat_ctx_ = nullptr;
        return false;
    }

    // 4. 重置流信息状态
    audio_stream_index_ = -1;
    video_stream_index_ = -1;

    // 5. 开始遍历物理轨道，分离音频/视频轨道，抽取关键参数构造纯净 DTO (Data Transfer Object)
    for (int i = 0; i < avformat_ctx_->nb_streams; ++i) {
        AVStream* stream = avformat_ctx_->streams[i];

        // video
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = i;

            if (listener_) {
                StreamInfo info;
                info.stream_index_ = stream->index; ///< 流索引
                info.codec_id_ = stream->codecpar->codec_id; ///< 编码器 ID (对应 AVCodecID)

                info.width_ = stream->codecpar->width; ///< 视频宽度
                info.height_ = stream->codecpar->height; ///< 视频高度

                info.timebase_num_ = stream->time_base.num; ///< 时间基分子
                info.timebase_den_ = stream->time_base.den; ///< 时间基分母

                // 提取解码器私有配置数据
                if (stream->codecpar->extradata_size > 0) {
                    info.extra_data_.assign(stream->codecpar->extradata,
                                            stream->codecpar->extradata + stream->codecpar->extradata_size);
                }

                LOG_INFO("Find Video Stream Successfully. Index: {}", i);

                // 向上层总控投递纯净 流信息 DTO
                listener_->OnNotifyVideoStream(info);
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index_ = i;

            if (listener_) {
                StreamInfo info;
                info.stream_index_ = i; ///< 流索引
                info.codec_id_ = stream->codecpar->codec_id; ///< 编码器 ID (对应 AVCodecID)

                info.channels_ = stream->codecpar->channels; ///< 通道数
                info.sample_rate_ = stream->codecpar->sample_rate; ///< 采样率

                info.timebase_den_ = stream->time_base.den; ///< 时间基分子
                info.timebase_num_ = stream->time_base.num; ///< 时间基分母

                // 提取解码器私有配置数据
                if (stream->codecpar->extradata_size > 0) {
                    info.extra_data_.assign(stream->codecpar->extradata,
                                            stream->codecpar->extradata + stream->codecpar->extradata_size);
                }

                LOG_INFO("Find Audio Stream Successfully. Index: {}", i);
                // 向上层总控投递纯净 流信息 DTO
                listener_->OnNotifyAudioStream(info);
            }
        }
    }
    LOG_INFO("Media Demuxer initialized successfully: {}", url.c_str());
    return true;
}

/**
 * @brief 启动异步解复用工作线程
 */
void Demuxer::Start(std::shared_ptr<SyncContext> sync_ctx) {
    // 1. 如果当前状态是停止状态直接返回
    if (demuxer_thread_.joinable()) {
        return;
    }
    // 2. 保存来自Player模块的同步信息
    sync_ctx_ = std::move(sync_ctx);

    // 3. 实例化底层的 OS 级解复用线程
    if (!demuxer_thread_.joinable()) {
        demuxer_thread_ = std::thread(&Demuxer::ThreadLoop, this);
    }

    LOG_INFO("Demuxer ThreadLoop started.");
}

/**
 * @brief 清空解复用器内部状态并向下游发送Flush信令
 * @note 主要用于Seek操作后清除旧数据，防止花屏
 */
void Demuxer::Flush() {
    LOG_INFO("Flush demuxer internal state");

    // 向音频和视频流分别发送Flush包
    auto flush_packet = std::make_shared<Packet>(nullptr);
    flush_packet->AddFlag(av::AVFrameFlag::kFlush);

    if (audio_stream_index_ != -1 && listener_) {
        listener_->OnNotifyAudioPacket(flush_packet);
    }

    if (video_stream_index_ != -1 && listener_) {
        listener_->OnNotifyVideoPacket(std::move(flush_packet));
    }

    LOG_INFO("Demuxer flush completed, Flush packets sent to decoders");
}

/**
 * @brief 安全终止解复用线程并释放 I/O 资源
 * @note 会触发中断信令，唤醒所有挂起的锁与条件变量，等待线程 join
 */
void Demuxer::Stop() {
    // 1. 如果已经是退出状态,直接返回
    if (is_abort_requested_.load()) {
        return;
    }

    // 2. 触发终止信令, 并释放可能阻塞的条件变量
    is_abort_requested_.store(true);

    // 3. 唤醒可能正在沉睡的线程，让它看到 is_abort_requested_ 为 true，从而退出循环
    if (sync_ctx_ && sync_ctx_->IsValid()) {
        sync_ctx_->state_cv_.notify_all();
    }

    // 4. 停止解解复用
    if (demuxer_thread_.joinable()) {
        demuxer_thread_.join();
    }

    // 5. 状态与指令复位
    is_abort_requested_.store(false);
    is_seek_requested_.store(false);

    // 释放对全局共享状态的引用，防止悬空
    sync_ctx_.reset();
    LOG_INFO("Demuxer pipeline stopped safely.");
}

/**
 * @brief 触发媒体物理时间轴跳转 (Seek)
 * @param target_time_sec 目标绝对时间（单位：秒）
 * @note 异步非阻塞信令投递。摒弃比例跳转，由外部业务态统一维持绝对时间系。
 */
void Demuxer::SeekTo(double target_time_sec) {
    // 防抖机制：若上一次跳转请求尚未处理，则丢弃本次冗余请求
    if (is_seek_requested_.load()) {
        LOG_DEBUG("Seek request debounced. A seek is already in progress.");
        return;
    }

    is_seek_requested_.store(true);
    target_seek_time_.store(target_time_sec);

    // 发射唤醒信令，中断 ThreadLoop 可能的挂起状态，令其优先处理 P0 级任务
    if (sync_ctx_ && sync_ctx_->IsValid()) {
        sync_ctx_->state_cv_.notify_all();
    }
}

/**
 * @brief 获取当前媒体容器的全局总时长
 * @return 总时长（单位：秒）
 */
double Demuxer::GetDuration() const {
    std::lock_guard<std::mutex> lock(avformat_mutex_);

    // AV_NOPTS_VALUE 时长未知（无限制/直播/格式不支持文件损坏/正在下载）
    if (!avformat_ctx_ || avformat_ctx_->duration == AV_NOPTS_VALUE) {
        return 0.0;
    }

    // 将 FFmpeg 内部时基 (AV_TIME_BASE, 微秒) 换算为标准绝对秒数
    return static_cast<double>(avformat_ctx_->duration) / AV_TIME_BASE;
}

/**
 * @brief 内部方法：彻底关闭媒体并释放所有物理资源
 * @note 仅供 Open() 和析构函数调用，对上层业务屏蔽
 */
void Demuxer::Close() {
    // 1. 清理资源前，先停止解复用线程
    Stop();

    // 2. 清理相关资源
    std::lock_guard<std::mutex> lock(avformat_mutex_);
    if (avformat_ctx_) {
        avformat_close_input(&avformat_ctx_);
        avformat_ctx_ = nullptr;
    }

    LOG_INFO("Demuxer closed successfully");
}

/**
 * @brief 异步 I/O 线程的事件循环 (Event Loop)
 * @details 维护三级优先级调度：P0(中断/跳转响应) > P1(背压限流阻塞) > P2(物理分包读取)
 */
void Demuxer::ThreadLoop() {
    LOG_INFO("Demuxer I/O thread activated.");

    while (!is_abort_requested_.load()) {
        // [优先级 P0]: 拦截与处理 Seek 请求
        if (is_seek_requested_.load()) {
            ProcessSeek();
            continue;
        }

        // [优先级 P1]: 管线挂起控制机制
        if (sync_ctx_ && sync_ctx_->IsValid()) {
            std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
            sync_ctx_->state_cv_.wait(lock, [this]() {
                return is_abort_requested_.load() || is_seek_requested_.load() ||
                    sync_ctx_->state_.load() == PlaybackState::kPlaying;
            });
        }

        if (is_abort_requested_.load() || is_seek_requested_.load()) {
            continue;
        }

        // 如果本次读取结束了，休眠并等待下一次唤醒 (通常是 Seek 或退出)
        if (is_eof_.load()) {
            std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
            // 统一使用 Demuxer 本地的 is_seek_requested_ 标志位
            sync_ctx_->state_cv_.wait_for(lock, std::chrono::milliseconds(100),
                                          [this]() { return is_abort_requested_.load() || is_seek_requested_.load(); });
            continue;
        }

        // [优先级 P2]: 提取流数据包并派发
        try {
            if (!ReadAndSendPacket()) {
                // 【只需要在这里标记 EOF，下一轮循环会自动被上面的拦截网接管休眠
                is_eof_.store(true);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Exception occurred demuxing: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    LOG_INFO("Demuxer I/O thread exited gracefully.");
}
/**
 * @brief 响应并执行跳帧逻辑
 * @note 必须在 I/O 线程上下文中执行，防止与 av_read_frame 发生跨线程上下文撕裂
 * 操作完成后向数据总线注入 kFlush 带内信令包
 */
void Demuxer::ProcessSeek() {
    // 1. 加锁
    std::unique_lock<std::mutex> lock(avformat_mutex_);

    if (!avformat_ctx_) {
        is_seek_requested_.store(false);
        return;
    }

    // 2. 转换物理时间为FFmpeg认识的内部微妙 (AV_TIME_BASE = 1000000)
    int64_t target_timestamp = static_cast<int64_t>(target_seek_time_.load() * AV_TIME_BASE);

    // 调用底层API av_seek_frame, 定位至目标时间戳之前的最近的一个 I帧 AVSEEK_FLAG_BACKWARD
    if (av_seek_frame(avformat_ctx_, -1, target_timestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
        LOG_INFO("I/O seek execution succeeded. Target: {} sec", target_seek_time_.load());
        is_eof_.store(false);

        Flush();
    } else {
        LOG_ERROR("I/O seek execution failed for target: {} sec", target_seek_time_.load());
    }

    is_seek_requested_.store(false);
}

/**
 * @brief 执行底层裸包提取与跨模块派发
 * @return true 代表循环继续；false 代表触发 EOF 或严重物理读取异常
 */
bool Demuxer::ReadAndSendPacket() {
    // 1. RAII 申请空包载体
    auto packet = std::make_shared<Packet>();

    int ret = 0;

    // 2. 判断av_format_ctx_
    if (!avformat_ctx_) {
        return false;
    }

    // 3. 从 avformat_ctx_ 中读取数据到 packet
    {
        std::lock_guard<std::mutex> lock(avformat_mutex_);
        ret = av_read_frame(avformat_ctx_, packet->GetRawPacket());
    }

    if (ret >= 0) {
        // ================= send video packet =================
        if (packet->GetRawPacket()->stream_index == video_stream_index_) {
            try {
                // 直接扔给下游,如果下游 Decoder 的 ThreadSafeQueue 满了，线程将在这里自动挂起
                listener_->OnNotifyVideoPacket(std::move(packet));
            } catch (const std::exception& e) {
                LOG_ERROR("Video packet routing exception: {}", e.what());
            }
        }
        // ================= send audio packet =================
        else if (packet->GetRawPacket()->stream_index == audio_stream_index_) {
            try {
                // 直接扔给下游,如果下游 Decoder 的 ThreadSafeQueue 满了，线程将在这里自动挂起
                listener_->OnNotifyAudioPacket(std::move(packet));
            } catch (const std::exception& e) {
                LOG_ERROR("Audio packet routing exception: {}", e.what());
            }
        }

        // 非核心轨道（比如字幕等），直接忽略并继续读取下一个包
        return true;
    }
    // ================= 文件末尾 =================
    else if (ret == AVERROR_EOF) {
        LOG_INFO("Demuxer reached physical EOF(End of File)");
        if (listener_) {
            listener_->OnNotifyDemuxerEOF();
        }
        return false;
    }
    // ================= 底层物理读取异常 =================
    else {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> error_buf{0};
        av_strerror(ret, error_buf.data(), error_buf.size());
        LOG_ERROR("FFmpeg physical read failed: {}", error_buf.data());
        return false;
    }
}
} // namespace av
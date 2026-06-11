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

std::unique_ptr<IDemuxer> IDemuxer::Create() { return std::make_unique<Demuxer>(); }

// 1. 构造/析构
Demuxer::Demuxer() {}

Demuxer::~Demuxer() {
    // 1. 停止解复用线程
    RequestStop();
    WaitForStop();

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
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index_ == -1) {
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
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index_ == -1) {
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

    LOG_INFO("Demuxer OS thread start successfully...");
}

/**
 * @brief 清空解复用器内部状态并向下游分别发送独立的 Flush 信令
 * @note 主要用于Seek操作后清除旧数据，防止花屏与爆音
 */
void Demuxer::Flush() {
    // 1. 音频
    if (audio_stream_index_ != -1 && listener_) {
        auto audio_flush_packet = std::make_shared<Packet>(nullptr);
        audio_flush_packet->AddFlag(av::AVFrameFlag::kFlush);
        listener_->OnNotifyAudioPacket(std::move(audio_flush_packet));
    }

    // 2. 视频
    if (video_stream_index_ != -1 && listener_) {
        auto video_flush_packet = std::make_shared<Packet>(nullptr);
        video_flush_packet->AddFlag(av::AVFrameFlag::kFlush);
        listener_->OnNotifyVideoPacket(std::move(video_flush_packet));
    }

    LOG_INFO("Demuxer flush completed, independent Flush packets sent to decoders");
}
/**
 * @brief 阶段一：下发异步终止信令 --非阻塞
 */
void Demuxer::RequestStop() {
    // 1. 如果已经是退出状态,直接返回
    if (is_abort_requested_.load()) {
        return;
    }

    // 2. 触发终止信令, 并释放可能阻塞的条件变量
    is_abort_requested_.store(true);

    // 3. 唤醒可能正在沉睡的线程，让它看到 is_abort_requested_ 为 true，从而退出循环
    if (sync_ctx_) {
        sync_ctx_->state_cv_.notify_all();
    }

    LOG_INFO("========== Demuxer::RequestStop ==========");
}

/**
 * @brief 阶段二：同步等待回收线程 --阻塞
 */
void Demuxer::WaitForStop() {
    // 1. 停止解解复用
    if (demuxer_thread_.joinable()) {
        demuxer_thread_.join();
    }

    // 2. 状态与指令复位
    is_abort_requested_.store(false);
    is_seek_requested_.store(false);

    // 释放对全局共享状态的引用，防止悬空
    sync_ctx_.reset();
    LOG_INFO("========== Demuxer::WaitForStop ==========");
}

/**
 * @brief 触发绝对时间轴跳转
 * @param target_time_us 目标绝对时间（单位：微秒）
 */
void Demuxer::SeekTo(int64_t target_time_us) {
    // 直接覆盖为最新的目标时间 (Last-writer wins)
    target_seek_time_.store(target_time_us);

    if (!is_seek_requested_.exchange(true)) {
        // 只有当之前没有挂起的 seek 请求时，才需要唤醒条件变量
        if (sync_ctx_) {
            sync_ctx_->state_cv_.notify_all();
        }
    }
}

/**
 * @brief 获取当前媒体容器的全局总时长
 * @return 总时长（单位：秒）
 */
int64_t Demuxer::GetDuration() const {
    std::lock_guard<std::mutex> lock(avformat_mutex_);

    // AV_NOPTS_VALUE 时长未知（无限制/直播/格式不支持文件损坏/正在下载）
    if (!avformat_ctx_ || avformat_ctx_->duration == AV_NOPTS_VALUE) {
        return 0.0;
    }

    // 将 FFmpeg 内部时基 (AV_TIME_BASE, 微秒) 换算为标准绝对秒数
    return avformat_ctx_->duration;
}

/**
 * @brief 探测当前媒体是否包含视频流
 * @return 存在视频流返回 true，否则返回 false
 */
bool Demuxer::HasVideo() const { return video_stream_index_ != -1; }

/**
 * @brief 探测当前媒体是否包含音频流
 * @return 存在音频流返回 true，否则返回 false
 */
bool Demuxer::HasAudio() const { return audio_stream_index_ != -1; }

/**
 * @brief 内部方法：彻底关闭媒体并释放所有物理资源
 * @note 仅供 Open() 和析构函数调用，对上层业务屏蔽
 */
void Demuxer::Close() {
    // 1. 清理资源前，先停止解复用线程
    RequestStop();
    WaitForStop();

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
    while (!is_abort_requested_.load()) {
        // [优先级 P0]: 拦截与处理 Seek 请求
        if (is_seek_requested_.load()) {
            ProcessSeek();
            continue;
        }

        // [优先级 P1]: 线程唤醒机制
        if (sync_ctx_) {
            std::unique_lock<std::mutex> lock(sync_ctx_->state_mutex_);
            sync_ctx_->state_cv_.wait(lock, [this]() {
                // 1. ThreadLoop 被停止了
                // 2. 发生了跳转
                // 3. 处于播放状态
                // 4. 全局停止
                return is_abort_requested_.load() || is_seek_requested_.load() ||
                    sync_ctx_->state_.load() == PlaybackState::kPlaying ||
                    sync_ctx_->state_.load() == PlaybackState::kStopped;
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
/**
 * @brief 响应并执行绝对时间轴跳转逻辑
 * @note 必须在 I/O 线程上下文中执行，防止与 av_read_frame 发生跨线程并发冲突
 */
void Demuxer::ProcessSeek() {
    std::unique_lock<std::mutex> lock(avformat_mutex_);

    if (!avformat_ctx_) {
        is_seek_requested_.store(false);
        return;
    }

    int64_t target_timestamp = target_seek_time_.load();

    // 1. 调用 FFmpeg 底层 API 执行容器级别的物理寻址
    if (av_seek_frame(avformat_ctx_, -1, target_timestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
        LOG_INFO("Demuxer seek succeeded. Target: {} us", target_timestamp);
        is_eof_.store(false);

        // 2. 触发向下游的队列复位信令 (解除 is_abort_ 阻塞状态)
        if (listener_) {
            listener_->OnSeekResetQueues();
        }

        // 3. 清理 FFmpeg 上下文以及下游解码器内部的历史帧参考缓存
        Flush();
    } else {
        LOG_ERROR("Demuxer seek failed for target: {} us", target_timestamp);
        // 寻址失败亦需复位队列，防止管线永久处于 Abort 阻塞状态
        if (listener_) {
            listener_->OnSeekResetQueues();
        }
    }

    bool should_resume = false;

    // 4. 状态机标志位重置与跨线程参数读取
    if (sync_ctx_) {
        std::lock_guard<std::mutex> lock(sync_ctx_->state_mutex_);
        sync_ctx_->is_seeking_.store(false);
        should_resume = sync_ctx_->resume_play_after_seek_.load(); // 获取跳转前的播放意图
        sync_ctx_->state_cv_.notify_all();
    }

    is_seek_requested_.store(false);

    // 5. 触发向上层的回调，请求重新拉起管线进入 kPlaying 状态
    if (should_resume && listener_) {
        listener_->OnSeekCompleteResumePlay();
    }
}

/**
 * @brief 执行底层裸包提取与跨模块派发
 * @return true 循环继续; false 触发 EOF 或物理读取异常
 */
bool Demuxer::ReadAndSendPacket() {
    auto packet = std::make_shared<Packet>();
    int ret = 0;

    if (!avformat_ctx_) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(avformat_mutex_);
        ret = av_read_frame(avformat_ctx_, packet->GetRawPacket());
    }

    if (ret >= 0) {
        // ================= 发送视频包 =================
        if (packet->GetRawPacket()->stream_index == video_stream_index_) {
            try {
                // 若底层队列已满，当前线程将挂起；若处于 Abort 状态，返回 false
                bool push_success = listener_->OnNotifyVideoPacket(std::move(packet));
                if (!push_success) {
                    LOG_INFO("Video packet push aborted by downstream. Exiting block context.");
                    return true; // 抛弃当前 packet，直接进入下一次循环探测指令
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Video packet routing exception: {}", e.what());
            }
        }
        // ================= 发送音频包 =================
        else if (packet->GetRawPacket()->stream_index == audio_stream_index_) {
            try {
                bool push_success = listener_->OnNotifyAudioPacket(std::move(packet));
                if (!push_success) {
                    LOG_INFO("Audio packet push aborted by downstream. Exiting block context.");
                    return true; // 抛弃当前 packet，直接进入下一次循环探测指令
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Audio packet routing exception: {}", e.what());
            }
        }
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
    // ================= 物理读取异常 =================
    else {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> error_buf{0};
        av_strerror(ret, error_buf.data(), error_buf.size());
        LOG_ERROR("FFmpeg physical read failed: {}", error_buf.data());
        return false;
    }
}
} // namespace av
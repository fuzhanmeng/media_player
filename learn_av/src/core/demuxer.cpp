/**
 * @file demuxer.cpp
 * @brief 解封装模块的实现，负责将媒体文件拆解为音视频 Packet 流
 * @details 采用单线程异步解复用架构，支持暂停/继续、进度跳转和背压流量控制
 */

#include "demuxer.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "data/av_packet.h"
#include "interface/idemuxer_listener.h"
#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace av {

// 1. 构造与析构

Demuxer::Demuxer() {}

Demuxer::~Demuxer() {
    // 先停止解复用线程，防止资源被线程访问时释放
    Stop();

    std::lock_guard<std::mutex> lock(avformat_mutex_);
    if (avformat_ctx_) {
        avformat_close_input(&avformat_ctx_);
        avformat_ctx_ = nullptr;
    }
}

// 2. Public Methods

void Demuxer::SetListener(IDemuxerListener* listener) { demuxer_listener_ = listener; }

/**
 * @brief 打开媒体文件并解析流信息
 * @param url 媒体文件路径或网络流地址
 * @return true 打开成功；false 打开失败
 * @note 该方法是同步阻塞的，会执行文件IO和流解析操作
 */
bool Demuxer::Open(const std::string& url) {
    // 1. 加锁
    std::lock_guard<std::mutex> lock(avformat_mutex_);

    // 2. 先停止当前正在进行的播放，清理旧状态
    Stop();

    // 3. 确保旧的格式上下文被完全释放
    if (avformat_ctx_) {
        avformat_close_input(&avformat_ctx_);
        avformat_ctx_ = nullptr;
    }

    // 4. 打开媒体文件，读取文件头信息
    if (avformat_open_input(&avformat_ctx_, url.c_str(), nullptr, nullptr) < 0) {
        LOG_ERROR("Failed to open input file: %s", url.c_str());
        return false;
    }

    // 5. 探测流信息
    if (avformat_find_stream_info(avformat_ctx_, nullptr) < 0) {
        LOG_ERROR("Failed to find stream info for file: %s", url.c_str());

        // 失败时必须关闭已经打开的格式上下文，防止内存泄漏
        avformat_close_input(&avformat_ctx_);
        avformat_ctx_ = nullptr;
        return false;
    }

    // 6. 重置音视频流信息
    video_stream_info_.Reset();
    audio_stream_info_.Reset();

    // 6. 遍历流，找到视频和音频，并通知下游
    for (int i = 0; i < avformat_ctx_->nb_streams; ++i) {
        AVStream* stream = avformat_ctx_->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_info_.stream_index_ = i;
            if (demuxer_listener_) {
                demuxer_listener_->OnNotifyVideoStream(stream);
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_info_.stream_index_ = i;
            if (demuxer_listener_) {
                demuxer_listener_->OnNotifyAudioStream(stream);
            }
        }
    }

    LOG_INFO("Successfully opened file: %s", url.c_str());
    return true;
}

void Demuxer::Start() {
    // 如果线程被销毁了，直接返回
    if (is_abort_requested_.load(std::memory_order_acquire)) {
        return;
    }

    // 如果线程还没有创建，创建后台解复用线程
    if (!demuxer_thread_.joinable()) {
        demuxer_thread_ = std::thread(&Demuxer::ThreadLoop, this);
    }

    // 设置为运行状态并唤醒线程
    is_paused_.store(false, std::memory_order_release);

    demuxer_cv_.notify_all();
    LOG_INFO("Engine Start");
}

void Demuxer::Pause() {
    is_paused_.store(true, std::memory_order_release);
    LOG_INFO("Engine Paused.");
}

void Demuxer::Stop() {
    // 如果已经请求终止，直接返回
    if (is_abort_requested_.load(std::memory_order_acquire)) {
        return;
    }

    // 设置终止标志，通知线程退出
    is_abort_requested_.store(true, std::memory_order_release);
    // 强制解除暂停状态，防止线程死锁在条件变量等待中
    is_paused_.store(false, std::memory_order_release);
    // 唤醒所有等待的线程，让它们执行退出逻辑
    demuxer_cv_.notify_all();

    // 等待线程完全退出
    if (demuxer_thread_.joinable()) {
        demuxer_thread_.join();
    }

    // 重置所有状态标志，允许再次调用Start()
    is_abort_requested_.store(false, std::memory_order_release);
    is_paused_.store(true, std::memory_order_release);
    is_seek_requested_.store(false, std::memory_order_release);

    LOG_INFO("Demuxer stopped");
}

/**
 * @brief 请求跳转到指定进度
 * @param progress 目标进度，范围[0.0, 1.0]
 * @note 该方法是异步非阻塞的，跳转操作会在后台线程中执行
 */
void Demuxer::SeekTo(float progress) {
    // 如果已经有未处理的跳转请求，直接返回
    if (is_seek_requested_.load(std::memory_order_acquire)) {
        return;
    }

    // 快速记录跳转请求，立即返回不阻塞UI线程
    is_seek_requested_.store(true, std::memory_order_release);
    target_seek_progress_.store(progress, std::memory_order_release);
    // 唤醒解复用线程处理跳转
    demuxer_cv_.notify_all();
}

/**
 * @brief 获取媒体文件总时长
 * @return 总时长，单位为秒
 */
float Demuxer::GetDuration() {
    std::lock_guard<std::mutex> lock(avformat_mutex_);
    if (!avformat_ctx_) {
        return 0.0f;
    }
    // FFmpeg的duration单位是微秒(AV_TIME_BASE)，转换为秒返回
    return static_cast<float>(avformat_ctx_->duration) / AV_TIME_BASE;
}

/**
 * @brief 解复用后台线程主循环
 * @details 优先级：处理跳转 > 暂停/背压控制 > 读取数据包
 *          只要Stop()没被调用，线程会一直运行
 */
void Demuxer::ThreadLoop() {
    LOG_INFO("Demuxer thread started");

    while (!is_abort_requested_.load(std::memory_order_acquire)) {
        // 第一优先级：处理跳转请求
        // 必须在条件变量等待之前检查，防止跳转请求被忽略
        if (is_seek_requested_.load(std::memory_order_acquire)) {
            ProcessSeek();
            // 跳转完成后直接进入下一轮循环，防止读到跳转前的脏包
            continue;
        }

        // 第二优先级：背压流量控制与暂停控制
        {
            std::unique_lock<std::mutex> lock(condition_mutex_);
            // 条件变量等待，只有满足以下条件之一时才退出等待：
            // 1. 收到终止请求
            // 2. 收到跳转请求
            // 3. 未暂停且音视频队列都未满
            demuxer_cv_.wait(lock, [this]() {
                return is_abort_requested_.load(std::memory_order_acquire) ||
                    is_seek_requested_.load(std::memory_order_acquire) ||
                    (!is_paused_.load(std::memory_order_acquire) &&
                     video_stream_info_.pipeline_resource_count_.load(std::memory_order_acquire) <
                         kMaxPipelineCapacity &&
                     audio_stream_info_.pipeline_resource_count_.load(std::memory_order_acquire) <
                         kMaxPipelineCapacity);
            });
        }

        // 检查是否需要退出线程
        if (is_abort_requested_.load(std::memory_order_acquire)) {
            break;
        }

        // 第三优先级：读取数据包并发送给下游
        try {
            if (!ReadAndSendPacket()) {
                // 读到文件末尾，使用条件变量等待新的跳转或退出请求
                // 比sleep_for更高效，有请求时可以立即被唤醒
                std::unique_lock<std::mutex> lock(condition_mutex_);
                demuxer_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                    return is_abort_requested_.load(std::memory_order_acquire) ||
                        is_seek_requested_.load(std::memory_order_acquire);
                });
            }
        } catch (const std::exception& e) {
            LOG_ERROR("ReadAndSendPacket threw exception: %s", e.what());
            // 发生异常时休眠一段时间，防止CPU占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    LOG_INFO("Demuxer thread exited normally");
}

/**
 * @brief 内部跳转处理函数
 * @details 在ThreadLoop中被安全调用，与读包操作互斥，防止资源抢占
 */
void Demuxer::ProcessSeek() {
    // 加锁保护格式上下文，防止与读包操作并发访问
    std::unique_lock<std::mutex> lock(avformat_mutex_);
    if (!avformat_ctx_) {
        is_seek_requested_.store(false, std::memory_order_release);
        return;
    }

    // 将目标进度转换为FFmpeg时间戳(微秒)
    int64_t target_timestamp = static_cast<int64_t>(target_seek_progress_.load(std::memory_order_acquire) *
                                                    static_cast<float>(avformat_ctx_->duration));

    // 调用FFmpeg底层API进行跳转
    // AVSEEK_FLAG_BACKWARD：寻找到目标时间戳之前最近的一个关键帧
    if (av_seek_frame(avformat_ctx_, -1, target_timestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
        LOG_INFO("Seek to %.2f seconds succeeded", target_seek_progress_.load(std::memory_order_acquire));

        // 重置背压计数器（关键修复）
        // 跳转后下游队列会被清空，必须重置计数器否则背压机制会失效
        video_stream_info_.pipeline_resource_count_.store(0, std::memory_order_release);
        audio_stream_info_.pipeline_resource_count_.store(0, std::memory_order_release);

        // 发送Flush包通知下游清空队列，丢弃跳转前的脏数据
        if (demuxer_listener_) {
            if (video_stream_info_.stream_index_ >= 0) {
                auto flush_pkt = std::make_shared<Packet>(nullptr);
                flush_pkt->AddFlag(av::AVFrameFlag::kFlush);
                demuxer_listener_->OnNotifyVideoPacket(flush_pkt);
            }
            if (audio_stream_info_.stream_index_ >= 0) {
                auto flush_pkt = std::make_shared<Packet>(nullptr);
                flush_pkt->AddFlag(av::AVFrameFlag::kFlush);
                demuxer_listener_->OnNotifyAudioPacket(flush_pkt);
            }
        }
    } else {
        LOG_ERROR("Seek to %.2f seconds failed", target_seek_progress_.load(std::memory_order_acquire));
    }

    // 重置跳转请求标志
    is_seek_requested_.store(false, std::memory_order_release);
}

/**
 * @brief 读取单个包并推给下游解码器
 * @return true 继续读取；false 读到文件末尾或发生错误
 * @note 使用RAII机制保证背压计数器一定被递减，防止异常导致的泄漏
 */
bool Demuxer::ReadAndSendPacket() {
    // 创建一个空的Packet对象，内部自动调用av_packet_alloc
    auto packet = std::make_shared<av::Packet>();
    int ret = 0;

    if (!avformat_ctx_) {
        LOG_ERROR("avformat_ctx_ is nullptr");
        return false;
    }

    // 加锁保护格式上下文，防止与跳转操作并发访问
    {
        std::unique_lock<std::mutex> lock(avformat_mutex_);
        ret = av_read_frame(avformat_ctx_, packet->GetRawPacket());
    }

    if (ret >= 0) {
        // 处理视频包
        if (packet->GetRawPacket()->stream_index == video_stream_info_.stream_index_) {
            // 使用RAII保证计数器一定被递减
            // 即使下游回调抛出异常，count_guard的析构函数也会被调用
            auto count_guard = std::shared_ptr<std::function<void()>>(nullptr, [this](void*) {
                video_stream_info_.pipeline_resource_count_--;
                demuxer_cv_.notify_all(); // 通知解复用线程可以继续读取
            });
            video_stream_info_.pipeline_resource_count_++;

            // 将计数器递减的回调设置到Packet上
            // 当Packet被所有引用者释放后，回调会自动执行
            packet->SetReleaseCallback(count_guard);

            try {
                demuxer_listener_->OnNotifyVideoPacket(packet);
            } catch (const std::exception& e) {
                LOG_ERROR("OnNotifyVideoPacket threw exception: %s", e.what());
            }

        }
        // 处理音频包
        else if (packet->GetRawPacket()->stream_index == audio_stream_info_.stream_index_) {
            auto count_guard = std::shared_ptr<std::function<void()>>(nullptr, [this](void*) {
                audio_stream_info_.pipeline_resource_count_--;
                demuxer_cv_.notify_all();
            });
            audio_stream_info_.pipeline_resource_count_++;

            packet->SetReleaseCallback(count_guard);

            try {
                demuxer_listener_->OnNotifyAudioPacket(packet);
            } catch (const std::exception& e) {
                LOG_ERROR("OnNotifyAudioPacket threw exception: %s", e.what());
            }
        }
        // 其他流(字幕、数据等)直接丢弃
        return true;
    }
    // 读到文件末尾
    else if (ret == AVERROR_EOF) {
        LOG_INFO("Reached end of file");
        if (demuxer_listener_) {
            demuxer_listener_->OnNotifyEOF();
        }
        return false;
    }
    // 发生严重读取错误
    else {
        char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err_buf, sizeof(err_buf));
        LOG_ERROR("av_read_frame failed: %s", err_buf);
        return false;
    }
}

// 4. Interface Factory Method (接口工厂方法)

std::unique_ptr<IDemuxer> IDemuxer::Create() { return std::make_unique<Demuxer>(); }

} // namespace av
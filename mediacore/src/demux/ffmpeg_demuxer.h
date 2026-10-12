/**
 * @file ffmpeg_demuxer.h
 * @brief 基于 FFmpeg libavformat 实现的解复用器接口封装（Open / Read / Seek）
 * @details
 * 负责解析媒体容器头、提取主视频流并提供统一的数据读取与定位接口：
 * 1. 职责边界：只负责 demux（解封装）产出压缩 Packet，不创建线程，不负责音视频解码；
 * 2. 单流过滤：仅保留选中的视频流，读取到音频、字幕等其他附带流时，Read() 返回 kSkipped；
 * 3. 线程模型契约：非线程安全。Open() 可在任意线程调用（内部包含耗时阻塞 I/O）；
 *    一旦打开成功，同一个 FfmpegDemuxer 实例的 Read() 与 Seek() 必须由同一个工作线程（如 DemuxerNode 线程）串行调用；
 * 4. 协作式中断：通过 DemuxerInterruptHandle 注入 AVIOInterruptCB 回调，支持对挂起的网络/磁盘 I/O 进行外部安全打断。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "av_types/packet.h"
#include "av_types/video_codec_parameters.h"
#include "mediacore/av_types/stream_info.h"
#include "mediacore/media_error.h"

struct AVFormatContext;

namespace mediacore {

/**
 * @enum DemuxerInterruptReason
 * @brief FFmpeg 底层阻塞调用提前退出的原因
 */
enum class DemuxerInterruptReason : std::uint8_t {
    kNone,  ///< 无中断请求，正常执行
    kSeek,  ///< 收到新的 Seek 请求，当前正阻塞的 Read/Seek 操作应尽快放弃并返回
    kStop,  ///< 播放器正在停机，当前及后续所有底层 FFmpeg 调用均应立即终止
};

/**
 * @class DemuxerInterruptHandle
 * @brief 用于外部控制并打断底层 FFmpeg 耗时操作的共享控制句柄
 * @details
 * 解决 Stop 与 Seek 在并发时序中的不同语义：
 * - Stop 属于不可逆的单向门：一旦触发，永久有效；
 * - Seek 属于单次打断：仅让“当时正在执行的旧 Read/Seek”尽快返回。处理完 Seek 后，下一次 Read 必须能够恢复正常读包。
 * 内部基于自增计数器管理 Seek 轮次，避免单一 bool 标志位误将后续合法的读取操作全部熔断。
 */
class DemuxerInterruptHandle final {
public:
    /**
     * @brief 构造初始共享控制状态（无 Stop 且无 Seek 中断请求）
     */
    DemuxerInterruptHandle();

    /**
     * @brief 默认析构函数
     */
    ~DemuxerInterruptHandle() = default;

    /**
     * @brief 具备共享语义：支持拷贝与移动，所有副本共享内部同一个控制状态
     */
    DemuxerInterruptHandle(const DemuxerInterruptHandle&) noexcept = default;
    DemuxerInterruptHandle& operator=(const DemuxerInterruptHandle&) noexcept = default;

    DemuxerInterruptHandle(DemuxerInterruptHandle&&) noexcept = default;
    DemuxerInterruptHandle& operator=(DemuxerInterruptHandle&&) noexcept = default;

    /**
     * @brief 请求永久停止所有底层 FFmpeg 操作（幂等原子操作）
     * @note 仅设置停止标志位，使 InterruptCallback 触发返回 1，实际资源释放在工作线程退出后进行
     */
    void RequestStop() noexcept;

    /**
     * @brief 查询是否已收到永久停止请求
     * @return true 表示已被要求停止
     */
    bool IsStopRequested() const noexcept;

private:
    friend class FfmpegDemuxer;

    /**
     * @brief 请求因新的 Seek 而打断当前阻塞的 I/O
     * @note 自增内部 Seek 编号，促使当前阻塞的底层调用快速退出
     */
    void RequestSeekInterrupt() noexcept;

    /**
     * @brief 在工作线程开始下一次阻塞 I/O 前，锚定当前的 Seek 计数基准
     * @details 记录当前 Seek 计数；若在 I/O 阻塞期间外部再次调用 RequestSeekInterrupt()，
     *          计数器发生变化，InterruptCallback 即可识别出新 Seek 从而要求中断
     */
    void PrepareNextIoForSeekInterrupt() noexcept;

    /**
     * @brief 获取当前中断的原因分类（Stop 的优先级高于 Seek）
     */
    DemuxerInterruptReason GetInterruptReason() const noexcept;

    struct State;
    std::shared_ptr<State> state_;  ///< 堆上共享控制状态（包含原子停止标志和 Seek 轮次计数器）
};

/**
 * @enum FfmpegReadStatus
 * @brief 单次 Read() 调用后的返回状态
 */
enum class FfmpegReadStatus : std::uint8_t {
    kPacket,  ///< 成功读取并筛选出一个完整的目标视频压缩包
    kSkipped,  ///< 读取到非视频流（如音频、字幕等），数据包已安全释放，调用方可继续 Read
    kEndOfStream,  ///< 视频流数据已完全读完（EOF）
    kSeekInterrupted,  ///< 本次读取因外部发起新 Seek 而被提前打断，工作线程应转入 Seek 处理
    kStopInterrupted,  ///< 本次读取因收到 Stop 请求而被中断退出
    kError,  ///< FFmpeg 读取发生底层 I/O 或协议错误，详情见 error_
};

/**
 * @struct FfmpegReadResult
 * @brief Read() 调用的结构化返回载荷
 */
struct FfmpegReadResult final {
    FfmpegReadStatus status_{FfmpegReadStatus::kError};  ///< 本次读取执行状态
    MediaError error_{};  ///< 发生错误时的详细结构化错误对象
};

/**
 * @enum FfmpegSeekStatus
 * @brief FfmpegDemuxer::Seek() 调用的执行状态
 */
enum class FfmpegSeekStatus : std::uint8_t {
    kSucceeded,  ///< 底层跳转成功，已定位到关键帧并冲刷解复用缓冲区
    kSeekInterrupted,  ///< 本次跳转因更晚到来的新 Seek 打断
    kStopInterrupted,  ///< 本次跳转因 Stop 请求打断
    kError,  ///< 跳转发生底层错误，详情见 error_
};

/**
 * @struct FfmpegSeekResult
 * @brief Seek() 调用的结构化返回载荷
 */
struct FfmpegSeekResult final {
    FfmpegSeekStatus status_{FfmpegSeekStatus::kError};  ///< 本次跳转执行状态

    /**
     * @brief Demux 层估算定位到的时间戳（微秒）
     * @note av_seek_frame() 不保证返回实际命中的关键帧绝对时间，通常保持为 std::nullopt；
     *       最终呈现时间由下游解码或渲染节点的第一帧实际输出确定
     */
    std::optional<std::int64_t> best_effort_position_us_{};

    MediaError error_{};  ///< 发生错误时的详细错误对象

    /**
     * @brief 判定本次 Seek 是否成功
     */
    bool IsSucceeded() const noexcept { return status_ == FfmpegSeekStatus::kSucceeded; }
};

class FfmpegDemuxer;

/**
 * @struct FfmpegDemuxerOpenResult
 * @brief FfmpegDemuxer::Open() 静态工厂方法的输出结构体
 * @details 成功时产出两个互相解耦的独立对象：
 *          - demuxer_：独占解复用器实例，用于后续解包；
 *          - video_codec_parameters_：解封装解析出的视频编解码器配置副本，可直接送入解码器初始化。
 */
struct FfmpegDemuxerOpenResult final {
    std::unique_ptr<FfmpegDemuxer> demuxer_{};  ///< 成功创建的解复用器对象独占所有权
    VideoCodecParameters video_codec_parameters_{};  ///< 用于配置视频解码器的参数元数据
    MediaError error_{};  ///< 失败时的详细错误信息
};

/**
 * @class FfmpegDemuxer
 * @brief 拥有并管理 AVFormatContext 的视频解封装执行器
 * @details 负责文件解析、数据包过滤和时间基准换算，输出标准化压缩 Packet。
 */
class FfmpegDemuxer final {
public:
    /**
     * @brief 析构函数：负责关闭输入流并释放底层的 AVFormatContext 内存
     */
    ~FfmpegDemuxer();

    // 严禁拷贝与移动，确保底层资源生命周期唯一确定
    FfmpegDemuxer(const FfmpegDemuxer&) = delete;
    FfmpegDemuxer& operator=(const FfmpegDemuxer&) = delete;
    FfmpegDemuxer(FfmpegDemuxer&&) = delete;
    FfmpegDemuxer& operator=(FfmpegDemuxer&&) = delete;

    /**
     * @brief 静态工厂：打开媒体源、解析容器元数据并定位视频流
     * @param uri 本地文件绝对路径或网络流 URL
     * @param interrupt 外部传入的中断句柄，支持在耗时探测时异步打断
     * @return 包含 Demuxer 实例和视频参数的复合打开结果
     * @note 属于高耗时阻塞调用，建议在后台线程中执行
     */
    static FfmpegDemuxerOpenResult Open(const std::string& uri, DemuxerInterruptHandle interrupt = {});

    /**
     * @brief 读取下一个有效视频数据包
     * @param packet 输出参数：成功时写入压缩数据载荷与时间戳
     * @return 读取结果状态（kPacket / kSkipped / kEndOfStream / 中断 / 错误）
     * @note 遇到音频等非目标流时直接跳过并返回 kSkipped，由外部循环继续读取
     */
    FfmpegReadResult Read(Packet& packet);

    /**
     * @brief 将输入流跳转定位到指定的目标时间戳
     * @param target_us 目标时间，以媒体播放零时刻为基准计算（微秒）
     * @return 跳转状态结果
     * @note 仅修改 libavformat 的内部读取偏移并重置 EOF 状态；下游节点需自行清空解码缓存
     */
    FfmpegSeekResult Seek(std::int64_t target_us);

    /**
     * @brief 永久停止当前及后续的 FFmpeg 操作
     */
    void RequestStop() noexcept { interrupt_.RequestStop(); }

    /**
     * @brief 请求打断当前正在阻塞的 FFmpeg 操作以执行新 Seek
     */
    void RequestSeekInterrupt() noexcept { interrupt_.RequestSeekInterrupt(); }

    /**
     * @brief 为下一次 FFmpeg 调用记录 Seek 中断基准
     */
    void PrepareNextIoForSeekInterrupt() noexcept { interrupt_.PrepareNextIoForSeekInterrupt(); }

    /**
     * @brief 获取媒体容器的元数据快照
     */
    const MediaInfo& GetMediaInfo() const noexcept { return media_info_; }

    /**
     * @brief 获取选中的视频流在容器中的流索引号（Stream Index）
     */
    int GetVideoStreamIndex() const noexcept { return video_stream_index_; }

private:
    /**
     * @brief 私有构造函数，由 Open() 验证成功后通过 std::make_unique 创建
     * @param av_format_ctx 已成功打开的 FFmpeg 输入上下文句柄
     * @param media_info 解析提取出的媒体元数据
     * @param video_stream_index 目标视频流对应的索引
     * @param interrupt 共享中断控制句柄
     * @param timeline_origin_ts 零时间轴对应的流原始基准时间戳（按 video_stream->time_base 换算）
     */
    FfmpegDemuxer(AVFormatContext* av_format_ctx,
                  MediaInfo media_info,
                  int video_stream_index,
                  DemuxerInterruptHandle interrupt,
                  std::int64_t timeline_origin_ts) noexcept;

    /**
     * @brief 注册给 FFmpeg AVIOInterruptCB 的底层 C 语言回调函数
     * @param opaque 传回的 DemuxerInterruptHandle 指针
     * @return 1 表示要求 FFmpeg 立即退出阻塞，0 表示继续等待
     */
    static int InterruptCallback(void* opaque) noexcept;

    AVFormatContext* av_format_ctx_{nullptr};  ///< 拥有的 FFmpeg 输入上下文
    MediaInfo media_info_{};  ///< 媒体流元信息缓存
    int video_stream_index_{-1};  ///< 目标视频流索引
    DemuxerInterruptHandle interrupt_{};  ///< 中断控制句柄

    /**
     * @brief 播放器零时刻在底层视频流原始时间轴中的基准偏移
     * @details
     * 单位为目标流的 time_base 刻度（Tick）。
     * 当媒体存在非零 start_time（如推流、TS 分段）时，用于校准消除播放起始时间偏移：
     * $PTS_{playback} = PTS_{raw} - timeline\_origin\_ts$。
     */
    std::int64_t timeline_origin_ts_{0};

    bool eof_reached_{false};  ///< 是否已读到流末尾；成功 Seek 后会被重置为 false
};

}  // namespace mediacore
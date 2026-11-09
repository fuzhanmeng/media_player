/**
 * @file ffmpeg_video_decoder.h
 * @brief 基于 FFmpeg 的视频解码器，支持 VA-API 硬件解码和软件解码
 * @details
 * 解码器负责把压缩的视频 Packet 转换为项目统一的 VideoFrame
 * 有两种输出形式：
 * 1. 硬件帧：
 *    VA-API 解码 → VA Surface → DMA-BUF → HardwareFrame
 *
 *    图像数据保留在 GPU 可以访问的内存中，不需要先复制到 CPU 内存
 *    后续由 Renderer 负责将 DMA-BUF 导入 EGLImage，再交给 OpenGL 渲染
 *
 * 2. 软件帧：
 *    软件解码，或者将硬件帧下载到 CPU 内存
 *       → 像素格式转换
 *       → YUV420P VideoFrame
 *
 *    图像数据存放在 CPU 内存中，后续由 Renderer 上传到 GPU
 *
 * 职责划分：
 * 解码器负责视频解码、DMA-BUF 导出和硬件帧资源管理
 * EGLImage 导入和 OpenGL 渲染由 Renderer 负责
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "av_types/packet.h"
#include "av_types/video_codec_parameters.h"
#include "mediacore/av_types/stream_info.h"
#include "mediacore/av_types/video_frame.h"
#include "mediacore/media_error.h"
#include "mediacore/player_config.h"
#include "mediacore/perf_metrics.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/pixfmt.h>
}

struct SwsContext;

namespace mediacore {
class FfmpegVideoDecoder;

/**
 * @brief 向 FFmpeg 提交压缩数据包后的结果状态
 */
enum class FfmpegDecoderSendStatus : std::uint8_t {
    kAccepted,  // 数据包已经成功提交给解码器
    kNeedReceive,  // 解码器暂时无法接收新数据，需要先读取已经解码的帧
    kDrained,  // 解码器已经结束接收数据
    kError  // 提交数据时发生错误
};

/**
 * @brief 从 FFmpeg 获取解码帧后的结果状态
 */
enum class FfmpegDecoderReceiveStatus : std::uint8_t {
    kFrame,  // 成功获取一帧
    kNeedInput,  // 当前没有可读取的帧，需要继续提交数据包
    kDrained,  // 解码器内部缓存已经读取完毕
    kError  // 获取解码帧时发生错误
};

/**
 * @brief 提交压缩视频数据包后的结果
 */
struct FfmpegDecoderSendResult final {
    FfmpegDecoderSendStatus status_{FfmpegDecoderSendStatus::kError};
    MediaError media_error_{};
};

/**
 * @brief 获取解码帧后的结果
 */
struct FfmpegDecoderReceiveResult final {
    FfmpegDecoderReceiveStatus status_{FfmpegDecoderReceiveStatus::kError};

    // 只有 status_ == kFrame 时，frame_ 才有有效值。
    std::optional<VideoFrame> frame_{};

    MediaError media_error_{};
};

/**
 * @brief 硬件像素格式协商状态
 * @details
 * FFmpeg 在打开解码器时，不一定马上确定最终使用哪种像素格式
 * get_format 回调会在解码的过程中被 FFmpeg 调用，用来从解码器提供的格式列表中选择实际使用的格式
 * opaque 只保存这个对象的地址，不负责管理对象生命周期
 */

/**
 * @brief 创建并打开 FFmpeg 视频解码器后的结果
 */
struct FfmpegVideoDecoderOpenResult final {
    // 打开成功时保存解码器对象,打开失败时为空
    std::unique_ptr<class FfmpegVideoDecoder> video_decoder_{};

    // 打开失败时保存错误信息
    MediaError error_{};

    /**
     * @brief 判断解码器是否成功创建
     *
     * 同时满足：
     * 1. 解码器对象存在；
     * 2. 没有错误信息
     */
    bool Succeeded() const noexcept { return video_decoder_ != nullptr && !error_.HasError(); }
};

/**
 * @brief 保存 FFmpeg 选择像素格式时需要使用的信息
 * @details
 * 本结构体保存两项信息：
 *
 * 1. acceleration_mode_：当前采用什么硬件加速策略。
 * 2. is_vaapi_selected_：FFmpeg 最终是否选择了 VA-API 硬件像素格式
 */
struct HardwarePixelFormatSelection {
    // 当前硬件加速策略，由创建解码器时传入
    HardwareAccel acceleration_mode_{HardwareAccel::kAuto};

    // FFmpeg 是否已经选择 AV_PIX_FMT_VAAPI,只有实际选择 VA-API 格式后才会变为 true
    bool is_vappi_selected_{false};
    AVPixelFormat logged_format_{AV_PIX_FMT_NONE};
};

/**
 * @class FfmpegVideoDecoder
 * @brief FFmpeg 视频解码器
 * @details
 *
 * 支持：
 * 1. FFmpeg 软件解码
 * 2. VA-API 硬件解码
 * 3. 将 VA-API 解码帧导出为 DMA-BUF
 * 4. DMA-BUF 导出失败时，将硬件帧下载到 CPU 内存
 */
class FfmpegVideoDecoder final {
public:
    /**
     * @brief 根据视频流参数创建并打开解码器
     * @param parameters 视频编码参数
     * @param video_stream_time_base 视频流时间基
     * @param hardware_acceleration 硬件加速策略
     * @details
     * hardware_acceleration 是项目定义的 HardwareAccel 类型，用来决定是否使用 VA-API
     */
    static FfmpegVideoDecoderOpenResult Open(VideoCodecParameters parameters,
                                             Rational video_stream_time_base,
                                             HardwareAccel hardware_acceleration,
                                             Rational video_frame_rate = {});

    ~FfmpegVideoDecoder();

    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    /**
     * @brief 将压缩视频数据包提交给 FFmpeg
     *
     * 调用 avcodec_send_packet() 后，不一定马上得到解码帧。
     * 需要继续调用 ReceiveFrame()，读取解码器输出的帧。
     */
    FfmpegDecoderSendResult SendPacket(const Packet& packet);

    /**
     * @brief 通知 FFmpeg 不再有新的数据包，开始读取剩余解码帧
     *
     * 通过 avcodec_send_packet(codec_context_, nullptr)
     * 通知 FFmpeg 输入已经结束。
     *
     * 但解码器内部可能还有尚未输出的帧，
     * 例如 B 帧重排序过程中暂存的帧。
     * 后续仍需要调用 ReceiveFrame()，直到返回 kDrained。
     */
    FfmpegDecoderSendResult BeginDrain();

    /**
     * @brief 从 FFmpeg 获取一帧，并转换为 VideoFrame
     * @param epoch 当前视频流的世代号
     * @details
     * 返回的 VideoFrame 有两种形式：
     * 1. 硬件帧：
     *    hardware_ 持有 HardwareFrame，图像数据仍然位于 GPU 可访问的内存中
     *
     * 2. 软件帧：buffer_ 持有 CPU 内存中的像素数据
     */
    FfmpegDecoderReceiveResult ReceiveFrame(std::uint64_t epoch);

    /**
     * @brief 清空 FFmpeg 解码器内部缓存
     *
     * 通常在 Seek 跳转时调用
     *
     * avcodec_flush_buffers() 用于清空解码器内部缓存
     * av_frame_unref() 用于释放当前复用 AVFrame 持有的资源引用
     */
    void Flush() noexcept;

    /**
     * @brief 获取底层 FFmpeg 解码器名称
     *
     * 例如 h264、hevc。
     */
    const std::string& GetVideoCodecName() const noexcept { return codec_name_; }

    /**
     * @brief 获取码流声明的编码宽度
     */
    int GetCodedWidth() const noexcept;

    /**
     * @brief 获取码流声明的编码高度
     */
    int GetCodedHeight() const noexcept;

    /**
     * @brief 设置渲染端的硬件帧接收能力
     * @param capability 与渲染端共享的布尔原子标志
     * @details
     * 主要用于设置是否具有硬解码的能力
     * true ： 渲染端可以接收 DMA-BUF 硬件帧，解码器允许尝试导出
     * false：渲染端尚未准备好或不支持 DMA-BUF 导入，解码器跳过 DMA-BUF 导出，直接将硬件帧下载到 CPU 内存
     * nullptr：不参与渲染端能力检查，解码器只根据自身的 dma_buf_export_enabled_ 决定是否尝试导出
     */
    void SetHardwareRenderCapability(std::shared_ptr<std::atomic<bool>> capability);

    /** @brief 输出解码统计；EOS 摘要由 Decoder Node 在完成 Drain 后调用。 */
    void LogStatistics(const char* reason, bool summary);
    void SetStatisticsEpoch(std::uint64_t epoch);
    void RecordSkippedCorruptInput() noexcept;

private:
    struct Statistics final {
        std::uint64_t accepted_packets_{0};
        std::uint64_t packet_bytes_{0};
        std::uint64_t send_eagain_{0};
        std::uint64_t receive_eagain_{0};
        std::uint64_t software_decode_frames_{0};
        std::uint64_t vaapi_decode_frames_{0};
        std::uint64_t export_api_attempts_{0};
        std::uint64_t export_api_success_{0};
        std::uint64_t export_api_failures_{0};
        std::uint64_t export_pipeline_attempts_{0};
        std::uint64_t export_pipeline_failures_{0};
        std::uint64_t cpu_download_bytes_{0};
        std::uint64_t skipped_corrupt_inputs_{0};
        std::array<std::uint64_t, 4> fallback_frames_{};
        perf::Timing decode_call_wall_{};
        perf::Timing decode_call_cpu_{};
        std::uint64_t codec_frames_{0};
        std::uint64_t decoded_frames_{0};
        std::uint64_t errors_{0};
        std::uint64_t send_calls_{0};
        std::uint64_t drain_send_calls_{0};
        std::uint64_t receive_calls_{0};
        std::uint64_t va_sync_calls_{0};
        std::uint64_t hw_transfer_calls_{0};
        std::uint64_t vaapi_export_frames_{0};
        std::uint64_t vaapi_download_frames_{0};
        std::uint64_t software_frames_{0};
        std::uint64_t decoder_thread_cpu_ns_{0};
        double send_api_ms_{0.0};
        double max_send_api_ms_{0.0};
        double drain_send_api_ms_{0.0};
        double max_drain_send_api_ms_{0.0};
        double receive_api_ms_{0.0};
        double max_receive_api_ms_{0.0};
        double frame_processing_ms_{0.0};
        double max_frame_processing_ms_{0.0};
        double va_sync_ms_{0.0};
        double max_va_sync_ms_{0.0};
        double hw_transfer_ms_{0.0};
        double max_hw_transfer_ms_{0.0};
    };

    /**
     * @brief 私有构造函数
     * @details
     * 只允许 Open() 创建解码器对象，传入的 FFmpeg 资源由当前对象接管，后续由析构函数统一释放
     */
    FfmpegVideoDecoder(AVCodecContext* codec_ctx,
                       AVFrame* decoded_frame,
                       Rational video_stream_time_base,
                       Rational video_frame_rate,
                       HardwareAccel requested_acceleration,
                       std::string codec_name,
                       AVBufferRef* hardware_device_context,
                       std::unique_ptr<HardwarePixelFormatSelection> hardware_pixel_format_selection) noexcept;

    void LogFirstFrameInfo(const VideoFrame& output_frame,
                           AVPixelFormat decoded_pixel_format,
                           int decoded_width,
                           int decoded_height);
    void MaybeLogIntervalStatistics();
    void ResetIntervalStatistics() noexcept;
    void RecordSend(double elapsed_ms, bool accepted, int bytes, bool eagain) noexcept;
    void RecordDrainSend(double elapsed_ms) noexcept;
    void RecordReceive(double elapsed_ms, bool produced_frame) noexcept;
    void RecordFrameProcessing(double elapsed_ms) noexcept;
    void RecordOutputPath(AVPixelFormat decoded_pixel_format, const VideoFrame& output_frame) noexcept;
    void RecordVaSync(double elapsed_ms) noexcept;
    void RecordHardwareTransfer(double elapsed_ms) noexcept;
    void RecordError() noexcept;
    void RecordDecodeCall(std::int64_t wall_ns, std::int64_t cpu_ns) noexcept;
    void LogHardwareExportFailure(const char* stage, std::uint64_t epoch, int va_status = -1);
    void RecordDownloadFallback(std::size_t reason, std::uint64_t epoch);
    class DecodeCallMeasurement final {
    public:
        explicit DecodeCallMeasurement(FfmpegVideoDecoder& owner) noexcept;
        ~DecodeCallMeasurement();
    private:
        FfmpegVideoDecoder& owner_;
        std::int64_t wall_started_ns_;
        std::int64_t cpu_started_ns_;
    };

    /**
     * @brief FFmpeg 请求项目选择解码输出像素格式时调用的回调
     * @param codec_context FFmpeg 当前正在使用的解码上下文
     * @param candidate_pixel_formats FFmpeg 提供的候选像素格式列表，以 AV_PIX_FMT_NONE 结尾
     * @details
     * FFmpeg 会把当前可以使用的像素格式列表传进来
     * 1. 如果列表中有 AV_PIX_FMT_VAAPI，优先选择它
     *
     * 2. 如果没有 VA-API 格式：
     *    - 强制硬件解码时，返回 AV_PIX_FMT_NONE，表示无法继续
     *    - 自动模式下，尝试选择软件像素格式
     *
     * 3. 如果没有可用的软件格式，则返回 AV_PIX_FMT_NONE
     */
    static AVPixelFormat SelectDecoderPixelFormat(AVCodecContext* codec_context,
                                                  const AVPixelFormat* candidate_pixel_formats);

    /**
     * @brief 将 FFmpeg 解码得到的帧转换为项目统一的 VideoFrame
     * @details
     * 根据解码输出格式选择处理方式:
     * 硬件帧路径：
     *
     * AV_PIX_FMT_VAAPI
     * 尝试导出 DMA-BUF
     * 导出成功：封装为 HardwareFrame
     * 导出失败：下载到 CPU 内存
     *
     * 软件帧路径：
     * CPU 内存中的像素数据
     * libswscale 转换
     * YUV420P VideoFrame
     */
    FfmpegDecoderReceiveResult ConvertDecodedFrameToVideoFrame(std::uint64_t epoch);

    /**
     * @brief 尝试将当前的 VA-API 解码帧导出为 DMA-BUF
     * @return
     * 返回值：
     *   成功时返回 HardwareFrame
     *   失败时返回 nullptr，由上层执行硬件帧下载回退
     * @details
     * 这是硬件解码到 GPU 渲染链路中的一部分
     * 主要工作：
     * 1. 从 AVFrame 中获取到 VA-API 设备和 Surface ID
     * 2. 等待 VA-API 硬件完成当前帧的写入
     * 3. 将 VA Surface 导出为 DMA-BUF
     * 4. 为导出的 DMA-BUF 创建独立的 fd，由 HardwareFrame 持有
     * 5. 保存图像格式、内存布局、plane 和 object 的对应关系
     * 6. 保存当前 AVFrame 的引用，防止 VA Surface 在下游仍使用时被解码器重新使用
     *
     */
    std::unique_ptr<HardwareFrame> ExportHardwareFrame(std::uint64_t epoch);

private:
    /**
     * @brief 像素格式选择状态对象
     * Open() 创建该对象后，会把它的地址保存到：codec_context_->opaque
     */
    std::unique_ptr<HardwarePixelFormatSelection> hardware_pixel_format_selection_{};

    /**
     * @brief 当前解码器持有的 VA-API 设备引用
     * AVBufferRef 是 FFmpeg 提供的资源引用，它指向 FFmpeg 内部管理的 AVBuffer，由 AVBuffer 维护引用计数
     */
    AVBufferRef* hardware_device_context_ref_{nullptr};

    /**
     * @brief FFmpeg 解码上下文
     *
     * 这是 FFmpeg 提供的 AVCodecContext 对象
     *
     * 负责管理：
     * 1. 当前使用的解码器；
     * 2. 输入的压缩数据包；
     * 3. 解码过程中的内部缓存；
     * 4. 硬件解码设备和像素格式选择回调等信息
     */
    AVCodecContext* codec_context_{nullptr};

    /**
     * @brief 用来接收解码结果的 AVFrame
     */
    AVFrame* decoded_frame_{nullptr};

    /**
     * @brief libswscale 像素格式转换上下文
     *
     * 用于软件像素格式转换，例如将 NV12、YUV422P 等格式转换为项目使用的 YUV420P
     * 硬件帧成功导出 DMA-BUF 时，不需要经过这个转换过程
     */
    SwsContext* software_scaler_context_{nullptr};

    /**
     * @brief 视频流时间基
     *
     * 用于把 FFmpeg 输出帧的时间戳换算为微秒。
     */
    Rational video_stream_time_base_{};

    Rational video_frame_rate_{};

    /**
     * @brief 当前使用的 FFmpeg 解码器名称
     */
    std::string codec_name_{};

    std::uint64_t diagnostic_stream_id_{0};
    HardwareAccel requested_acceleration_{HardwareAccel::kAuto};
    bool hardware_device_initialized_{false};
    bool first_frame_logged_{false};
    bool summary_logged_{false};
    bool statistics_epoch_set_{false};
    bool first_epoch_frame_logged_{false};
    bool thread_cpu_available_{true};
    std::uint64_t statistics_epoch_{0};
    std::array<int, 3> first_hardware_layout_{};
    bool hardware_frame_logged_{false};
    std::array<std::uint64_t, 128> logged_export_layout_{};
    bool export_layout_logged_{false};
    struct ExportFailureCounter {
        const char* stage;
        std::uint64_t count{0};
    };
    // Fixed storage: recording the bad_alloc failure itself must not allocate.
    std::array<ExportFailureCounter, 16> export_failure_reasons_{{
        {"invalid_hardware_frame"}, {"invalid_frames_device"}, {"invalid_va_display"}, {"invalid_surface"},
        {"vaSyncSurface"}, {"vaExportSurfaceHandle"}, {"unsupported_descriptor_layout"},
        {"inconsistent_va_drm_pixel_format"}, {"invalid_dma_buf_object"}, {"repeated_dma_buf_fd"},
        {"dup_fd"}, {"unsupported_format_modifier_plane_bounds"}, {"av_frame_ref"}, {"bad_alloc"},
        {"length_error"}, {"other"}}};
    const char* last_export_failure_stage_{nullptr};
    std::chrono::steady_clock::time_point last_export_failure_logged_at_{};
    std::array<std::chrono::steady_clock::time_point, 4> fallback_logged_at_{};
    std::int64_t process_cpu_started_ns_{-1};
    std::int64_t process_interval_started_ns_{-1};
    std::string actual_mode_{"pending"};
    std::string input_pixel_format_{"pending"};
    std::string output_pixel_format_{"pending"};
    int decoded_frame_width_{0};
    int decoded_frame_height_{0};
    Statistics total_statistics_{};
    Statistics interval_statistics_{};
    std::chrono::steady_clock::time_point statistics_started_at_{};
    std::chrono::steady_clock::time_point interval_started_at_{};

    /**
     * @brief 是否允许尝试 DMA-BUF 导出：默认允许
     */
    bool dma_buf_export_enabled_{true};

    /**
     * @brief 连续 DMA-BUF 导出失败的次数
     * 导出成功后清零，连续失败达到设定次数后，关闭当前流的 DMA-BUF 导出尝试
     */
    int consecutive_export_failures_{0};

    /**
     * @brief 渲染端的硬件帧接收能力标志
     * 由 Player 注入，与 OpenGLView 共享同一个 atomic<bool> 对象。
     *
     * nullptr：
     *   不检查渲染端能力，只根据 dma_buf_export_enabled_ 决定是否导出
     *
     * true：
     *   渲染端可以接收 DMA-BUF 硬件帧
     *
     * false：
     *   渲染端暂时不能接收 DMA-BUF 硬件帧，跳过导出并走 CPU 下载路径
     *
     * OpenGLView 初始化完成后更新该标志。
     * 解码工作线程读取该标志，决定是否尝试 DMA-BUF 导出。
     */
    std::shared_ptr<std::atomic<bool>> hardware_render_capability_{nullptr};

    /**
     * @brief 是否已经通知 FFmpeg 输入结束，BeginDrain() 成功发送结束信号后变为 true
     */
    bool drain_started_{false};

    /**
     * @brief 是否已经读取完解码器内部剩余的帧，ReceiveFrame() 收到 FFmpeg 的 AVERROR_EOF 后变为 true
     */
    bool drained_{false};
};

}  // namespace mediacore

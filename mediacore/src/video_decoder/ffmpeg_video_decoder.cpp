/**
 * @file ffmpeg_video_decoder.cpp
 * @brief 基于 FFmpeg libavcodec 与 libswscale 的底层视频解码器实现
 * @details
 * 负责执行压缩数据包解码与目标色彩空间转码：
 * 1. 状态机驱动：遵循 avcodec_send_packet() 与 avcodec_receive_frame() 的异步流转规则；
 * 2. 协作排空与重置：支持 nullptr packet 进入 Drain 流程，以及 avcodec_flush_buffers() 的零残留 Flush；
 * 3. 两种输出方式：VA-API 硬件帧优先导出为 DMA-BUF 并封装为 HardwareFrame，导出不可用时或这失败时
 * 仍使用 VA-API 完成解码，再把当前硬件帧下载到 CPU 内存最后通过 sws_getCachedContext() 转换为 YUV420P
 VideoFrame，下载回 CPU 不等于切换软件解码器
 * 4. 资源生命周期：HardwareFrame 同时持有 DMA-BUF fd 和 AVFrame 引用，
 * 避免 VA Surface,在下游仍使用时被 FFmpeg 解码器提前复用
 */

#include "video_decoder/ffmpeg_video_decoder.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <time.h>
#include <utility>

#include "mediacore/av_types/hardware_frame.h"
#include "mediacore/logger.h"
#include "utils/ffmpeg_error.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_par.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// VA-API 头文件： libva 自己的头文件已经带 extern "C" 保护，所以不用再包一层
// va/va.h -> VAStatus / VASurface ID / vaSyncSurface / vaErrorStr
// va/va_drmcommon.h -> VADRMPRIMESurfaceDescriptor / VA_EXPORT_SURFACE_*
// libavutil/hwcontext_vaapi.h -> AVVAAPIDeviceContext

#ifdef MEDIACORE_HAVE_VAAPI
#include <va/va.h>
#include <va/va_drmcommon.h>

extern "C" {
#include <libavutil/hwcontext_vaapi.h>
#include <unistd.h>
}

#endif

namespace mediacore {

namespace {

constexpr const char* kFindDecoderOperation = "avcodec_find_decoder(video)";
constexpr const char* kAllocateContextOperation = "avcodec_alloc_context3(video)";
constexpr const char* kCopyParametersOperation = "avcodec_parameters_to_context(video)";
constexpr const char* kOpenDecoderOperation = "avcodec_open2(video)";
constexpr const char* kAllocateFrameOperation = "av_frame_alloc()";
constexpr const char* kSendPacketOperation = "avcodec_send_packet(video)";
constexpr const char* kReceiveFrameOperation = "avcodec_receive_frame(video)";
constexpr const char* kValidateFrameSizeOperation = "av_image_get_buffer_size(yuv420p)";
constexpr const char* kConvertFrameOperation = "sws_scale(video->yuv420p)";
constexpr const char* kModuleName = "FfmpegVideoDecoder";

#ifdef MEDIACORE_HAVE_VAAPI
constexpr std::size_t kMaxVaapiExportObjects = kMaxHardwareDmaBufObjects;
#endif

// VA-API 使用的 DRM Render Node。
//
// Linux DRM 为 GPU 提供两类常见设备节点：
//
// 1. Primary Node（主设备节点）：
//    路径格式为 /dev/dri/cardN，例如 /dev/dri/card0
//    提供 GPU 渲染和显示设备管理等功能
//    涉及显示模式设置（Modesetting）的特权操作通常需要 DRM Master 权限
//    DRM Master 通常由桌面显示服务器或合成器持有，但这不代表其他进程不能打开 card node
//
// 2. Render Node（渲染设备节点）：
//    路径格式为 /dev/dri/renderDXXX，例如 /dev/dri/renderD128
//    专门供 GPU 渲染、计算和视频解码等客户端使用
//    不支持显示模式设置等特权操作，也不需要 DRM Master
//    访问权限主要由设备文件的读写权限控制。
//
// Render Node 的编号由 Linux DRM 子系统分配：
//    renderD128 通常是系统中的第一个 Render Node；
//    renderD129 通常是下一个 Render Node
//
// 当前默认使用 renderD128
// 后续可通过 PlayerConfig 支持用户指定 GPU 设备路径。
constexpr const char* kVaapiRenderNode = "/dev/dri/renderD128";

// av-hwdevice_ctx_create 的操作名，只用于错误文本
constexpr const char* kCreateHardwareDeviceOperation = "av_hwdevice_ctx_create)vaapi";
constexpr const char* kTransferFrameOperation = "av_hwframe_transfer_data(vaapi)";
constexpr const char* kCopyFramePropertiesOperation = "av_frame_copy_props(vaapi)";

/**
 * @brief 将 FFmpeg 操作失败信息与错误码封装为项目统一的 MediaError
 * @param operation_name 触发错误的 FFmpeg 函数或操作名称
 * @param ffmpeg_error_code FFmpeg 负数错误码
 * @return 归属于 kFfmpeg 类型的结构化错误对象
 */
MediaError CreateVideoDecoderError(const char* operation_name, int ffmpeg_error_code) {
    return ffmpeg_detail::MakeFfmpegMediaError(kModuleName, ffmpeg_error_code, operation_name);
}

std::uint64_t CurrentThreadCpuTimeNanoseconds() noexcept {
    timespec cpu_time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_time) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(cpu_time.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(cpu_time.tv_nsec);
}

const char* HardwareAccelName(HardwareAccel acceleration) noexcept {
    switch (acceleration) {
        case HardwareAccel::kOff:
            return "Software";
        case HardwareAccel::kAuto:
            return "Auto";
        case HardwareAccel::kVaapi:
            return "VAAPI";
    }
    return "Unknown";
}

/**
 * @brief 将项目自定义的 Rational 安全转换为 FFmpeg 的 AVRational
 * @param rational_value 项目自定义的时间基分数
 * @return 若分数合法且未超出 int 上限则返回 AVRational，否则返回 std::nullopt
 */
std::optional<AVRational> ConvertRationalToAvRational(const Rational& rational_value) noexcept {
    constexpr auto kIntMax = static_cast<std::int64_t>(std::numeric_limits<int>::max());

    if (!rational_value.IsValid() || rational_value.numerator_ > kIntMax || rational_value.denominator_ > kIntMax) {
        return std::nullopt;
    }

    return AVRational{static_cast<int>(rational_value.numerator_), static_cast<int>(rational_value.denominator_)};
}

/**
 * @brief 将基于流时间基的视频帧时间戳换算为微秒（us）
 * @param timestamp_ticks 原始时间戳刻度（AVFrame::best_effort_timestamp 或 pts）
 * @param timestamp_time_base 视频流原生时间基
 * @return 换算后的微秒时间戳；若时间戳非法或换算失败则返回 std::nullopt
 */
std::optional<std::int64_t> ConvertTimestampToMicroseconds(std::int64_t timestamp_ticks,
                                                           const Rational& timestamp_time_base) {
    if (timestamp_ticks == AV_NOPTS_VALUE) {
        return std::nullopt;
    }

    const std::optional<AVRational> source_av_time_base = ConvertRationalToAvRational(timestamp_time_base);
    if (!source_av_time_base.has_value()) {
        return std::nullopt;
    }

    const std::int64_t timestamp_us = av_rescale_q(timestamp_ticks, *source_av_time_base, AV_TIME_BASE_Q);
    return timestamp_us == AV_NOPTS_VALUE ? std::nullopt : std::optional<std::int64_t>(timestamp_us);
}

/**
 * @struct CodecContextCloser
 * @brief 自定义 RAII 删除器：释放 AVCodecContext
 */
struct CodecContextCloser final {
    void operator()(AVCodecContext* context) const noexcept {
        if (context != nullptr) {
            avcodec_free_context(&context);
        }
    }
};

/**
 * @struct AvCodecContextReleaser
 * @brief 自定义 RAII 删除器：释放 AVCodecContext
 */
struct AvCodecContextReleaser final {
    void operator()(AVCodecContext* codec_ctx) const noexcept {
        if (codec_ctx != nullptr) {
            avcodec_free_context(&codec_ctx);
        }
    }
};

/**
 * @struct AvFrameReleaser
 * @brief 自定义 RAII 删除器：释放复用 AVFrame
 */
struct AvFrameReleaser final {
    void operator()(AVFrame* decoded_frame) const noexcept {
        if (decoded_frame != nullptr) {
            av_frame_free(&decoded_frame);
        }
    }
};

/**
 * @struct AvBufferRefReleaser
 * @brief 自定义 RAII 删除器：释放复用 AvBufferRefReleaser
 */
struct AvBufferRefReleaser final {
    void operator()(AVBufferRef* av_buf_ref) noexcept {
        if (av_buf_ref != nullptr) {
            av_buffer_unref(&av_buf_ref);
        }
    }
};

using AvCodecContextOwner = std::unique_ptr<AVCodecContext, AvCodecContextReleaser>;
using AvFrameOwner = std::unique_ptr<AVFrame, AvFrameReleaser>;
using AvBufferRefOwner = std::unique_ptr<AVBufferRef, AvBufferRefReleaser>;

#ifdef MEDIACORE_HAVE_VAAPI
/**
 * @class ScopedVaapiDmaBufExport
 * @brief 一个专门负责清理 VA-API 导出 fd 的资源管理器
 * @details
 * vaExportSurfaceHandle() 导出 VA Surface 时，会将底层内存对象的信息和对应的 DMA-BUF fd 填入
 * VADRMPRIMESurfaceDescriptor
 * 其中
 * objects[] 记录底层内存对象，每个对象包含 fd、大小和内存布局信息
 * layers[] 记录了图像格式和各个 plane 对象的内存对象和位置
 * 本类只负责管理 objects[] 中的原始 fd：
 * 1. 创建时，将所有 fd 初始化为 -1，表示尚未获得有效 fd。
 * 2. VA-API 导出成功后，由本类接管这些 fd。
 * 3. 使用完成后，关闭所有仍由本类持有的 fd。
 * 4. 如果中途发生错误或提前 return，析构函数会自动关闭剩余 fd。
 */
class ScopedVaapiDmaBufExport final {
public:
    /**
     * @brief 初始化导出描述符中的文件描述符，将每个 Object 的 fd 设置为 -1
     */
    ScopedVaapiDmaBufExport() noexcept {
        for (auto& exported_object : export_descriptor_.objects) {
            exported_object.fd = -1;
        }
    }

    /**
     * @brief 析构时关闭所有尚未释放的原始 fd
     */
    ~ScopedVaapiDmaBufExport() { ReleaseExportedDmaBufFileDescriptors(); }

    // 禁止复制，避免多个对象同时管理同一组 fd，导致同一个文件描述符被重复关闭。
    ScopedVaapiDmaBufExport(const ScopedVaapiDmaBufExport&) = delete;
    ScopedVaapiDmaBufExport& operator=(const ScopedVaapiDmaBufExport&) = delete;

    /**
     * @brief 获取 VA-API 导出描述符
     * @return 内部导出描述符的引用
     * 将内部描述符交给 vaExportSurfaceHandle() 填充，后续可以通过它读取导出的fd、图像格式和内存布局信息
     */
    VADRMPRIMESurfaceDescriptor& GetExportDescriptor() noexcept { return export_descriptor_; }

    /**
     * @brief 关闭所有仍由本类管理的原始 DMA-BUF fd
     * @details
     * 遍历导出描述符中的所有 Object：先将相同的 fd 设置为 -1，再执行 close()，每个有效 fd 只关闭一次
     * @note kMaxVaapiExportObjects 必须与导出描述符 objects[] 数组的容量一致，避免访问数组范围之外的元素
     */
    void ReleaseExportedDmaBufFileDescriptors() noexcept {
        for (std::size_t object_index = 0; object_index < kMaxVaapiExportObjects; ++object_index) {
            const int exported_dma_buf_fd = export_descriptor_.objects[object_index].fd;

            if (exported_dma_buf_fd < 0) {
                continue;
            }

            // 将所有相同的 fd 标记为无效，确保每个 fd 只关闭一次。
            for (std::size_t remaining_index = object_index; remaining_index < kMaxVaapiExportObjects;
                 ++remaining_index) {
                if (export_descriptor_.objects[remaining_index].fd == exported_dma_buf_fd) {
                    export_descriptor_.objects[remaining_index].fd = -1;
                }
            }

            // 关闭当前由本类持有的原始文件描述符。
            ::close(exported_dma_buf_fd);
        }
    }

private:
    /**
     * @brief VA-API 导出的图像描述信息
     *
     * VA-API 成功导出后，会在这里填写：
     * - 图像格式和宽高；
     * - 底层内存对象数量及其 fd；
     * - 每个 Object 的大小和 Modifier；
     * - Layer 和 Plane 的组织及布局信息
     *
     * 本类负责其中 objects[].fd 的生命周期。
     */
    VADRMPRIMESurfaceDescriptor export_descriptor_{};
};

/**
 * @brief 自动管理一个 DMA-BUF 文件描述符，防止忘记关闭 fd
 * @details
 * 创建对象时接管传入的 fd,如果对象销毁时仍然持有 fd，就自动调用 close() 关闭它。
 *
 */
class ScopedDmaBufFileDescriptor final {
public:
    /**
     * @brief 接管一个 DMA-BUF 文件描述符
     * @param dma_buf_fd：需要管理的 fd
     */
    explicit ScopedDmaBufFileDescriptor(int dma_buf_fd) noexcept : dma_buf_fd_(dma_buf_fd) {}

    /**
     * @brief 析构时自动关闭仍由当前对象管理的 fd
     */
    ~ScopedDmaBufFileDescriptor() {
        if (dma_buf_fd_ >= 0) {
            ::close(dma_buf_fd_);
        }
    }

    // 禁止拷贝，避免两个对象同时管理同一个 fd，导致重复关闭。
    ScopedDmaBufFileDescriptor(const ScopedDmaBufFileDescriptor&) = delete;
    ScopedDmaBufFileDescriptor& operator=(const ScopedDmaBufFileDescriptor&) = delete;

    /**
     * @brief 获取当前管理的 fd
     * 只返回 fd，不转移所有权
     * 调用者可以使用这个 fd，但不能擅自关闭它
     */
    int GetDmaBufFileDescriptor() const noexcept { return dma_buf_fd_; }

    /**
     * @brief 取出 fd，并放弃当前对象对它的管理
     * @details
     * 返回原来的 fd，同时把内部 fd 设置为 -1，调用者接收返回值后，需要自己负责管理和关闭 fd
     */
    int ReleaseDmaBufFileDescriptor() noexcept { return std::exchange(dma_buf_fd_, -1); }

private:
    // 当前对象负责管理的 DMA-BUF 文件描述符
    int dma_buf_fd_{-1};
};
#endif

/**
 * @brief 把 FFmpeg 的色彩空间枚举映射为项目自己的枚举
 * @param av_color_space FFmpeg 的 AVColorSpace 值
 * @return 项目 VideoColorSpace
 * @details
 * FFmpeg:  AVCOL_SPC_BT709=1, AVCOL_SPC_UNSPECIFIED=2, AVCOL_SPC_BT470BG=5
 * 项目:    kBt601=1, kBt709=2, kBt2020=3, kUnspecified=0
 * 直接 cast 会把 FFmpeg 的 BT709(1) 映射成项目的 kBt601，渲染端就会用错色彩矩阵，画面明显偏色
 * 软件路径和硬件（零拷贝）路径共用本函数，保证两条路径得到一致的色彩元数据
 */
VideoColorSpace ConvertAvColorSpaceToVideoColorSpace(AVColorSpace av_color_space) noexcept {
    switch (av_color_space) {
        case AVCOL_SPC_BT709:
            return VideoColorSpace::kBt709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return VideoColorSpace::kBt601;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return VideoColorSpace::kBt2020;
        default:
            return VideoColorSpace::kUnspecified;
    }
}

}  // namespace

/**
 * @brief 私有构造函数：保存并独占管理已成功打开的 FFmpeg 资源
 */
FfmpegVideoDecoder::FfmpegVideoDecoder(
    AVCodecContext* codec_context,
    AVFrame* decoded_frame,
    Rational video_stream_time_base,
    Rational video_frame_rate,
    HardwareAccel requested_acceleration,
    std::string codec_name,
    AVBufferRef* hardware_device_context_ref,
    std::unique_ptr<HardwarePixelFormatSelection> hardware_pixel_format_selection) noexcept
    : hardware_pixel_format_selection_(std::move(hardware_pixel_format_selection)),
      hardware_device_context_ref_(hardware_device_context_ref),
      codec_context_(codec_context),
      decoded_frame_(decoded_frame),
      video_stream_time_base_(video_stream_time_base),
      video_frame_rate_(video_frame_rate),
      codec_name_(std::move(codec_name)),
      requested_acceleration_(requested_acceleration),
      hardware_device_initialized_(hardware_device_context_ref != nullptr),
      statistics_started_at_(std::chrono::steady_clock::now()),
      interval_started_at_(statistics_started_at_) {
    LOG_INFO("VideoDecoderOpen codec={} resolution={}x{} frame_rate={:.3f} requested_mode={} "
             "hardware_device_type={} hardware_device_initialized={} hardware_fallback={}",
             codec_name_,
             codec_context_ != nullptr ? codec_context_->width : 0,
             codec_context_ != nullptr ? codec_context_->height : 0,
             video_frame_rate_.ToDouble(),
             HardwareAccelName(requested_acceleration_),
             hardware_device_initialized_ ? "vaapi" : "none",
             hardware_device_initialized_,
             requested_acceleration_ == HardwareAccel::kAuto && !hardware_device_initialized_);
}

/**
 * @brief 析构函数：释放转码上下文、解码器上下文、硬件设备及持有的复用 AVFrame
 * @details
 * 硬件设备上下文有两份独立的 AVBufferRef 引用
 * 1. AVCodecContext::hw_device_ctx：由 avcodec_free_context() 负责释放
 * 2. hardware_device_context_ref_：由当前的 FfmpegVideoDecoder 持有，在这里单独释放
 * 两份引用指向同一个硬件设备资源。释放其中一份不会直接销毁设备；
 * 只有所有相关引用都释放后，底层 AVHWDeviceContext 才可能真正销毁
 */
FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    if (!summary_logged_) {
        LogStatistics("destroyed", true);
    }
    LOG_INFO("FfmpegVideoDecoder destroyed, codec {}", codec_name_);

    // 1. 释放 AVCodecContext；FFmpeg 会同时释放它持有的 hw_device_ctx 引用
    if (codec_context_ != nullptr) {
        avcodec_free_context(&codec_context_);
    }

    // 2. 释放当前解码器自己持有的设备引用
    if (hardware_device_context_ref_ != nullptr) {
        av_buffer_unref(&hardware_device_context_ref_);
    }

    // 3. 像素格式转换释放
    if (software_scaler_context_ != nullptr) {
        sws_free_context(&software_scaler_context_);
    }

    // 4. 释放 AVFrame
    if (decoded_frame_ != nullptr) {
        av_frame_free(&decoded_frame_);
    }
}

void FfmpegVideoDecoder::RecordSend(double elapsed_ms, std::uint64_t cpu_ns, bool accepted) noexcept {
    auto update = [elapsed_ms, cpu_ns, accepted](Statistics& stats) {
        ++stats.send_calls_;
        stats.send_api_ms_ += elapsed_ms;
        stats.decoder_thread_cpu_ns_ += cpu_ns;
        if (accepted) {
            ++stats.accepted_packets_;
        }
    };
    update(total_statistics_);
    update(interval_statistics_);
    if (accepted) {
        summary_logged_ = false;
    }
}

void FfmpegVideoDecoder::RecordDrainSend(double elapsed_ms, std::uint64_t cpu_ns) noexcept {
    auto update = [elapsed_ms, cpu_ns](Statistics& stats) {
        ++stats.drain_send_calls_;
        stats.drain_send_api_ms_ += elapsed_ms;
        stats.decoder_thread_cpu_ns_ += cpu_ns;
    };
    update(total_statistics_);
    update(interval_statistics_);
}

void FfmpegVideoDecoder::RecordReceive(double elapsed_ms, std::uint64_t cpu_ns, bool produced_frame) noexcept {
    auto update = [elapsed_ms, cpu_ns, produced_frame](Statistics& stats) {
        ++stats.receive_calls_;
        stats.receive_api_ms_ += elapsed_ms;
        stats.decoder_thread_cpu_ns_ += cpu_ns;
        if (produced_frame) {
            ++stats.codec_frames_;
        }
    };
    update(total_statistics_);
    update(interval_statistics_);
}

void FfmpegVideoDecoder::RecordFrameProcessing(double elapsed_ms) noexcept {
    auto update = [elapsed_ms](Statistics& stats) {
        ++stats.decoded_frames_;
        stats.frame_processing_ms_ += elapsed_ms;
        stats.max_frame_processing_ms_ = std::max(stats.max_frame_processing_ms_, elapsed_ms);
    };
    update(total_statistics_);
    update(interval_statistics_);
    summary_logged_ = false;
}

void FfmpegVideoDecoder::RecordOutputPath(AVPixelFormat decoded_pixel_format,
                                          const VideoFrame& output_frame) noexcept {
    auto update = [decoded_pixel_format, &output_frame](Statistics& stats) {
        if (decoded_pixel_format == AV_PIX_FMT_VAAPI) {
            if (output_frame.hardware_ != nullptr) {
                ++stats.vaapi_export_frames_;
            } else {
                ++stats.vaapi_download_frames_;
            }
        } else {
            ++stats.software_frames_;
        }
    };
    update(total_statistics_);
    update(interval_statistics_);
}

void FfmpegVideoDecoder::RecordVaSync(double elapsed_ms) noexcept {
    auto update = [elapsed_ms](Statistics& stats) {
        ++stats.va_sync_calls_;
        stats.va_sync_ms_ += elapsed_ms;
        stats.max_va_sync_ms_ = std::max(stats.max_va_sync_ms_, elapsed_ms);
    };
    update(total_statistics_);
    update(interval_statistics_);
}

void FfmpegVideoDecoder::RecordHardwareTransfer(double elapsed_ms) noexcept {
    auto update = [elapsed_ms](Statistics& stats) {
        ++stats.hw_transfer_calls_;
        stats.hw_transfer_ms_ += elapsed_ms;
        stats.max_hw_transfer_ms_ = std::max(stats.max_hw_transfer_ms_, elapsed_ms);
    };
    update(total_statistics_);
    update(interval_statistics_);
}

void FfmpegVideoDecoder::RecordError() noexcept {
    ++total_statistics_.errors_;
    ++interval_statistics_.errors_;
}

void FfmpegVideoDecoder::ResetIntervalStatistics() noexcept {
    interval_statistics_ = Statistics{};
    interval_started_at_ = std::chrono::steady_clock::now();
}

void FfmpegVideoDecoder::LogFirstFrameInfo(const VideoFrame& output_frame,
                                           AVPixelFormat decoded_pixel_format,
                                           int decoded_width,
                                           int decoded_height) {
    if (first_frame_logged_) {
        return;
    }

    first_frame_logged_ = true;
    decoded_frame_width_ = decoded_width;
    decoded_frame_height_ = decoded_height;
    const char* format_name = av_get_pix_fmt_name(decoded_pixel_format);
    input_pixel_format_ = format_name != nullptr ? format_name : "unknown";
    actual_mode_ = decoded_pixel_format == AV_PIX_FMT_VAAPI ? "VAAPI" : "Software";
    const char* output_path = nullptr;
    if (output_frame.hardware_ != nullptr) {
        output_pixel_format_ = GetDrmFourccName(output_frame.hardware_->layer_format_);
        output_path = "VAAPI_DMA_BUF";
    } else {
        output_pixel_format_ = "YUV420P";
        output_path = decoded_pixel_format == AV_PIX_FMT_VAAPI ? "VAAPI_CPU_DOWNLOAD" : "Software_CPU";
    }

    const bool hardware_fallback = requested_acceleration_ == HardwareAccel::kAuto && actual_mode_ != "VAAPI";
    LOG_INFO("VideoDecoderFirstFrame mode={} output_path={} codec={} resolution={}x{} frame_rate={:.3f} "
             "input_pixel_format={} output_pixel_format={} hardware_device_type={} "
             "hardware_device_initialized={} hardware_fallback={}",
             actual_mode_,
             output_path,
             codec_name_,
             decoded_width,
             decoded_height,
             video_frame_rate_.ToDouble(),
             input_pixel_format_,
             output_pixel_format_,
             hardware_device_initialized_ ? "vaapi" : "none",
             hardware_device_initialized_,
             hardware_fallback);
}

void FfmpegVideoDecoder::LogStatistics(const char* reason, bool summary) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = summary ? now - statistics_started_at_ : now - interval_started_at_;
    const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
    if (!summary && elapsed_seconds < 5.0) {
        return;
    }

    const Statistics& stats = summary ? total_statistics_ : interval_statistics_;
    const double wall_fps = elapsed_seconds > 0.0
        ? static_cast<double>(stats.decoded_frames_) / elapsed_seconds
        : 0.0;
    const double elapsed_ms = elapsed_seconds * 1000.0;
    const double cpu_pct = elapsed_ms > 0.0
        ? (static_cast<double>(stats.decoder_thread_cpu_ns_) / 1000000.0) / elapsed_ms * 100.0
        : 0.0;
    const auto average = [](double total, std::uint64_t count) { return count == 0 ? 0.0 : total / count; };
    const bool hardware_fallback = requested_acceleration_ == HardwareAccel::kAuto && actual_mode_ == "Software";

    LOG_INFO("VideoDecoderStats reason={} scope={} rate_basis=wall_clock mode={} requested_mode={} codec={} resolution={}x{} "
             "frame_rate={:.3f} input_pixel_format={} output_pixel_format={} hardware_device_type={} "
             "hardware_device_initialized={} hardware_fallback={} interval_s={:.3f} input_packets={} "
             "codec_frames={} decoded_frames={} decoder_output_wall_fps={:.3f} cumulative_frames={} "
             "software_frames={} vaapi_cpu_download_frames={} vaapi_dma_buf_frames={} "
             "avg_send_api_ms={:.3f} avg_drain_send_api_ms={:.3f} avg_receive_api_ms={:.3f} "
             "avg_total_frame_wall_ms={:.3f} max_total_frame_wall_ms={:.3f} "
             "va_sync_scope=explicit_dma_export_only avg_va_sync_ms={:.3f} max_va_sync_ms={:.3f} "
             "avg_hw_transfer_wall_ms={:.3f} max_hw_transfer_wall_ms={:.3f} "
             "decoder_timed_thread_cpu_pct={:.2f} errors={}",
             reason,
             summary ? "total" : "interval",
             actual_mode_,
             HardwareAccelName(requested_acceleration_),
             codec_name_,
             decoded_frame_width_ > 0 ? decoded_frame_width_ : GetCodedWidth(),
             decoded_frame_height_ > 0 ? decoded_frame_height_ : GetCodedHeight(),
             video_frame_rate_.ToDouble(),
             input_pixel_format_,
             output_pixel_format_,
             hardware_device_initialized_ ? "vaapi" : "none",
             hardware_device_initialized_,
             hardware_fallback,
             elapsed_seconds,
             stats.accepted_packets_,
             stats.codec_frames_,
             stats.decoded_frames_,
             wall_fps,
             total_statistics_.decoded_frames_,
             stats.software_frames_,
             stats.vaapi_download_frames_,
             stats.vaapi_export_frames_,
             average(stats.send_api_ms_, stats.send_calls_),
             average(stats.drain_send_api_ms_, stats.drain_send_calls_),
             average(stats.receive_api_ms_, stats.receive_calls_),
             average(stats.frame_processing_ms_, stats.decoded_frames_),
             stats.max_frame_processing_ms_,
             average(stats.va_sync_ms_, stats.va_sync_calls_),
             stats.max_va_sync_ms_,
             average(stats.hw_transfer_ms_, stats.hw_transfer_calls_),
             stats.max_hw_transfer_ms_,
             cpu_pct,
             stats.errors_);

    if (summary) {
        summary_logged_ = true;
    } else {
        ResetIntervalStatistics();
    }
}

void FfmpegVideoDecoder::MaybeLogIntervalStatistics() {
    LogStatistics("interval", false);
}

/**
 * @brief 静态工厂：根据流参数创建并初始化视频解码器
 * @param parameters 解封装获取的编解码器参数（所有权转移）
 * @param video_stream_time_base 视频流原生时间基
 * @param hardware_acceleration 硬件加速策略
 * @return 解码器打开结果
 * @details
 * 初始化阶段有两条独立路径，每条都使用单独分配的 AVCodecContext：
 *
 * 1. 硬解：创建 VA-API 设备 -> 创建解码上下文 -> 设置 hw_device_ctx_ opaque 和 get_format
 * -> 调用 avcodec_open2()
 *
 * 2. 软解：重新创建解码上下文 -> 只设置通用编码参数 -> 调用 av_codec_open2()
 */
FfmpegVideoDecoderOpenResult FfmpegVideoDecoder::Open(VideoCodecParameters parameters,
                                                      Rational video_stream_time_base,
                                                      HardwareAccel hardware_acceleration,
                                                      Rational video_frame_rate) {
    FfmpegVideoDecoderOpenResult result;

    // 1. 获取视频编码参数
    const AVCodecParameters* codec_parameters = parameters.Get();

    if (codec_parameters == nullptr || codec_parameters->codec_type != AVMEDIA_TYPE_VIDEO) {
        LOG_ERROR("FfmpegVideoDecoder::Open requires valid video codec parameters");
        result.error_ = CreateVideoDecoderError(kFindDecoderOperation, AVERROR(EINVAL));
        return result;
    }

    // 2. 获取视频帧时间基
    const std::optional<AVRational> packet_time_base = ConvertRationalToAvRational(video_stream_time_base);
    if (!packet_time_base.has_value()) {
        LOG_ERROR("FfmpegVideoDecoder::Open received an invalid or out-of-range stream time base {}/{}",
                  video_stream_time_base.numerator_,
                  video_stream_time_base.denominator_);
        result.error_ = CreateVideoDecoderError(kCopyParametersOperation, AVERROR(EINVAL));
        return result;
    }

    LOG_INFO("Opening video decoder for codec id {}, time base {}/{}",
             static_cast<int>(codec_parameters->codec_id),
             video_stream_time_base.numerator_,
             video_stream_time_base.denominator_);

    // 3. 查找适用的解码器实现
    const AVCodec* codec = avcodec_find_decoder(codec_parameters->codec_id);
    if (codec == nullptr) {
        LOG_ERROR("avcodec_find_decoder returned nullptr for codec_id {}",
                  static_cast<int>(codec_parameters->codec_id));
        result.error_ = CreateVideoDecoderError(kFindDecoderOperation, AVERROR_DECODER_NOT_FOUND);
        return result;
    }

    const std::string codec_name = codec->name != nullptr ? codec->name : "unknown";

    // 4. 硬件像素格式
    std::unique_ptr<HardwarePixelFormatSelection> hw_pixel_fmt_select;

    // 5. 初始化 CodecContext，configure_codec_parameters lambda 表达式
    auto configure_codec_parameters = [&](AVCodecContext* codec_ctx) -> int {
        // 把 AVCodecParameters 中的编码参数复制到 AVCodecContext 中
        const int copy_parameters_result = avcodec_parameters_to_context(codec_ctx, codec_parameters);

        if (copy_parameters_result < 0) {
            return copy_parameters_result;
            ;
        }

        codec_ctx->pkt_timebase = *packet_time_base;
        return 0;
    };

    // 6. 创建视频解码器
    auto create_video_decoder = [&](AvCodecContextOwner& codec_ctx_owner,
                                    AvBufferRefOwner* hw_device_ref_owner) -> bool {
        // 分配一个 AVFrame，后续用于接收解码器输出的视频帧
        AvFrameOwner output_frame_owner(av_frame_alloc());

        if (!output_frame_owner) {
            LOG_ERROR("av_frame_alloc failed while opening decoder '{}'", codec_name);
            result.error_ = CreateVideoDecoderError(kAllocateFrameOperation, AVERROR(ENOMEM));
            return false;
        }

        try {
            result.video_decoder_.reset(
                new FfmpegVideoDecoder(codec_ctx_owner.get(),
                                       output_frame_owner.get(),
                                       video_stream_time_base,
                                       video_frame_rate,
                                       hardware_acceleration,
                                       codec_name,
                                       hw_device_ref_owner != nullptr ? hw_device_ref_owner->get() : nullptr,
                                       std::move(hw_pixel_fmt_select)));
        } catch (const std::bad_alloc&) {
            LOG_ERROR("Failed to create FfmpegVideoDecoder for '{}' due to insufficient memory", codec_name);
            result.error_ = CreateVideoDecoderError(kAllocateContextOperation, AVERROR(ENOMEM));
            return false;
        }

        // 移交所有权
        (void)codec_ctx_owner.release();
        (void)output_frame_owner.release();

        if (hw_device_ref_owner != nullptr) {
            (void)hw_device_ref_owner->release();
        }

        return true;
    };

    // 7. ==================== 路径 1：尝试配置 VA-API 解码 ====================
    // ① 创建 VA-API 设备 ② 分配上下文  ③ 拷贝流参数  ④ 创建格式协商状态
    // ⑤ 上下文持有设备引用  ⑥ 注册 get_format  ⑦ avcodec_open2  ⑧ 构造解码器并移交所有权

    enum class HardwareOpenResult : std::uint8_t {
        kOpened,  // 解码器已构造完成，result 可直接返回
        kFallbackToSoftware,  // 仅 kAuto：硬解失败，转入软解
        kFatalError,  // 失败且不可回退，result.error_ 已填好
    };

    // 统一的失败出口：kVaapi 记录错误并终止，kAuto 记录日志后回退软解
    auto handle_hardware_failure = [&](const char* operation_name, int error_code) -> HardwareOpenResult {
        // is_forced 是否强制使用硬解码
        const bool is_forced = hardware_acceleration == HardwareAccel::kVaapi;

        LOG_WARN("VA-API step failed for '{}' : {}, {}",
                 codec_name,
                 ffmpeg_detail::DescribeFfmpegError(operation_name, error_code),
                 is_forced ? "aborting forced hardware decode" : "falling back to software decoding");

        if (is_forced) {
            result.error_ = CreateVideoDecoderError(operation_name, error_code);

            return HardwareOpenResult::kFatalError;
        }

        return HardwareOpenResult::kFallbackToSoftware;
    };

    auto try_open_vaapi_decoder = [&]() -> HardwareOpenResult {
        // ① 创建 VA-API 设备：render node 不需要 DRM master
        AVBufferRef* raw_device_ref = nullptr;

        const int device_result =
            av_hwdevice_ctx_create(&raw_device_ref, AV_HWDEVICE_TYPE_VAAPI, kVaapiRenderNode, nullptr, 0);

        AvBufferRefOwner device_ref_owner(raw_device_ref);

        if (device_result < 0 || device_ref_owner == nullptr) {
            return handle_hardware_failure(kCreateHardwareDeviceOperation,
                                           device_result < 0 ? device_result : AVERROR(ENOMEM));
        }

        LOG_INFO("VA-API hardware device created on {}", kVaapiRenderNode);

        // ② 硬解专用上下文：失败后随本函数返回自动释放，软解会另建新的
        AvCodecContextOwner codec_context_owner(avcodec_alloc_context3(codec));

        if (codec_context_owner == nullptr) {
            return handle_hardware_failure(kAllocateContextOperation, AVERROR(ENOMEM));
        }

        // ③ 拷贝流参数、设置 pkt_timebase（与软解共用 ConfigureCodecParameters）
        const int parameters_result = configure_codec_parameters(codec_context_owner.get());

        if (parameters_result < 0) {
            return handle_hardware_failure(kCopyParametersOperation, parameters_result);
        }

        // ④ 格式协商状态：get_format 回调通过 opaque 读它,对象由外层 unique_ptr 持有
        try {
            hw_pixel_fmt_select = std::make_unique<HardwarePixelFormatSelection>();
        } catch (std::bad_alloc&) {
            return handle_hardware_failure(kOpenDecoderOperation, AVERROR(ENOMEM));
        }

        // 设置硬件加速策略
        hw_pixel_fmt_select->acceleration_mode_ = hardware_acceleration;

        // ⑤ 上下文自己持有一份设备引用（av_buffer_ref 只增加引用计数）：此后设备有两份引用：device_ref_owner 与
        // ctx->hw_device_ctx
        codec_context_owner->hw_device_ctx = av_buffer_ref(device_ref_owner.get());

        if (codec_context_owner->hw_device_ctx == nullptr) {
            return handle_hardware_failure(kOpenDecoderOperation, AVERROR(ENOMEM));
        }

        // ⑥ 注册格式协商：opaque 只存地址，不管理生命周期
        // 把 HardwarePixelFormatSelection 对象的地址保存到 AVCodecContext::opaque
        codec_context_owner->opaque = hw_pixel_fmt_select.get();
        // 告诉 FFmpeg 使用哪个像素格式选择函数
        codec_context_owner->get_format = &FfmpegVideoDecoder::SelectDecoderPixelFormat;

        // ⑦ 打开解码器：成功只代表配置完成，最终格式要等第一帧触发 get_format 才知道
        const int open_result = avcodec_open2(codec_context_owner.get(), codec, nullptr);

        if (open_result < 0) {
            return handle_hardware_failure(kOpenDecoderOperation, open_result);
        }

        // 日志必须放在 if 之外：原来写在 return 之后，永远执行不到
        LOG_INFO("Video decoder '{}' opened with VA-API configured; hardware format selection is pending",
                 codec_name);

        // ⑧ 构造 FfmpegVideoDecoder 并移交上下文、AVFrame、设备引用的所有权
        return create_video_decoder(codec_context_owner, &device_ref_owner) ? HardwareOpenResult::kOpened
                                                                            : HardwareOpenResult::kFatalError;
    };

    // kOff 不碰硬件；kAuto / kVaapi 都先尝试 VA-API
    if (hardware_acceleration != HardwareAccel::kOff) {
        if (try_open_vaapi_decoder() != HardwareOpenResult::kFallbackToSoftware) {
            // kOpened：成功；kFatalError：失败
            return result;
        }

        // kAuto 回退：硬解上下文已随 lambda 销毁，此时再释放协商状态，opaque 不会悬空
        hw_pixel_fmt_select.reset();
    }

    // ==================== 路径 2：独立的软件解码上下文 ====================
    // 到达此处的情况：kOff；或 kAuto 且 VA-API 设备创建/配置/打开阶段失败,使用新建的
    // codec_context，不复用上面失败的硬件上下文

    AvCodecContextOwner software_codec_context_owner(avcodec_alloc_context3(codec));

    if (!software_codec_context_owner) {
        LOG_ERROR("avcodec_alloc_context3 failed for decoder '{}'", codec_name);
        result.error_ = CreateVideoDecoderError(kAllocateContextOperation, AVERROR(ENOMEM));
        return result;
    }

    const int configure_parameters_result = configure_codec_parameters(software_codec_context_owner.get());

    if (configure_parameters_result < 0) {
        LOG_ERROR("avcodec_parameters_to_context failed for decoder '{}': {}",
                  codec_name,
                  ffmpeg_detail::DescribeFfmpegError(kCopyParametersOperation, configure_parameters_result));
        result.error_ = CreateVideoDecoderError(kCopyParametersOperation, configure_parameters_result);
        return result;
    }

    const int open_decoder_result = avcodec_open2(software_codec_context_owner.get(), codec, nullptr);
    if (open_decoder_result < 0) {
        LOG_ERROR("avcodec_open2 failed for decoder '{}': {}",
                  codec_name,
                  ffmpeg_detail::DescribeFfmpegError(kOpenDecoderOperation, open_decoder_result));
        result.error_ = CreateVideoDecoderError(kOpenDecoderOperation, open_decoder_result);
        return result;
    }

    LOG_INFO("Video decoder '{}' opened with software decoding", codec_name);

    // 软解路径没有设备，也没有格式协商状态
    (void)create_video_decoder(software_codec_context_owner, nullptr);
    return result;
}

/**
 * @brief FFmpeg 请求项目选择解码输出像素格式时调用
 * @param codec_context 解码器上下文
 * @param candidate_pixel_formats 候选像素格式
 * @return AVPixelFormat 像素格式
 * @details
 * 根据硬件加速策略处理候选格式：
 * 1. 候选列表包含 AV_PIX_FMT_VAAPI：选择 VA-API，并记录选择结果
 * 2. 没有 VA-API 且策略为 kVaapi：返回 AV_PIX_FMT_NONE，明确拒绝继续
 * 3. 没有 VA-API 且策略为 kAuto：优先选择 YUV420P 或 YUVJ420P
 * 4. 上述格式不可用时，再选择 libswscale 支持的软件输入格式
 * 5. 没有可处理的格式时，返回 AV_PIX_FMT_NONE
 */
enum AVPixelFormat FfmpegVideoDecoder::SelectDecoderPixelFormat(AVCodecContext* codec_context,
                                                                const enum AVPixelFormat* candidate_pixel_formats) {
    auto* pixel_format_selection = static_cast<HardwarePixelFormatSelection*>(codec_context->opaque);

    if (pixel_format_selection == nullptr) {
        // 内部错误：opaque 没设好。返回第一个候选总比返回 NONE 崩得晚一点，
        // 而且后续的下游转换有机会报出更具体的原因
        LOG_ERROR("get_format invoked without opaque state; using the first candidate format");
        return candidate_pixel_formats[0];
    }

    // ---- 1. 候选里有 VAAPI：选中 ----
    for (const enum AVPixelFormat* candidate_pixel_format = candidate_pixel_formats;
         *candidate_pixel_format != AV_PIX_FMT_NONE;
         ++candidate_pixel_format) {
        if (*candidate_pixel_format == AV_PIX_FMT_VAAPI) {
            pixel_format_selection->is_vappi_selected_ = true;
            LOG_DEBUG("Decoder format negotiation selected VAAPI");

            return AV_PIX_FMT_VAAPI;
        }
    }

    // ---- 2. 没有 VAAPI 且强制硬解：明确失败 ----
    if (pixel_format_selection->acceleration_mode_ == HardwareAccel::kVaapi) {
        LOG_ERROR(
            "VA-API is not among the decoder's candidate pixel formats; forced hardware decoding cannot continue");

        return AV_PIX_FMT_NONE;
    }

    // ---- 3. kAuto：优先选现有链路验证过的软件格式 ----
    for (const enum AVPixelFormat* candidate_pixel_format = candidate_pixel_formats;
         *candidate_pixel_format != AV_PIX_FMT_NONE;
         ++candidate_pixel_format) {
        if (*candidate_pixel_format == AV_PIX_FMT_YUV420P || *candidate_pixel_format == AV_PIX_FMT_YUVJ420P) {
            LOG_WARN("VA-API unavailable for this stream, using software format {}",
                     av_get_pix_fmt_name(*candidate_pixel_format));
            return *candidate_pixel_format;
        }
    }

    // ---- 4. 兜底：选一个 sws_scale 能接受的格式 ----
    for (const enum AVPixelFormat* candidate_pixel_format = candidate_pixel_formats;
         *candidate_pixel_format != AV_PIX_FMT_NONE;
         ++candidate_pixel_format) {
        if (sws_isSupportedInput(*candidate_pixel_format) > 0) {
            LOG_WARN("Neither VA-API nor YUV420P is available, using {}", av_get_pix_fmt_name(*candidate_pixel_format));
            return *candidate_pixel_format;
        }
    }

    // ---- 5. 一个都处理不了 ----
    LOG_ERROR("Decoder offered no pixel format usable by this pipeline");
    return AV_PIX_FMT_NONE;
}

/**
 * @brief 向底层解码器提交压缩数据包
 */
FfmpegDecoderSendResult FfmpegVideoDecoder::SendPacket(const Packet& packet) {
    if (codec_context_ == nullptr || decoded_frame_ == nullptr) {
        LOG_ERROR(
            "Video Decoder '{}' cannot send packet to AVCodecContext because its internal FFmpeg objects are "
            "unavailable",
            codec_name_);
        RecordError();
        return {FfmpegDecoderSendStatus::kError, CreateVideoDecoderError(kSendPacketOperation, AVERROR(EINVAL))};
    }

    if (!packet.IsValid() || packet.GetAVPacket()->data == nullptr || packet.GetAVPacket()->size <= 0) {
        LOG_ERROR("Video Decoder '{}' received an invalid packet for epoch {}", codec_name_, packet.GetEpoch());
        RecordError();
        return {FfmpegDecoderSendStatus::kError, CreateVideoDecoderError(kSendPacketOperation, AVERROR(EINVAL))};
    }

    if (drain_started_ || drained_) {
        return {FfmpegDecoderSendStatus::kDrained, {}};
    }

    const auto send_started_at = std::chrono::steady_clock::now();
    const std::uint64_t send_cpu_started_ns = CurrentThreadCpuTimeNanoseconds();
    const int send_result = avcodec_send_packet(codec_context_, packet.GetAVPacket());
    const auto send_elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - send_started_at).count();
    const std::uint64_t send_cpu_finished_ns = CurrentThreadCpuTimeNanoseconds();
    RecordSend(send_elapsed_ms,
               send_cpu_finished_ns >= send_cpu_started_ns ? send_cpu_finished_ns - send_cpu_started_ns : 0,
               send_result == 0);

    if (send_result == 0) {
        return {FfmpegDecoderSendStatus::kAccepted, {}};
    }

    // EAGAIN：缓冲区已满，需先调用 ReceiveFrame() 腾出空间再重试
    if (send_result == AVERROR(EAGAIN)) {
        return {FfmpegDecoderSendStatus::kNeedReceive, {}};
    }

    // AVERROR_EOF：解码器已被排空，Flush 前不能再接收新包
    if (send_result == AVERROR_EOF) {
        drain_started_ = true;
        return {FfmpegDecoderSendStatus::kDrained, {}};
    }

    LOG_ERROR("avcodec_send_packet failed for decoder '{}', packet epoch {}, code {}",
              codec_name_,
              packet.GetEpoch(),
              send_result);
    RecordError();

    return {FfmpegDecoderSendStatus::kError, CreateVideoDecoderError(kSendPacketOperation, send_result)};
}

/**
 * @brief 发送 nullptr packet，令解码器进入 Drain 排空模式
 */
FfmpegDecoderSendResult FfmpegVideoDecoder::BeginDrain() {
    if (codec_context_ == nullptr || decoded_frame_ == nullptr) {
        LOG_ERROR("Video Decoder '{}' cannot begin drain because its internal FFmpeg objects are unavailable",
                  codec_name_);
        RecordError();
        return {FfmpegDecoderSendStatus::kError, CreateVideoDecoderError(kSendPacketOperation, AVERROR(EINVAL))};
    }

    if (drained_ || drain_started_) {
        return {FfmpegDecoderSendStatus::kAccepted, {}};
    }

    // 传入 nullptr packet 发送 EOF 信号，排空内部暂存的延迟帧（如 B 帧重排队列）
    const auto send_started_at = std::chrono::steady_clock::now();
    const std::uint64_t send_cpu_started_ns = CurrentThreadCpuTimeNanoseconds();
    const int send_result = avcodec_send_packet(codec_context_, nullptr);
    const double send_elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - send_started_at).count();
    const std::uint64_t send_cpu_finished_ns = CurrentThreadCpuTimeNanoseconds();
    RecordDrainSend(send_elapsed_ms,
                    send_cpu_finished_ns >= send_cpu_started_ns ? send_cpu_finished_ns - send_cpu_started_ns : 0);

    if (send_result == 0) {
        drain_started_ = true;
        LOG_DEBUG("Video Decoder '{}' started drain", codec_name_);
        return {FfmpegDecoderSendStatus::kAccepted, {}};
    }

    if (send_result == AVERROR(EAGAIN)) {
        return {FfmpegDecoderSendStatus::kNeedReceive, {}};
    }

    if (send_result == AVERROR_EOF) {
        drain_started_ = true;
        drained_ = true;
        return {FfmpegDecoderSendStatus::kDrained, {}};
    }

    LOG_ERROR("avcodec_send_packet(nullptr) failed while draining decoder '{}', code {}", codec_name_, send_result);
    RecordError();
    return {FfmpegDecoderSendStatus::kError, CreateVideoDecoderError(kSendPacketOperation, send_result)};
}

/**
 * @brief 尝试把当前 VA-API 解码帧导出为 HardwareFrame
 * @param epoch 当前播放世代
 * @details
 * 这是整个零拷贝链路的第一环，也是唯一在**解码线程**执行的环节
 * 它不复制或导出一份新的像素数据，而是取得当前 VA Surface 对应的 DMA-BUF fd
 * 并保存图像布局信息和 AVFrame 引用，供 Renderer 后续导入和使用
 * 后续 EGLImage 导入和 OpenGL Texture 创建由 Renderer 负责
 */
std::unique_ptr<HardwareFrame> FfmpegVideoDecoder::ExportHardwareFrame(std::uint64_t epoch) {
    try {
#ifdef MEDIACORE_HAVE_VAAPI

        // ---------- 第 1 步：从 AVFrame 里取出 VADisplay 与 VASurfaceID ----------
        // VADisplay：VA-API 的「显示连接」，相当于驱动的操作句柄
        //                   —— 在 hw_frames_ctx->device_ctx->hwctx 里
        // VASurfaceID：这块 GPU 内存的编号
        //                   —— 在 decoded_frame_->data[3] 里（注意：这是 ID，不是指针）

        if (decoded_frame_ == nullptr || decoded_frame_->format != AV_PIX_FMT_VAAPI || decoded_frame_->width <= 0 ||
            decoded_frame_->height <= 0 || decoded_frame_->buf[0] == nullptr ||
            decoded_frame_->hw_frames_ctx == nullptr || decoded_frame_->hw_frames_ctx->data == nullptr) {
            return nullptr;
        }

        // 从当前 AVFrame 中找到它对应的硬件帧上下文，并把它转换成 AVHWFramesContext* 类型，方便后续访问
        auto* hardware_frames_context = reinterpret_cast<AVHWFramesContext*>(decoded_frame_->hw_frames_ctx->data);

        if (hardware_frames_context == nullptr || hardware_frames_context->device_ctx == nullptr ||
            hardware_frames_context->device_ctx->type != AV_HWDEVICE_TYPE_VAAPI) {
            return nullptr;
        }

        auto* vaapi_device_context = static_cast<AVVAAPIDeviceContext*>(hardware_frames_context->device_ctx->hwctx);
        if (vaapi_device_context == nullptr || vaapi_device_context->display == nullptr) {
            return nullptr;
        }

        // data[3] 存放的是 VASurfaceID。FFmpeg 把它存成指针宽度的整数，
        const std::uintptr_t surface_handle = reinterpret_cast<std::uintptr_t>(decoded_frame_->data[3]);

        if (surface_handle > std::numeric_limits<VASurfaceID>::max() || surface_handle == VA_INVALID_SURFACE) {
            return nullptr;
        }

        const VASurfaceID surface_id = static_cast<VASurfaceID>(surface_handle);

        // ---------- 第 2 步：vaSyncSurface，等 GPU 真正写完 ----------

        // VA-API 解码是异步提交的：avcodec_receive_frame() 返回只代表「这一帧已被调度到 GPU」，不代表 GPU
        // 已经写完像素不等待就导出，后续在 GPU 上采样时会读到写了一半的内容

        const auto sync_started_at = std::chrono::steady_clock::now();
        const VAStatus sync_status = vaSyncSurface(vaapi_device_context->display, surface_id);
        RecordVaSync(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - sync_started_at)
                         .count());

        if (sync_status != VA_STATUS_SUCCESS) {
            LOG_WARN(
                "vaSyncSurface failed on decoder '{}' at epoch {}: {}", codec_name_, epoch, vaErrorStr(sync_status));
            return nullptr;
        }

        // ---------- 第 3 步：导出 DMA-BUF 描述符 ----------
        // VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 表示用「二代」导出接口,它会带上 modifier（内存排布方式）

        // DMA-BUF fd 资源管理器
        ScopedVaapiDmaBufExport exported_dma_buf_owner;

        // 获取导出的图像信息
        auto& export_descriptor = exported_dma_buf_owner.GetExportDescriptor();

        const VAStatus export_status =
            vaExportSurfaceHandle(vaapi_device_context->display,
                                  surface_id,
                                  VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                  &export_descriptor);

        if (export_status != VA_STATUS_SUCCESS) {
            LOG_WARN("vaExportSurfaceHandle failed on decoder '{}' at epoch {}: {}",
                     codec_name_,
                     epoch,
                     vaErrorStr(export_status));
            return nullptr;
        }

        // ---------- 第 4 步：把 VA-API 描述符转换成项目自己的 HardwareFrame ----------
        // HardwareFrame 保存访问原有图像内存所需的 fd 和布局信息，不保存一份新的 CPU 像素数据。
        // 必须先检查数组数量再访问 layer/object，不能相信驱动返回的数量总是正确。
        if (export_descriptor.width == 0 || export_descriptor.height == 0 ||
            export_descriptor.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            export_descriptor.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            export_descriptor.num_objects == 0 || export_descriptor.num_objects > kMaxVaapiExportObjects ||
            export_descriptor.num_layers != 1 || export_descriptor.layers[0].num_planes != 2 ||
            GetDrmPlaneFourcc(export_descriptor.layers[0].drm_format, 0) == 0 ||
            export_descriptor.width < static_cast<std::uint32_t>(decoded_frame_->width) ||
            export_descriptor.height < static_cast<std::uint32_t>(decoded_frame_->height)) {
            LOG_WARN("Unsupported VA-API DMA-BUF layout on decoder '{}' at epoch {}", codec_name_, epoch);
            return nullptr;
        }

        // Surface 的 VA FourCC、合并 layer 的 DRM FourCC 和 FFmpeg 软件格式必须一致。
        // NV12 是 8 bit；P010 把 10 bit 有效值放在 16 bit 分量里，不能混用解释。
        const auto& exported_layer = export_descriptor.layers[0];
        const bool is_nv12_export = export_descriptor.fourcc == VA_FOURCC_NV12 &&
            exported_layer.drm_format == kDrmFormatNv12 && hardware_frames_context->sw_format == AV_PIX_FMT_NV12;
        const bool is_p010_export = export_descriptor.fourcc == VA_FOURCC_P010 &&
            exported_layer.drm_format == kDrmFormatP010 && hardware_frames_context->sw_format == AV_PIX_FMT_P010LE;
        if (!is_nv12_export && !is_p010_export) {
            LOG_WARN(
                "Inconsistent or unsupported VA/DRM pixel formats on decoder '{}' at epoch {}", codec_name_, epoch);
            return nullptr;
        }

        auto hardware_frame = std::make_unique<HardwareFrame>();
        hardware_frame->width_ = static_cast<int>(export_descriptor.width);
        hardware_frame->height_ = static_cast<int>(export_descriptor.height);
        hardware_frame->layer_format_ = export_descriptor.layers[0].drm_format;

        // 4a. 每个 object：dup 一份 fd，让 HardwareFrame 独立持有。
        //     为什么要 dup：导出 fd 由本次调用者拥有，HardwareFrame 却需要跨线程、
        //     跨队列长期存活；副本引用同一块内存，但可以独立关闭。
        //     先 reserve；即使日后代码触发扩容异常，局部 RAII 仍负责关闭尚未移交的 fd。
        hardware_frame->dma_buf_objects_.reserve(export_descriptor.num_objects);
        for (std::uint32_t object_index = 0; object_index < export_descriptor.num_objects; ++object_index) {
            const auto& exported_object = export_descriptor.objects[object_index];
            if (exported_object.fd < 0 || exported_object.size == 0) {
                LOG_WARN("Invalid DMA-BUF object {} on decoder '{}'", object_index, codec_name_);
                return nullptr;
            }
            for (std::uint32_t previous_index = 0; previous_index < object_index; ++previous_index) {
                if (exported_object.fd == export_descriptor.objects[previous_index].fd) {
                    LOG_WARN("Repeated DMA-BUF fd in VA-API export on decoder '{}'", codec_name_);
                    return nullptr;
                }
            }

            ScopedDmaBufFileDescriptor duplicated_dma_buf_fd(::dup(exported_object.fd));
            if (duplicated_dma_buf_fd.GetDmaBufFileDescriptor() < 0) {
                LOG_WARN(
                    "dup() failed for DMA-BUF object {} on decoder '{}': errno {}", object_index, codec_name_, errno);
                return nullptr;
            }
            DmaBufObject dma_buf_object;
            dma_buf_object.fd_ = duplicated_dma_buf_fd.GetDmaBufFileDescriptor();
            dma_buf_object.size_bytes_ = exported_object.size;
            dma_buf_object.format_modifier_ = exported_object.drm_format_modifier;
            hardware_frame->dma_buf_objects_.push_back(dma_buf_object);
            // push_back 成功后立刻移交所有权；发生异常则仍由局部 RAII 关闭 fd。
            (void)duplicated_dma_buf_fd.ReleaseDmaBufFileDescriptor();
        }

        // 4b. 每个 plane：记录它在哪个 object 内、偏移多少、行跨度多少。
        //     dma_buf_index_ 是 dma_buf_objects_ 的下标，不是 fd；多个 plane 可共用 object。
        //     offset 是对象内的字节偏移，pitch 是相邻两行之间的字节跨度。
        hardware_frame->planes_.reserve(exported_layer.num_planes);
        for (std::uint32_t plane_index = 0; plane_index < exported_layer.num_planes; ++plane_index) {
            DmaBufPlane dma_buf_plane;
            dma_buf_plane.dma_buf_index_ = exported_layer.object_index[plane_index];
            dma_buf_plane.offset_bytes_ = exported_layer.offset[plane_index];
            dma_buf_plane.pitch_bytes_ = exported_layer.pitch[plane_index];
            dma_buf_plane.drm_format_ = GetDrmPlaneFourcc(exported_layer.drm_format, plane_index);
            hardware_frame->planes_.push_back(dma_buf_plane);
        }

        // FourCC 描述像素格式，modifier 描述线性、分块或压缩排布。
        // 共同的数据层验证会检查 object/plane 引用、字节对齐、完整存储足迹与溢出。
        // 未支持的 modifier 或辅助平面布局必须失败；通过这里只说明描述可表示，
        // 不代表目标 EGLDisplay/GPU 支持，驱动格式查询由第三阶段 Renderer 负责。
        if (!hardware_frame->ValidateDmaBufLayout()) {
            LOG_WARN(
                "Unsupported DMA-BUF format/modifier/plane bounds on decoder '{}' at epoch {}", codec_name_, epoch);
            return nullptr;
        }

        // ---------- 第 5 步：持有 AVFrame 引用，锁住 VA surface ----------
        // ★ 这是零拷贝里第二关键的步骤（第一关键是 vaSyncSurface）。
        //
        //   VA Surface 由 FFmpeg 的硬件帧缓冲池管理。
        //   当前 AVFrame 持有的资源引用没有全部释放前，Surface 不能被缓冲池重新分配。
        //
        //   不持有引用的话：
        //     帧 N   → 导出 fd → 存进队列（GUI 线程还没渲染）
        //             解码器 av_frame_unref → surface 归还池
        //     帧 N+5  → 解码器重用该 surface，写入新画面
        //     GUI    → 终于来渲染帧 N 的 fd，读到的是**帧 N+5 的画面**
        //
        //   症状：画面偶发错帧，不崩溃，日志无异常，极难定位。
        hardware_frame->av_frame_ref_ = av_frame_alloc();
        if (hardware_frame->av_frame_ref_ == nullptr ||
            av_frame_ref(hardware_frame->av_frame_ref_, decoded_frame_) < 0) {
            LOG_WARN("av_frame_ref failed on decoder '{}' at epoch {}", codec_name_, epoch);
            // HardwareFrame 析构会关闭已 dup 的 fd 并释放 av_frame_ref_
            return nullptr;
        }

        // ---------- 第 6 步：关闭描述符里的原始 fd ----------
        // 我们已经 dup 了自己的副本，原始 fd 用完即关。
        // 这里必须按 object 遍历，不能按 plane（plane 不拥有 fd）。
        exported_dma_buf_owner.ReleaseExportedDmaBufFileDescriptors();

        LOG_DEBUG(
            "Exported DMA-BUF on decoder '{}' at epoch {}: {}x{}, {} object(s), {} plane(s), "
            "fourcc={}, modifier=0x{:x}",
            codec_name_,
            epoch,
            hardware_frame->width_,
            hardware_frame->height_,
            hardware_frame->dma_buf_objects_.size(),
            hardware_frame->planes_.size(),
            GetDrmFourccName(hardware_frame->layer_format_),
            hardware_frame->dma_buf_objects_[0].format_modifier_);

        return hardware_frame;
#else
        // 构建时没找到 libva：此处不参与编译
        (void)epoch;
        return nullptr;
#endif
    } catch (const std::bad_alloc&) {
        // 局部 RAII 先释放原始 fd、尚未移交的 dup fd、已构造的 HardwareFrame。
        // 返回 nullptr 后仍由 ConvertDecodedFrameToVideoFrame 执行原有下载回退。
        LOG_WARN("Insufficient memory exporting DMA-BUF on decoder '{}' at epoch {}", codec_name_, epoch);
        return nullptr;
    } catch (const std::length_error&) {
        LOG_WARN("Invalid DMA-BUF descriptor container size on decoder '{}' at epoch {}", codec_name_, epoch);
        return nullptr;
    }
}

/**
 * @brief 设置渲染端是否具备 DMA-BUF 导入能力
 * @details
 * 保存由 Player 注入、并与 Renderer 共享的能力标志，解码器处理每一帧时会读取该标志：
 * - true：允许尝试 DMA-BUF 导出；
 * - false：跳过导出，将当前 VA-API 硬件帧下载到 CPU 内存；
 * - nullptr：视为渲染端能力尚未确认，同样跳过导出
 */
void FfmpegVideoDecoder::SetHardwareRenderCapability(std::shared_ptr<std::atomic<bool>> capability) {
    hardware_render_capability_ = std::move(capability);
}

/**
 * @brief 清空 FFmpeg 解码器内部缓存并复位 Drain 状态
 *
 * 通常在 Seek 跳转时调用。此操作不会释放已经交给下游的 VideoFrame；
 * 下游帧仍由各自持有的资源引用管理。
 */
void FfmpegVideoDecoder::Flush() noexcept {
    LOG_DEBUG("Video Decoder '{}' flushing cached frames", codec_name_);
    if (codec_context_ != nullptr) {
        avcodec_flush_buffers(codec_context_);
    }

    if (decoded_frame_ != nullptr) {
        av_frame_unref(decoded_frame_);
    }

    drain_started_ = false;
    drained_ = false;
    ResetIntervalStatistics();
}

/**
 * @brief 返回码流中声明的编码宽度
 */
int FfmpegVideoDecoder::GetCodedWidth() const noexcept {
    return codec_context_ != nullptr ? codec_context_->coded_width : 0;
}

/**
 * @brief 返回码流中声明的编码高度
 */
int FfmpegVideoDecoder::GetCodedHeight() const noexcept {
    return codec_context_ != nullptr ? codec_context_->coded_height : 0;
}

/**
 * @brief 从解码上下文中提取一帧画面，输出硬件帧或软件 YUV420P VideoFrame
 * @param epoch 目标帧所属的数据流世代版本号
 */
FfmpegDecoderReceiveResult FfmpegVideoDecoder::ReceiveFrame(std::uint64_t epoch) {
    if (codec_context_ == nullptr || decoded_frame_ == nullptr) {
        LOG_ERROR("Video Decoder '{}' cannot receive frame for epoch {} because its internal FFmpeg objects are null",
                  codec_name_,
                  epoch);
        RecordError();
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                CreateVideoDecoderError(kReceiveFrameOperation, AVERROR(EINVAL))};
    }

    const auto frame_processing_started_at = std::chrono::steady_clock::now();
    const auto receive_started_at = frame_processing_started_at;
    const std::uint64_t receive_cpu_started_ns = CurrentThreadCpuTimeNanoseconds();
    const int receive_result = avcodec_receive_frame(codec_context_, decoded_frame_);
    const double receive_elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - receive_started_at).count();

    if (receive_result == 0) {
        const AVPixelFormat decoded_pixel_format = static_cast<AVPixelFormat>(decoded_frame_->format);
        const int decoded_width = decoded_frame_->width;
        const int decoded_height = decoded_frame_->height;
        // 成功提取到解码帧：可能封装为 HardwareFrame，也可能转换为软件像素帧。
        FfmpegDecoderReceiveResult result = ConvertDecodedFrameToVideoFrame(epoch);
        const double frame_processing_elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - frame_processing_started_at).count();
        const std::uint64_t receive_cpu_finished_ns = CurrentThreadCpuTimeNanoseconds();
        if (result.status_ == FfmpegDecoderReceiveStatus::kFrame && result.frame_.has_value()) {
            LogFirstFrameInfo(*result.frame_, decoded_pixel_format, decoded_width, decoded_height);
            RecordOutputPath(decoded_pixel_format, *result.frame_);
        }
        av_frame_unref(decoded_frame_);
        RecordReceive(receive_elapsed_ms,
                      receive_cpu_finished_ns >= receive_cpu_started_ns
                          ? receive_cpu_finished_ns - receive_cpu_started_ns
                          : 0,
                      true);
        if (result.status_ == FfmpegDecoderReceiveStatus::kFrame) {
            RecordFrameProcessing(frame_processing_elapsed_ms);
        } else if (result.status_ == FfmpegDecoderReceiveStatus::kError) {
            RecordError();
        }
        MaybeLogIntervalStatistics();
        return result;
    }

    if (receive_result == AVERROR(EAGAIN)) {
        const std::uint64_t receive_cpu_finished_ns = CurrentThreadCpuTimeNanoseconds();
        RecordReceive(receive_elapsed_ms,
                      receive_cpu_finished_ns >= receive_cpu_started_ns
                          ? receive_cpu_finished_ns - receive_cpu_started_ns
                          : 0,
                      false);
        MaybeLogIntervalStatistics();
        return {FfmpegDecoderReceiveStatus::kNeedInput, std::nullopt, {}};
    }

    if (receive_result == AVERROR_EOF) {
        const std::uint64_t receive_cpu_finished_ns = CurrentThreadCpuTimeNanoseconds();
        RecordReceive(receive_elapsed_ms,
                      receive_cpu_finished_ns >= receive_cpu_started_ns
                          ? receive_cpu_finished_ns - receive_cpu_started_ns
                          : 0,
                      false);
        drained_ = true;
        LOG_DEBUG("Video Decoder '{}' drain completed for epoch {}", codec_name_, epoch);
        MaybeLogIntervalStatistics();
        return {FfmpegDecoderReceiveStatus::kDrained, std::nullopt, {}};
    }

    const std::uint64_t receive_cpu_finished_ns = CurrentThreadCpuTimeNanoseconds();
    RecordReceive(receive_elapsed_ms,
                  receive_cpu_finished_ns >= receive_cpu_started_ns
                      ? receive_cpu_finished_ns - receive_cpu_started_ns
                      : 0,
                  false);
    RecordError();
    LOG_ERROR("avcodec_receive_frame failed for decoder '{}', epoch {}, code {}", codec_name_, epoch, receive_result);
    MaybeLogIntervalStatistics();
    return {FfmpegDecoderReceiveStatus::kError,
            std::nullopt,
            CreateVideoDecoderError(kReceiveFrameOperation, receive_result)};
}

/**
 * @brief 将 decoded_frame_ 中的原始 AVFrame 转换为统一的 VideoFrame
 * @details
 * 返回的 VideoFrame 要么是硬件帧（hardware_ 非空，没有 CPU 像素），要么是 YUV420P 软件帧
 */
FfmpegDecoderReceiveResult FfmpegVideoDecoder::ConvertDecodedFrameToVideoFrame(std::uint64_t epoch) {
    // ① 基础校验。宽高必须为正，format 必须是合法的 AVPixelFormat
    // 这些字段来自 avcodec_receive_frame 填充的 AVFrame，
    if (decoded_frame_->width <= 0 || decoded_frame_->height <= 0 || decoded_frame_->format < 0) {
        LOG_ERROR("Decoder '{}' produced an invalid frame for epoch {}: {}x{}, format {}",
                  codec_name_,
                  epoch,
                  decoded_frame_->width,
                  decoded_frame_->height,
                  decoded_frame_->format);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                CreateVideoDecoderError(kReceiveFrameOperation, AVERROR_INVALIDDATA)};
    }

    // 把 int 形式的 format 转成枚举。
    // 注意这里读到的是"解码器直接吐出来"的格式，硬解时它是 AV_PIX_FMT_VAAPI，
    // 其中的 data[] 存放的是 VASurfaceID，CPU 读不到真实像素。
    const AVPixelFormat decoded_pixel_format = static_cast<AVPixelFormat>(decoded_frame_->format);

    // ==================== ② 零拷贝：优先尝试导出 DMA-BUF ====================
    // 与下面「下载」分支的关系：
    //   导出成功 → 返回 HardwareFrame，Renderer 后续通过 EGLImage 导入原有图像内存。
    //   导出失败 → 将当前 VA-API 硬件帧下载到 CPU 内存，再转换为软件 VideoFrame；
    //              这只是硬件帧下载，不代表切换成软件解码器
    //
    // 导出还受两个独立开关控制，任一为 false 时跳过：
    //   dma_buf_export_enabled_       — 运行期连续失败 30 次后关闭（本流不再尝试）
    //   hardware_render_capability_   — 渲染端写入的能力标志：
    //       nullptr 或 false → 能力尚未确认或当前不可用，跳过导出，直接下载硬件帧
    //       true             → 渲染端报告可以尝试 EGL DMA-BUF 导入
    //
    // MEDIACORE_HAVE_VAAPI 由 CMake 在找到 libva 开发依赖时定义，控制下面的 VA-API DMA-BUF 导出代码。
    // 未定义时不尝试 DMA-BUF 导出；如果解码器仍输出 VA-API 硬件帧，会走后面的 CPU 下载分支。

    // 检查渲染端是否已确认可以导入 DMA-BUF；nullptr 视为"尚未确认"，保守跳过导出
    const bool renderer_can_import_dma_buf =
        hardware_render_capability_ != nullptr && hardware_render_capability_->load(std::memory_order_acquire);

#ifdef MEDIACORE_HAVE_VAAPI
    if (decoded_pixel_format == AV_PIX_FMT_VAAPI && dma_buf_export_enabled_ && renderer_can_import_dma_buf) {
        if (auto hardware_frame = ExportHardwareFrame(epoch); hardware_frame != nullptr) {
            // 导出成功：构造硬件帧 VideoFrame
            VideoFrame output_video_frame;
            output_video_frame.width_ = decoded_frame_->width;
            output_video_frame.height_ = decoded_frame_->height;
            output_video_frame.epoch_ = epoch;
            output_video_frame.hardware_ = std::move(hardware_frame);

            // 时间戳与色彩元数据的提取逻辑必须与软件路径完全一致，
            // 否则同一路视频在软/硬路径下会得到不同的色彩矩阵。
            const std::int64_t hardware_frame_timestamp_ticks = decoded_frame_->best_effort_timestamp != AV_NOPTS_VALUE
                ? decoded_frame_->best_effort_timestamp
                : decoded_frame_->pts;
            output_video_frame.pts_us_ =
                ConvertTimestampToMicroseconds(hardware_frame_timestamp_ticks, video_stream_time_base_);
            output_video_frame.color_space_ = ConvertAvColorSpaceToVideoColorSpace(decoded_frame_->colorspace);
            output_video_frame.color_range_ = static_cast<VideoColorRange>(decoded_frame_->color_range);

            // 连续失败计数清零：说明这条流当前可以正常导出
            consecutive_export_failures_ = 0;

            return {FfmpegDecoderReceiveStatus::kFrame, std::move(output_video_frame), {}};
        }

        // 导出失败：累加计数，超过阈值就关闭导出，避免每帧都白白尝试 + 刷日志
        ++consecutive_export_failures_;
        constexpr int kMaxConsecutiveExportFailures = 30;
        if (consecutive_export_failures_ >= kMaxConsecutiveExportFailures) {
            dma_buf_export_enabled_ = false;
            LOG_ERROR(
                "DMA-BUF export failed {} times consecutively on decoder '{}'; "
                "disabling zero-copy for this stream and using download path",
                consecutive_export_failures_,
                codec_name_);
        } else {
            LOG_WARN(
                "DMA-BUF export failed on decoder '{}' at epoch {} ({}/{}), "
                "falling back to download for this frame",
                codec_name_,
                epoch,
                consecutive_export_failures_,
                kMaxConsecutiveExportFailures);
        }
    }
#endif

    // ==================== ③ 回退：将 VA-API 硬件帧下载到 CPU 内存 ====================
    // 以下情况会走到这里：
    //   · 渲染端尚未确认 DMA-BUF 导入能力；
    //   · dma_buf_export_enabled_ 已因连续导出失败被关闭；
    //   · 本帧 ExportHardwareFrame() 返回 nullptr（同步、导出、布局校验或引用保存失败等）
    //
    // 注意：这里不会更换解码器，当前帧仍由 VA-API 解码
    // 只是把已经解码完成的像素数据复制到 CPU 内存，供后续软件格式转换使用
    // 导出成功的帧在上面已经返回，不会经过这里
    if (decoded_pixel_format == AV_PIX_FMT_VAAPI) {
        AvFrameOwner downloaded_frame(av_frame_alloc());
        if (downloaded_frame == nullptr) {
            return {FfmpegDecoderReceiveStatus::kError,
                    std::nullopt,
                    CreateVideoDecoderError(kAllocateFrameOperation, AVERROR(ENOMEM))};
        }

        // 下载像素数据。这一步是 GPU→CPU 的真实拷贝，耗时随分辨率增长
        const auto transfer_started_at = std::chrono::steady_clock::now();
        const int transfer_result = av_hwframe_transfer_data(downloaded_frame.get(), decoded_frame_, 0);
        RecordHardwareTransfer(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - transfer_started_at)
                                   .count());
        if (transfer_result < 0) {
            LOG_ERROR("{} failed for decoder '{}' at epoch {}: {}",
                      kTransferFrameOperation,
                      codec_name_,
                      epoch,
                      ffmpeg_detail::DescribeFfmpegError(kTransferFrameOperation, transfer_result));
            // 运行期下载失败不自动切换软件解码器：
            // 切换需要重新打开解码器并 Seek 回合适的关键帧，还要处理已输出帧的时间戳连续性
            // 这里直接上报错误，由上层决定是否重建解码器
            return {FfmpegDecoderReceiveStatus::kError,
                    std::nullopt,
                    CreateVideoDecoderError(kTransferFrameOperation, transfer_result)};
        }

        // av_hwframe_transfer_data 只搬像素，不搬帧属性
        // 而下面 ④-3 要读 best_effort_timestamp / pts，
        // 后面还要读 colorspace / color_range 选择 YUV→RGB 矩阵
        // 少了这些，会得到"像素正确但时间戳和色彩矩阵错误"的画面，所以必须检查返回值
        const int copy_properties_result = av_frame_copy_props(downloaded_frame.get(), decoded_frame_);
        if (copy_properties_result < 0) {
            LOG_ERROR("{} failed for decoder '{}' at epoch {}: {}",
                      kCopyFramePropertiesOperation,
                      codec_name_,
                      epoch,
                      ffmpeg_detail::DescribeFfmpegError(kCopyFramePropertiesOperation, copy_properties_result));
            return {FfmpegDecoderReceiveStatus::kError,
                    std::nullopt,
                    CreateVideoDecoderError(kCopyFramePropertiesOperation, copy_properties_result)};
        }

        // 交接：用 move_ref 而不是直接赋值
        // AVFrame::data 指向引用计数的 AVBuffer，直接赋值会泄漏原数据，
        // 而且只搬指针不搬所有权
        av_frame_unref(decoded_frame_);
        av_frame_move_ref(decoded_frame_, downloaded_frame.get());
        // downloaded_frame 此时已经是空壳，由局部 RAII 自动释放
    }
    // ==================== 下载分支结束 ====================

    // 下载后格式已经变了（可能是 NV12），必须重新读取
    // 下载后的格式以实际 AVFrame::format 为准，不要假设所有设备和驱动都返回同一种格式
    //
    // 这一步如果漏掉，后续 sws_getCachedContext 会拿旧的 AV_PIX_FMT_VAAPI 去建转换器，
    // 必然失败，而且错误信息不会指向真正的原因
    const AVPixelFormat software_pixel_format = static_cast<AVPixelFormat>(decoded_frame_->format);

    // ④-1 确认 libswscale 认识这个源格式
    // sws_isSupportedInput 返回 <= 0 表示无法转换，此时无论直通还是sws_scale 都没有意义，直接报错
    if (sws_isSupportedInput(software_pixel_format) <= 0) {
        LOG_ERROR("Decoder '{}' produced unsupported swscale input format {} for epoch {}",
                  codec_name_,
                  decoded_frame_->format,
                  epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                CreateVideoDecoderError(kReceiveFrameOperation, AVERROR(EINVAL))};
    }

    // ④-2 预计算输出帧的字节数，只用于"防御性检查"，
    // 这个值实际上没有被用于分配（VideoFrame 内部自己算），
    // 这里只用它来验证 width/height 是否会导致尺寸溢出，返回负值说明宽高组合非法
    const int output_buffer_size_bytes =
        av_image_get_buffer_size(AV_PIX_FMT_YUV420P, decoded_frame_->width, decoded_frame_->height, 1);
    if (output_buffer_size_bytes < 0) {
        LOG_ERROR("Decoder '{}' produced invalid output dimensions {}x{} for epoch {}, code {}",
                  codec_name_,
                  decoded_frame_->width,
                  decoded_frame_->height,
                  epoch,
                  output_buffer_size_bytes);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                CreateVideoDecoderError(kValidateFrameSizeOperation, output_buffer_size_bytes)};
    }

    // ④-3 提取时间戳
    // best_effort_timestamp 是 FFmpeg 根据容器时间戳、帧序、B 帧重排等信息
    // "尽力推算"出来的显示时间；直接用 pts 可能在有 B 帧时不准
    // 两者都没有（AV_NOPTS_VALUE）时，ConvertTimestampToMicroseconds 返回 nullopt，
    // 下游 AvSyncNode 会用"上一帧 PTS + 帧时长"补齐
    const std::int64_t frame_timestamp_ticks = decoded_frame_->best_effort_timestamp != AV_NOPTS_VALUE
        ? decoded_frame_->best_effort_timestamp
        : decoded_frame_->pts;
    const std::optional<std::int64_t> presentation_timestamp_us =
        ConvertTimestampToMicroseconds(frame_timestamp_ticks, video_stream_time_base_);

    // ④-4 判断能否跳过 sws_scale
    // 条件：源格式已经是 YUV420P，且三个平面的 stride 都是正的
    // 注意用的是 software_pixel_format（下载之后重新读到的格式）：
    // 硬解帧下载出来通常是 NV12，不是 YUV420P，因此会落到下面的 sws_scale 分支
    const bool can_copy_yuv420p_planes = software_pixel_format == AV_PIX_FMT_YUV420P &&
        decoded_frame_->linesize[0] > 0 && decoded_frame_->linesize[1] > 0 && decoded_frame_->linesize[2] > 0;

    try {
        // ④-5 分配输出帧
        VideoFrame output_video_frame(FrameSize{decoded_frame_->width, decoded_frame_->height});
        output_video_frame.pts_us_ = presentation_timestamp_us;
        output_video_frame.epoch_ = epoch;

        // 映射 FFmpeg 的色彩空间枚举到项目枚举
        output_video_frame.color_space_ = ConvertAvColorSpaceToVideoColorSpace(decoded_frame_->colorspace);

        // 取值范围枚举值恰好对齐：AVCOL_RANGE_UNSPECIFIED=0, MPEG=1, JPEG=2
        output_video_frame.color_range_ = static_cast<VideoColorRange>(decoded_frame_->color_range);

        // 分支 A：源已经是 YUV420P —— 只拷贝，不转换
        if (can_copy_yuv420p_planes) {
            const int frame_width = decoded_frame_->width;
            const int frame_height = decoded_frame_->height;

            // 色度平面是亮度平面的一半（2:1 下采样）
            // 宽高为奇数时向上取整，保证最后一行/列不越界
            const int chroma_width = frame_width / 2 + frame_width % 2;
            const int chroma_height = frame_height / 2 + frame_height % 2;

            // 逐平面拷贝，av_image_copy_plane 的签名：
            //   av_image_copy_plane(dst, dst_linesize, src, src_linesize, bytewidth, height)
            // 它每一行拷贝 bytewidth 个字节，行与行之间分别走 dst/src 的 linesize。
            av_image_copy_plane(output_video_frame.data_[0],
                                output_video_frame.linesize_[0],
                                decoded_frame_->data[0],
                                decoded_frame_->linesize[0],
                                frame_width,
                                frame_height);  // Y 平面
            av_image_copy_plane(output_video_frame.data_[1],
                                output_video_frame.linesize_[1],
                                decoded_frame_->data[1],
                                decoded_frame_->linesize[1],
                                chroma_width,
                                chroma_height);  // U 平面
            av_image_copy_plane(output_video_frame.data_[2],
                                output_video_frame.linesize_[2],
                                decoded_frame_->data[2],
                                decoded_frame_->linesize[2],
                                chroma_width,
                                chroma_height);  // V 平面

            // 不需要 sws_scale，直接返回成功
            return {FfmpegDecoderReceiveStatus::kFrame, std::optional<VideoFrame>(std::move(output_video_frame)), {}};
        }

        // 分支 B：源是其它格式（NV12 / YUV422P / P010 / YUVJ420P …）
        //  —— 必须真正做格式转换

        // sws_getCachedContext 的行为：
        //   - 参数没变 → 返回已有的 swscale 转换上下文（不重新分配）
        //   - 参数变了（例如分辨率变化、像素格式变化）→ 释放旧的、新建一个
        // 所以它能自动处理"流中途分辨率变化"的情况，
        // 不需要调用方额外判断
        software_scaler_context_ = sws_getCachedContext(software_scaler_context_,
                                                        decoded_frame_->width,
                                                        decoded_frame_->height,
                                                        software_pixel_format,  // 源格式（硬解下载后通常是 NV12）
                                                        decoded_frame_->width,
                                                        decoded_frame_->height,
                                                        AV_PIX_FMT_YUV420P,  // 目标格式
                                                        SWS_BILINEAR,  // 缩放算法；因为不缩放，实际不生效
                                                        nullptr,
                                                        nullptr,
                                                        nullptr);

        if (software_scaler_context_ == nullptr) {
            // 分配失败（内存不足）或参数非法
            LOG_ERROR("sws_getCachedContext failed for decoder '{}', epoch {}, {}x{}, source format {}",
                      codec_name_,
                      epoch,
                      decoded_frame_->width,
                      decoded_frame_->height,
                      static_cast<int>(software_pixel_format));
            return {FfmpegDecoderReceiveStatus::kError,
                    std::nullopt,
                    CreateVideoDecoderError(kConvertFrameOperation, AVERROR(ENOMEM))};
        }

        // sws_scale 要求第 4 个槽位是 nullptr（YUV420P 只有 3 个平面），
        // linesize 的第 4 个槽位填 0，这是 FFmpeg 的约定
        std::array<std::uint8_t*, 4> destination_data{
            output_video_frame.data_[0], output_video_frame.data_[1], output_video_frame.data_[2], nullptr};
        std::array<int, 4> destination_linesize{
            output_video_frame.linesize_[0], output_video_frame.linesize_[1], output_video_frame.linesize_[2], 0};

        // 执行转换。返回"转换了多少行"，正常应该等于 height
        // src_slice_y = 0、src_slice_h = height 表示处理整帧
        const int converted_height = sws_scale(software_scaler_context_,
                                               decoded_frame_->data,
                                               decoded_frame_->linesize,
                                               0,
                                               decoded_frame_->height,
                                               destination_data.data(),
                                               destination_linesize.data());

        if (converted_height != decoded_frame_->height) {
            // 返回行数不对，说明转换中途失败
            LOG_ERROR("sws_scale produced {} rows for decoder '{}' at epoch {}, expected {}",
                      converted_height,
                      codec_name_,
                      epoch,
                      decoded_frame_->height);
            return {FfmpegDecoderReceiveStatus::kError,
                    std::nullopt,
                    CreateVideoDecoderError(kConvertFrameOperation, AVERROR_EXTERNAL)};
        }

        return {FfmpegDecoderReceiveStatus::kFrame, std::optional<VideoFrame>(std::move(output_video_frame)), {}};
    } catch (const std::bad_alloc&) {
        // VideoFrame 构造里 av_mallocz 失败会抛这个
        LOG_ERROR("Failed to allocate output video frame for decoder '{}' at epoch {}", codec_name_, epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                CreateVideoDecoderError(kConvertFrameOperation, AVERROR(ENOMEM))};
    } catch (const std::length_error&) {
        // std::vector 等容器请求了非法长度（VideoFrame 目前不用 vector，属防御性保留）
        LOG_ERROR("Output frame size is invalid for decoder '{}' at epoch {}", codec_name_, epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                CreateVideoDecoderError(kConvertFrameOperation, AVERROR(EINVAL))};
    } catch (const std::invalid_argument&) {
        // 参数非法（例如 FrameSize 宽高为 0）
        LOG_ERROR("Failed to create output video frame for decoder '{}' at epoch {} due to invalid arguments",
                  codec_name_,
                  epoch);
        return {FfmpegDecoderReceiveStatus::kError,
                std::nullopt,
                CreateVideoDecoderError(kConvertFrameOperation, AVERROR(EINVAL))};
    }
}

}  // namespace mediacore

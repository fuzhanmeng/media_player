/**
 * @file ffmpeg_error.h
 * @brief 将 FFmpeg 底层数字错误码转换为项目统一的结构化 MediaError
 * @details
 * 负责错误信息的翻译与适配（Adapter 模式）：
 * 1. 架构解耦：上层业务与 Pipeline 错误处理依赖统一的 MediaError，不直接暴露 FFmpeg 头文件与错误码宏；
 * 2. 文本翻译：封装底层 C 函数 av_strerror()，将负数错误码转换为清晰可读的人类语言描述；
 * 3. 错误封装：组合模块名称（Module）、原始错误码（Raw Code）以及具体失败的操作上下文（Operation），
 *    构造类型为 kFfmpeg 的标准 MediaError 对象。
 *
 * @note 本文件仅负责错误信息的格式化与封装，不负责决策错误的严重性或重试策略。
 */

#pragma once

#include <array>
#include <string>
#include <string_view>

#include "mediacore/media_error.h"

extern "C" {
#include <libavutil/error.h>
}

namespace mediacore {
namespace ffmpeg_detail {

/**
 * @brief 将 FFmpeg 错误码翻译为包含操作上下文的格式化错误描述字符串
 * @param operation 发生失败的 FFmpeg 操作或函数名（如 "av_read_frame"、"avcodec_open2"）
 * @param code FFmpeg 返回的负数整数错误码（如 AVERROR(EINVAL)、AVERROR_EOF）
 * @return 格式化后的错误描述文本；若 av_strerror 无法识别则提供 unknown 兜底描述
 */
inline std::string DescribeFfmpegError(std::string_view operation, int code) {
    // AV_ERROR_MAX_STRING_SIZE（通常为 64 字节）是 FFmpeg 保证容纳所有错误描述的安全缓冲区上限
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};

    // av_strerror 成功解析返回 0，无法识别该错误码时返回负数
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        if (operation.empty()) {
            return "unknown FFmpeg error (" + std::to_string(code) + ")";
        }
        return std::string(operation) + ": unknown FFmpeg error (" + std::to_string(code) + ")";
    }

    // 若未传入操作名称上下文，仅返回纯错误描述文本
    if (operation.empty()) {
        return buffer.data();
    }

    // 格式化输出："<operation>: <ffmpeg_error_text>"
    return std::string(operation) + ": " + buffer.data();
}

/**
 * @brief 构造归属于 kFfmpeg 类型的项目标准 MediaError 对象
 * @param module 触发错误的项目模块名称（如 "FfmpegDemuxer"、"FfmpegVideoDecoder"）
 * @param code FFmpeg 返回的原始数字错误码
 * @param operation 触发失败的具体 FFmpeg 函数或业务阶段
 * @return 包含模块源、原生错误代码与详细文字说明的 MediaError 实例
 */
inline MediaError MakeFfmpegMediaError(std::string_view module, int code, std::string_view operation) {
    return MediaError::FromFfmpeg(std::string(module), code, DescribeFfmpegError(operation, code));
}

}  // namespace ffmpeg_detail
}  // namespace mediacore
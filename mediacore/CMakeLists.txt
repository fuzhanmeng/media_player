# mediacore/CMakeLists.txt —— 播放器内核库

# ============ 收集源文件 ============
# 显式列出而非 GLOB：CMake 官方建议——GLOB 在增删文件时不会自动触发重新配置，易踩坑
set(MEDIACORE_SOURCES
    src/audio_decoder/alsa_audio_sink.cpp
    src/audio_decoder/ffmpeg_audio_decoder.cpp
    src/audio_decoder/ffmpeg_audio_decoder_node.cpp
    src/av_sync/av_sync_node.cpp 
    src/av_types/audio_codec_parameters.cpp
    src/av_types/hardware_frame.cpp
    src/av_types/packet.cpp
    src/av_types/video_codec_parameters.cpp
    src/config/player_config.cpp
    src/core/media_error.cpp
    src/core/player_factory.cpp
    src/core/player.cpp
    src/demux/ffmpeg_demuxer_node.cpp
    src/demux/ffmpeg_demuxer.cpp
    src/sink/audio_sink_node.cpp
    src/sink/video_sink_node.cpp
    src/utils/logger.cpp 
    src/video_decoder/ffmpeg_video_decoder_node.cpp
    src/video_decoder/ffmpeg_video_decoder.cpp
)

# ============ 定义静态库 ============
add_library(mediacore STATIC ${MEDIACORE_SOURCES})

# ALSA is used by the concrete audio sink. Because mediacore is a static
# library, the dependency must propagate to executables that link the library.
find_library(MEDIACORE_ASOUND_LIBRARY asound REQUIRED)

# ============ 头文件搜索路径 ============
# PUBLIC：app 层可见，随依赖传播（仅 include/mediacore/ 下的契约与跨边界 DTO）
# PRIVATE：仅库自身可见，不传播（src/ 下的 DAG 框架、并发原语、各具体 Node、内部 DTO）
target_include_directories(mediacore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ============ 第三方依赖 ============
# FFmpeg：video_frame.h 用到 av_malloc / av_free（<libavutil/mem.h>）
# 因为 video_frame.h 是【公共头】，凡 include 它的目标（含 app 层 opengl_view.cpp）
# 都需要 FFmpeg 头路径与 avutil 符号，故用 PUBLIC 让依赖随头文件一起传播
# SPDLOG_ACTIVE_LEVEL 必须在任何源码包含 spdlog 头文件之前生效
# 放在 target_compile_definitions 里会进入编译命令行，比在公共头文件中更好
target_compile_definitions(mediacore PUBLIC 
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE    
)

# ============ VA-API ============
# vaExportSurFacehandle / vaSyncSurfcae 都在 libva 中，不在 FFmpeg 中
#
# 为什么要用条件链接而不是 REQUIRED：
# 播放器应该能在没有 VA-API 的机器上正常构建和运行-软解
# 如果使用 find_library(... REQUIRED)，缺少库时整个项目直接失败
#
# 找到 libva 时定义 MEDIACORE_HAVE_VAAPI = 1，解码器里导出逻辑用这个宏
# DRM（Direct Rendering Manager，直接渲染管理器）是 Linux 内核中的 GPU 图形管理子系统
find_library(MEDIACORE_VA_LIBRARY NAMES va)
find_library(MEDIACORE_VA_DRM_LIBRARY NAMES  va-drm)

if(MEDIACORE_VA_LIBRARY)
    message(STATUS "Found libva: ${MEDIACORE_VA_LIBRARY}")
    target_compile_definitions(mediacore PRIVATE MEDIACORE_HAVE_VAAPI=1)
else()
    message(STATUS "libva not found: hardware zero-copy rendering disabled,software path only")
endif()

target_link_libraries(mediacore PUBLIC
    ffmpeg::avutil    # av_malloc / av_free 等内存与工具函数
    ffmpeg::avcodec   # AVPacket 相关函数
    ffmpeg::swresample # 音频解码后的统一 PCM 重采样
    ffmpeg::swscale    # 视频解码后的像素格式转换
    spdlog::spdlog    # LOG_* 宏和 logger 实现都依赖 spdlog
    ffmpeg::avformat  # ffmpeg 解复用
    ${MEDIACORE_ASOUND_LIBRARY}
)

# libva 只在找到时连接，用 PRIVATE： app 层不直接调用 va* 函数，不需要看到这些符号
if(MEDIACORE_VA_LIBRARY)
    target_link_libraries(mediacore PRIVATE ${MEDIACORE_VA_LIBRARY})
endif()

if(MEDIACORE_VA_DRM_LIBRARY)
    target_link_libraries(mediacore PRIVATE ${MEDIACORE_VA_DRM_LIBRARY})
endif()

# mediacore/CMakeLists.txt —— 播放器内核库

# ============ 收集源文件 ============
# 显式列出而非 GLOB：CMake 官方建议——GLOB 在增删文件时不会自动触发重新配置，易踩坑
set(MEDIACORE_SOURCES
    src/core/stub_player.cpp
    src/core/player_factory.cpp
    src/av_types/packet.cpp
    src/utils/logger.cpp
    src/config/player_config.cpp
    src/core/media_error.cpp
)

# ============ 定义静态库 ============
add_library(mediacore STATIC ${MEDIACORE_SOURCES})

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
find_package(ffmpeg REQUIRED)
find_package(spdlog REQUIRED)

# SPDLOG_ACTIVE_LEVEL 必须在任何源码包含 spdlog 头文件之前生效
# 放在 target_compile_definitions 里会进入编译命令行，比在公共头文件中更好
target_compile_definitions(mediacore PUBLIC 
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE    
)

target_link_libraries(mediacore PUBLIC
    ffmpeg::avutil    # av_malloc / av_free 等内存与工具函数
    ffmpeg::avcodec   # AVPacket 相关函数
    spdlog::spdlog    # LOG_* 宏和 logger 实现都依赖 spdlog
)

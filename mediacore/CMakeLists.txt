# mediacore/CMakeLists.txt —— 播放器内核库
# 从 INTERFACE 升级为 STATIC 的原因：
# 从 D5 起，mediacore 不再只是头文件集合 -- StubPlayer 有独立的 .cpp需要编译成目标文件

# ============ 收集源文件 ============
# 显式列出而非 GLOB：CMake 官方建议——GLOB 在增删文件时不会自动触发重新配置，易踩坑。
set(MEDIACORE_SOURCES
    src/core/stub_player.cpp
    src/core/player_factory.cpp
    src/core/packet.cpp
)

# ============ 定义静态库 ============
add_library(mediacore STATIC ${MEDIACORE_SOURCES})

# ============ 头文件搜索路径 ============

target_include_directories(mediacore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ============ 第三方依赖 ============
# FFmpeg：video_frame.h 用到 av_malloc / av_free（<libavutil/mem.h>）。
# 因为 video_frame.h 是【公共头】，凡 include 它的目标（含 app 层 opengl_view.cpp）
# 都需要 FFmpeg 头路径与 avutil 符号，故用 PUBLIC 让依赖随头文件一起传播。
find_package(ffmpeg REQUIRED)

target_link_libraries(mediacore PUBLIC
    ffmpeg::avutil    # av_malloc / av_free 等内存与工具函数
    ffmpeg::avcodec   # AVPacket 相关函数
)

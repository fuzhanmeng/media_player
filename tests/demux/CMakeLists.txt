# FFmpeg 视频 Demuxer 的公开接口测试。
# 使用自行生成的临时 Y4M 文件验证媒体信息、读包、Seek 和取消。

add_executable(ffmpeg_demuxer_node_test
    ffmpeg_demuxer_node_test.cpp
)

target_compile_features(ffmpeg_demuxer_node_test PRIVATE cxx_std_17)

set_target_properties(ffmpeg_demuxer_node_test PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(ffmpeg_demuxer_node_test PRIVATE
    ${PROJECT_SOURCE_DIR}/mediacore/src
)

target_link_libraries(ffmpeg_demuxer_node_test PRIVATE
    mediacore
    ffmpeg::avformat
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ffmpeg_demuxer_node_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

add_test(
    NAME demux.ffmpeg_demuxer_node
    COMMAND ffmpeg_demuxer_node_test
)

set_tests_properties(demux.ffmpeg_demuxer_node PROPERTIES TIMEOUT 5)

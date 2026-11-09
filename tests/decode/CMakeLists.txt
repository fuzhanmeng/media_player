add_executable(ffmpeg_decode_integration_test
    ffmpeg_decode_integration_test.cpp
)

target_compile_features(ffmpeg_decode_integration_test PRIVATE cxx_std_17)
target_include_directories(ffmpeg_decode_integration_test PRIVATE
    ${PROJECT_SOURCE_DIR}/mediacore/src
)

set_target_properties(ffmpeg_decode_integration_test PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(ffmpeg_decode_integration_test PRIVATE
    mediacore
    ffmpeg::avcodec
    ffmpeg::avformat
    ffmpeg::avutil
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ffmpeg_decode_integration_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

add_test(
    NAME decode.ffmpeg_compressed_av_integration
    COMMAND ffmpeg_decode_integration_test
)

set_tests_properties(decode.ffmpeg_compressed_av_integration PROPERTIES TIMEOUT 30)

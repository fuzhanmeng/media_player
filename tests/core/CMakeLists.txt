# MediaError 是 mediacore 的公共错误对象。
# 测试程序只编译自己的测试源码，然后链接 mediacore 静态库。

add_executable(media_error_test
    media_error_test.cpp
)

target_compile_features(media_error_test PRIVATE cxx_std_17)

set_target_properties(media_error_test PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(media_error_test PRIVATE
    mediacore
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(media_error_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

add_test(
    NAME core.media_error
    COMMAND media_error_test
)

set_tests_properties(core.media_error PROPERTIES TIMEOUT 5)

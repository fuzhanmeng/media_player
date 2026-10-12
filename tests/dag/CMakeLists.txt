
# 当前测试入口直接包含 mediacore/src 下的正式头文件，不链接 mediacore 静态库。
# 这样核心测试不会被 FFmpeg、Qt 或真实媒体节点依赖阻塞；日志仍使用 Conan
# 管理的 spdlog。

find_package(Threads REQUIRED)
if(NOT AI_MEDIA_PLAYER_BUILD_MEDIACORE)
    # DAG-only mode still uses the project's Conan-managed logger, but does
    # not need the full Qt/FFmpeg graph.
    find_package(spdlog 1.15.3 EXACT CONFIG REQUIRED
        PATHS "${AI_MEDIA_PLAYER_CONAN_GENERATORS_DIR}"
        NO_DEFAULT_PATH
    )
    ai_media_player_assert_conan_package(spdlog)
endif()

add_executable(all_tests
    all_tests.cpp
    ${PROJECT_SOURCE_DIR}/mediacore/src/utils/logger.cpp
)

target_compile_features(all_tests PRIVATE cxx_std_17)

# 根工程为 Qt 应用打开了 AUTOGEN；当前测试不包含 Q_OBJECT、.ui 或 .qrc，
# 在本 target 上关闭这些处理，避免测试配置寻找 Qt 包。
set_target_properties(all_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(all_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/mediacore/src
    ${PROJECT_SOURCE_DIR}/mediacore/include
)

target_compile_definitions(all_tests PRIVATE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)

target_link_libraries(all_tests PRIVATE
    Threads::Threads
    spdlog::spdlog
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(all_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

set(ALL_TEST_CASES
    eos_propagation_and_drain
    stop_while_receiving
    stop_while_sending
    stop_while_paused
    pause_resume_pause
    interrupt_blocking_io
    partial_start_rollback
    repeated_stop
    tick_exception
    join_before_destruction
    consumer_starts_first
)

foreach(test_case IN LISTS ALL_TEST_CASES)
    add_test(
        NAME all.${test_case}
        COMMAND all_tests --case ${test_case}
    )
    # 用例自身有条件变量超时；这里再设置总超时，防止线程协议回归时 CTest 永久等待。
    set_tests_properties(all.${test_case} PROPERTIES TIMEOUT 10)
endforeach()

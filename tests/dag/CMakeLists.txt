# D9 DAG 生命周期测试。
#
# 测试直接包含 mediacore/src 下的正式头文件，不链接 mediacore 静态库。
# 这样 DAG 核心测试不会被 FFmpeg、Qt 或真实媒体节点依赖阻塞。

find_package(Threads REQUIRED)

add_executable(d9_dag_lifecycle_test
    d9_dag_lifecycle_test.cpp
)

target_compile_features(d9_dag_lifecycle_test PRIVATE cxx_std_17)

# 根工程为 Qt 应用打开了 AUTOGEN；D9 测试不包含 Q_OBJECT、.ui 或 .qrc，
# 在本 target 上关闭这些处理，避免测试配置寻找 Qt 包。
set_target_properties(d9_dag_lifecycle_test PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(d9_dag_lifecycle_test PRIVATE
    ${PROJECT_SOURCE_DIR}/mediacore/src
)

target_link_libraries(d9_dag_lifecycle_test PRIVATE Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(d9_dag_lifecycle_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

set(D9_TEST_CASES
    eof_close_and_drain
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

foreach(test_case IN LISTS D9_TEST_CASES)
    add_test(
        NAME d9.${test_case}
        COMMAND d9_dag_lifecycle_test --case ${test_case}
    )
    # 用例自身有条件变量超时；这里再设置总超时，防止线程协议回归时 CTest 永久等待。
    set_tests_properties(d9.${test_case} PROPERTIES TIMEOUT 10)
endforeach()

# PlayerConfig 是 mediacore 的公共接口测试
# 测试程序只编译自己的测试源码，然后直接链接 mediacore 静态库
# 这样可以验证公共接口是否能被真实使用，同时避免重复编译实现文件

# 1. 定义可执行文件
# 创建名为 player_cfg_test 的程序，源码为 player_cfg_test.cpp
add_executable(player_cfg_test
    player_cfg_test.cpp
)

# 2. 设置 C++ 标准
# 要求编译器使用 C++17 标准 (cxx_std_17) 来编译 player_cfg_test
# 使用 PRIVATE 表示这个 C++ 标准要求只对当前这个测试目标有效
target_compile_features(player_cfg_test PRIVATE cxx_std_17)

# 3. 禁用不必要的 Qt 预处理
# 当前测试没有 Qt 对象系统相关内容，关闭自动 moc/uic/rcc。
set_target_properties(player_cfg_test PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

# 6. 链接外部库 相当于编译时的 -l 参数，链接 mediacore
target_link_libraries(player_cfg_test PRIVATE
    mediacore
)

# 7. 设置编译器警告选项
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(player_cfg_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

add_test(
    NAME config.player_config
    COMMAND player_cfg_test
)

# 8. 设置测试超时属性
set_tests_properties(config.player_config PROPERTIES TIMEOUT 5)

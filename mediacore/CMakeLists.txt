# mediacore/CMakeLists.txt —— 播放器内核库
# 用 INTERFACE 库最贴切：它不产生编译产物，只向使用者传递

add_library(mediacore INTERFACE)

target_include_directories(mediacore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# CMakeLists.txt 

# cmake 版本号定义
cmake_minimum_required(VERSION 3.23)
project(ai_media_player VERSION 1.0.0 LANGUAGES CXX)

# 全局 C++ 标准策略
# STANDARD 17       → 用 C++17
# STANDARD_REQUIRED → 编译器不支持 17 就报错，不许偷偷降级
# EXTENSIONS OFF    → 关 GNU 扩展(-std=gnu++17)，强制标准 -std=c++17，保跨平台
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出 compile_commands.json,给 clangd/VSCode 做跳转与静态分析
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Qt 代码生成流水线
# AUTOMOC：自动对含 Q_OBJECT 的头跑 moc 预处理器（signal/slot 靠它，漏了会链接报错）
# AUTORCC：自动处理 .qrc 资源文件
# AUTOUIC：自动处理 .ui 界面文件
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# CMakeDeps writes Conan package configuration files next to the Conan
# toolchain.  Keep project-managed packages on that path so a direct CMake
# configure cannot silently switch the project to a system FFmpeg/Qt/spdlog.
set(AI_MEDIA_PLAYER_CONAN_GENERATORS_DIR "${CMAKE_BINARY_DIR}/generators")
# Keep transitive CMakeDeps find_dependency() calls on the same Conan prefix
# when this project is configured with an explicitly generated toolchain.
list(PREPEND CMAKE_PREFIX_PATH "${AI_MEDIA_PLAYER_CONAN_GENERATORS_DIR}")

function(ai_media_player_assert_conan_package package_name)
    file(REAL_PATH "${${package_name}_DIR}" _package_dir_real)
    file(REAL_PATH "${AI_MEDIA_PLAYER_CONAN_GENERATORS_DIR}" _conan_dir_real)
    cmake_path(IS_PREFIX _conan_dir_real "${_package_dir_real}" NORMALIZE _is_conan_package)
    if(NOT _is_conan_package)
        message(FATAL_ERROR
            "${package_name} resolved outside Conan generators: ${_package_dir_real}")
    endif()
endfunction()

# BUILD_TESTING 是 CMake/CTest 的标准开关，默认开启。
# 两个构建选项允许在没有 FFmpeg/Qt 包的环境中只编译 DAG 测试；默认值保持
# 原来的完整播放器构建行为不变。
include(CTest)
option(AI_MEDIA_PLAYER_BUILD_MEDIACORE "Build the mediacore library" ON)
option(AI_MEDIA_PLAYER_BUILD_APP "Build the Qt application" ON)

# Import Conan targets at the project root so their directory-scoped imported
# targets are visible to all sibling subdirectories and transitive consumers.
if(AI_MEDIA_PLAYER_BUILD_MEDIACORE)
    find_package(ffmpeg 8.0.1 EXACT CONFIG REQUIRED
        PATHS "${AI_MEDIA_PLAYER_CONAN_GENERATORS_DIR}"
        NO_DEFAULT_PATH
    )
    find_package(spdlog 1.15.3 EXACT CONFIG REQUIRED
        PATHS "${AI_MEDIA_PLAYER_CONAN_GENERATORS_DIR}"
        NO_DEFAULT_PATH
    )
    ai_media_player_assert_conan_package(ffmpeg)
    ai_media_player_assert_conan_package(spdlog)
endif()

if(AI_MEDIA_PLAYER_BUILD_APP)
    find_package(Qt6 6.7.3 EXACT CONFIG REQUIRED
        COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets
        PATHS "${AI_MEDIA_PLAYER_CONAN_GENERATORS_DIR}"
        NO_DEFAULT_PATH
    )
    ai_media_player_assert_conan_package(Qt6)
endif()

if(AI_MEDIA_PLAYER_BUILD_MEDIACORE)
    add_subdirectory(mediacore)
endif()

if(AI_MEDIA_PLAYER_BUILD_APP)
    if(NOT AI_MEDIA_PLAYER_BUILD_MEDIACORE)
        message(FATAL_ERROR "AI_MEDIA_PLAYER_BUILD_APP requires AI_MEDIA_PLAYER_BUILD_MEDIACORE")
    endif()
    add_subdirectory(app)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

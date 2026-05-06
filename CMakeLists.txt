cmake_minimum_required(VERSION 3.23)
project(ai_media_player VERSION 1.0.0 LANGUAGES CXX)


# 1. Global Toolchain & Build Strategy Configuration (全局工具链与构建策略配置)
# Enforce C++17 Standard Compilation
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Qt Code Generation Pipelines
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Generate Compilation Database for LSP static analysis
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 2. External Dependency Resolution (外部依赖项寻址与链接)
# Locate third-party libraries via CMake config modules or pkg-config (定位第三方库配置模块)
find_package(ffmpeg REQUIRED CONFIG)
find_package(stb REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Multimedia)
find_package(Threads REQUIRED)       # POSIX Threads API 支持
find_package(spdlog REQUIRED CONFIG) # 高性能日志中间件



# 3. Target Definition: Core Static Library (核心逻辑层静态库构建)
# Recursively collect translation units; CONFIGURE_DEPENDS triggers re-configuration on filesystem mutation.
# (递归收集编译单元；通过监视文件系统变动触发自动重配置)
file(GLOB_RECURSE LEARN_AV_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/learn_av/*.cpp")

# Define a Static Library Target (定义静态库目标实体)
add_library(learn_av_lib STATIC ${LEARN_AV_SOURCES})

# Set PUBLIC Include Directories: Propagated to both internal compilation and external dependent targets.
# (配置公有头文件搜索路径：提供给内部编译使用，并传递给所有链接此库的外部模块)
target_include_directories(learn_av_lib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/learn_av/include
)

# Set PRIVATE Include Directories: Restricted to internal translation unit compilation only.
# (配置私有头文件搜索路径：作用域严格限制于内部源码编译，禁止向外传递)
target_include_directories(learn_av_lib PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/learn_av/src     
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/inspire_face 
)

# Configure Target Link Libraries (配置目标实体的动态/静态链接依赖)
target_link_libraries(learn_av_lib PUBLIC 
    ffmpeg::ffmpeg 
    stb::stb 
    glm::glm
    Threads::Threads
    spdlog::spdlog
)

# 4. Target Definition: Application Executable
# Collect presentation layer translation units (收集表现层编译单元)
file(GLOB_RECURSE QT_DEMO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qt/*.cpp")

# Define Executable Target (定义最终可执行文件实体)
add_executable(${PROJECT_NAME} ${QT_DEMO_SOURCES})

# Restrict presentation layer includes to internal scope (将表现层头文件作用域限制为私有)
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/qt
)

# Link core business logic target and UI framework (执行链接过程：整合核心逻辑模块与 Qt 框架)
target_link_libraries(${PROJECT_NAME} PRIVATE 
    learn_av_lib  
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Multimedia
)

# 5. Preprocessor Macro Injection (预处理器宏定义注入)
# Inject absolute filesystem paths as compilation definitions for runtime I/O 
# (将文件系统绝对路径注入为预处理宏，供运行时 I/O 操作寻址使用)
target_compile_definitions(${PROJECT_NAME} PRIVATE 
    RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resource"
    CACHE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cache"
)
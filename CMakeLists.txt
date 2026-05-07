cmake_minimum_required(VERSION 3.23)
project(ai_media_player VERSION 1.0.0 LANGUAGES CXX)

# 1. Global Configurations (全局编译与构建策略配置)

# 设置 C++ 标准为 C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 启用 Qt 的元对象编译 (MOC)、资源编译 (RCC) 和 UI 文件编译 (UIC) 自动化
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 生成 compile_commands.json，供 clangd 等 LSP (语言服务器) 进行代码补全与静态分析
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# 2. Dependency Resolution (外部依赖项寻址)
# 查找通过包管理器 (如 Conan/vcpkg) 或系统环境变量安装的第三方依赖
find_package(ffmpeg REQUIRED CONFIG)
find_package(stb REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Multimedia)
find_package(Threads REQUIRED) # POSIX 线程库支持


# 3. Target Definition: Static Library静态库构建
file(GLOB_RECURSE LEARN_AV_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/learn_av/*.cpp")

add_library(learn_av_lib STATIC ${LEARN_AV_SOURCES})

# 配置 Target 的头文件搜索路径 (Include Directories)
target_include_directories(learn_av_lib 
    PUBLIC 
        # PUBLIC：qt 模块也会继承这里的路径，所以放对外暴露的接口
        ${CMAKE_CURRENT_SOURCE_DIR}/learn_av/include
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/inspire_face 
    PRIVATE
        # PRIVATE：仅 learn_av_lib 内部编译时可见
        ${CMAKE_CURRENT_SOURCE_DIR}/learn_av/src
)

# 配置 Target 的链接依赖 (Link Libraries)
target_link_libraries(learn_av_lib PUBLIC 
    ffmpeg::ffmpeg 
    stb::stb 
    glm::glm
    Threads::Threads
)


# 4. Target Definition: Application Executable 应用层：可执行程序构建
# 收集表现层 (Qt GUI) 源代码
file(GLOB_RECURSE QT_DEMO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/qt/*.cpp")

# 定义可执行文件 Target
add_executable(${PROJECT_NAME} ${QT_DEMO_SOURCES})

# 应用层仅供内部编译使用，不向外暴露，因此声明为 PRIVATE
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/qt
)

# 链接业务逻辑层 (learn_av_lib) 及应用层框架 (Qt6)
target_link_libraries(${PROJECT_NAME} PRIVATE 
    learn_av_lib  
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Multimedia
)


# 5. 预处理宏注入

# 将资源目录的绝对路径作为宏定义注入到 C++ 源码中，方便运行时寻址
target_compile_definitions(${PROJECT_NAME} PRIVATE 
    RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resource"
    CACHE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cache"
)
cmake_minimum_required(VERSION 3.23)
project(ai_media_player VERSION 1.0.0)

# 1. 全局配置 
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 2. 引入 Conan 依赖 
find_package(ffmpeg REQUIRED CONFIG)
find_package(stb REQUIRED CONFIG)
find_package(glm REQUIRED CONFIG)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Multimedia)

# 3. 构建底层静态库 (learn_av) 
file(GLOB_RECURSE LEARN_AV_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/learn_av/*.cpp")
add_library(learn_av_lib STATIC ${LEARN_AV_SOURCES})

target_include_directories(learn_av_lib PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/learn_av/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/inspire_face 
)

target_link_libraries(learn_av_lib PRIVATE 
    ffmpeg::ffmpeg 
    stb::stb 
    glm::glm
)

# 4. 构建主程序 (qt) 
file(GLOB_RECURSE QT_DEMO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/qt/*.cpp")
add_executable(${PROJECT_NAME} ${QT_DEMO_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/qt
)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    learn_av_lib  
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Multimedia
)

# 5. 注入宏定义 
target_compile_definitions(${PROJECT_NAME} PRIVATE 
    RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resource"
    CACHE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/cache"
)
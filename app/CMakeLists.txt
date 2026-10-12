# app/CMakeLists.txt —— Qt 应用程序

# REQUIRED：找不到直接报错终止
# COMPONENTS：只找用得到的模块，Qt 是组件化的
# Core/Gui/Widgets -> 基础对象系统 + Gui + 窗口事件
# OpenGL/OpenGLWidgets -> QOpenGLWidget 要用
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

# 定义可执行文件 target
add_executable(ai_media_player
    main.cpp
    controller_widget.cpp
    player_widget.cpp
    opengl_view.cpp
)

# 链接 Qt 和媒体核心库
target_link_libraries(ai_media_player PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    mediacore
)

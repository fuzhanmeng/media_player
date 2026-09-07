# app/CMakeLists.txt —— Qt 应用程序

# 创建播放器应用程序
add_executable(ai_media_player
    main.cpp
    controller_widget.cpp
    player_widget.cpp
    opengl_view.cpp
)

# 设置默认视频路径
target_compile_definitions(ai_media_player PRIVATE
    AI_MEDIA_PLAYER_DEFAULT_MEDIA="${PROJECT_SOURCE_DIR}/../resource/video/video0.mp4"
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

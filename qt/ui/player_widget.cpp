/**
 * @file player_widget
 * @brief Description
 */
#include "player_widget.h"

#include <QVBoxLayout>

#include "iplayer.h"
#include "logger.h"
#include "opengl_view.h"

PlayerWidget::PlayerWidget(QWidget* parent, std::shared_ptr<av::IPlayer> player)
    : QWidget(parent), m_player(std::move(player)) {
    // 1. 初始化容器布局，去除所有边框
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // 2. 实例化真正的 OpenGL 渲染板
    m_opengl_view = new av::OpenGLView(this);
    layout->addWidget(m_opengl_view);

    // 3.
    if (m_player) {
        void* view_handle = m_opengl_view->GetVideoDisplayView();
        LOG_INFO("Attaching OpenGL view handle: [{}]", view_handle);
        m_player->AttachDisplayView(view_handle);
    }
}

PlayerWidget::~PlayerWidget() {
    // 在 UI 窗口销毁前，必须通知底层引擎停止向该句柄推流，
    // 否则底层还在往已销毁的内存里画图，必报段错误 (Segmentation Fault) 崩溃
    if (m_player && m_opengl_view) {
        void* view_handle = m_opengl_view->GetVideoDisplayView();
        LOG_INFO("Detaching OpenGL view handle [{}] for destruction...", view_handle);

        m_player->DetachDisplayView(view_handle);

        LOG_INFO("OpenGL view handle detached successfully.");
    }
}
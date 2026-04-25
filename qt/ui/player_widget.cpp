/**
 * @file player_widget
 * @brief Description
 */
#include "player_widget.h"
#include "opengl_view.h"
#include <QVBoxLayout>

#include "iplayer.h"

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
        m_player->AttachDisplayView(m_opengl_view->GetVideoDisplayView());
    }
}

PlayerWidget::~PlayerWidget() {
    // 在 UI 窗口销毁前，必须通知底层引擎停止向该句柄推流，
    // 否则底层还在往已销毁的内存里画图，必报段错误 (Segmentation Fault) 崩溃
    if (m_player && m_opengl_view) {
        m_player->DetachDisplayView(m_opengl_view->GetVideoDisplayView());
    }
}
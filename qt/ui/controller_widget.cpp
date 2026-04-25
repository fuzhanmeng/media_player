#include "controller_widget.h"

#include <QDebug>
#include <QHBoxLayout>
#include <QPushButton>

#include "iplayer.h"

ControllerWidget::ControllerWidget(QWidget* parent, std::shared_ptr<av::IPlayer> player)
    : QWidget(parent), m_player(std::move(player)) {
    // 设置底部控制面板固定高度
    setFixedHeight(60);

    // 初始化布局 (水平排列)
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0,0, 0);

    // 初始化播放器按钮
    m_buttonPlay = new QPushButton(tr("Play"), this);

    // 连接点击信号与槽函数
    connect(m_buttonPlay, &QPushButton::clicked, this, &ControllerWidget::onPlayButtonClicked);

    layout->addWidget(m_buttonPlay);
    setLayout(layout);
}

ControllerWidget::~ControllerWidget() = default;

void ControllerWidget::onPlayButtonClicked() {
    // 如果没有播放器实例，直接返回
    if (!m_player) {
        return;
    }
    qDebug() << "[UI 交互] 播放/暂停按钮被点击了！";
    // 根据底层引擎的状态，决定是调用 Play 还是 Pause
    if (m_player->IsPlaying()) {
        m_player->Pause();
        m_buttonPlay->setText(tr("Play"));
    } else {
        m_player->Play();
        m_buttonPlay->setText(tr("Pause"));
    }
}
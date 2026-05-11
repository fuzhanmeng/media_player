/**
 * @file controller_widget
 * @brief 底部控制面板的实现
 */
#include "controller_widget.h"
#include "logger.h"

#include <QDebug>
#include <QHBoxLayout>
#include <QPushButton>

#include "iplayer.h"

ControllerWidget::ControllerWidget(QWidget* parent, std::shared_ptr<av::IPlayer> player)
    : QWidget(parent), m_player(std::move(player)) {
    // 1. 设置自身 UI 属性
    setFixedHeight(60);

    // 2. 初始化布局管理器(水平排列)
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // 3. 创建 UI 控件 (物理按键)
    m_buttonPlay = new QPushButton(tr("Play"), this);

    // 4. 连接点击信号与槽函数
    // connect(发送者, 发送者发出的信号, 接收者, 接收者要执行的动作)
    connect(m_buttonPlay, &QPushButton::clicked, this, &ControllerWidget::onPlayButtonClicked);

    // 5. 组装 UI
    layout->addWidget(m_buttonPlay);
    setLayout(layout);
}

ControllerWidget::~ControllerWidget() = default;

void ControllerWidget::onPlayButtonClicked() {
    // 如果没有播放器实例，直接返回
    if (!m_player) {
        LOG_ERROR("m_player is null");
        return;
    }
    LOG_INFO("Click Play or Pause");
    if (m_player->IsPlaying()) {
        // 如果底层当前正在播放，说明用户想要暂停
        m_player->Pause(); // 发送暂停指令给底层引擎
        m_buttonPlay->setText(tr("Play")); // 改变按钮文字，提示用户下次点击将恢复播放
    } else {
        // 如果底层当前处于停止/暂停状态，说明用户想要播放
        m_player->Play(); // 发送播放指令给底层引擎
        m_buttonPlay->setText(tr("Pause")); // 改变按钮文字，提示用户下次点击将暂停
    }
}
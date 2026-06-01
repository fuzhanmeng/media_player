/**
 * @file title_bar_widget.cpp
 * @brief 自定义无边框窗口标题栏实现
 */
#include "title_bar_widget.h"

#include <QMouseEvent>
#include <QStyle>

TitleBarWidget::TitleBarWidget(QWidget* parent) : QWidget(parent) {
    SetupUI();
    SetupConnections();
}

void TitleBarWidget::SetWindowTitle(const QString& title) { m_titleLabel_->setText(title); }

void TitleBarWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit sigMaximizeClicked();
        event->accept();
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TitleBarWidget::SetupUI() {
    // 设置标题栏高度和背景色
    setFixedHeight(36);

    setStyleSheet(R"(
        TitleBarWidget {
            background-color: #2b2b2b;  /* 标题栏背景色：深灰色 */
            border-bottom: 1px solid #444444;  /* 底部边框色 */
        }
        QLabel {
            color: #ffffff;  /* 标题文字颜色：白色 */
            font-size: 14px;
            padding-left: 12px;
        }
        QPushButton {
            width: 46px;
            height: 36px;
            border: none;
            background-color: transparent;
            color: #ffffff;  /* 按钮图标颜色：白色 */
            font-size: 16px;
        }
        QPushButton:hover {
            background-color: #3c3f41;  /* 鼠标悬停时的背景色 */
        }
        QPushButton#closeBtn:hover {
            background-color: #e81123;  /* 关闭按钮悬停时的红色 */
        }
    )");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 窗口标题
    m_titleLabel_ = new QLabel("AI Media Player", this);
    layout->addWidget(m_titleLabel_, 1);

    // 最小化按钮
    m_minimizeBtn_ = new QPushButton("─", this);
    layout->addWidget(m_minimizeBtn_);

    // 最大化/还原按钮
    m_maximizeBtn_ = new QPushButton("□", this);
    m_maximizeBtn_->setObjectName("maximizeBtn"); 
    layout->addWidget(m_maximizeBtn_);

    // 关闭按钮
    m_closeBtn_ = new QPushButton("✕", this);
    m_closeBtn_->setObjectName("closeBtn");
    layout->addWidget(m_closeBtn_);
}

void TitleBarWidget::SetupConnections() {
    connect(m_minimizeBtn_, &QPushButton::clicked, this, &TitleBarWidget::sigMinimizeClicked);
    connect(m_maximizeBtn_, &QPushButton::clicked, this, &TitleBarWidget::sigMaximizeClicked);
    connect(m_closeBtn_, &QPushButton::clicked, this, &TitleBarWidget::sigCloseClicked);
}
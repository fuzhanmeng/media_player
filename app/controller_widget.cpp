/**
 * @file controller_widget.cpp
 * @brief 构造子控件、布局，并把控件事件转发为本类信号
 */

#include "controller_widget.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>

#include "mediacore/logger.h"

ControllerWidget::ControllerWidget(QWidget* parent)
    : QWidget(parent),
      play_pause_button_(new QPushButton("Play", this)),
      progress_slider_(new QSlider(Qt::Horizontal, this)) {
    // 进度条用 [0, 1000] 整数刻度表示播放百分比，避免浮点数带来的精度误差
    progress_slider_->setRange(0, 1000);

    auto* layout = new QHBoxLayout(this);
    layout->addWidget(play_pause_button_);
    layout->addWidget(progress_slider_);

    // QPushButton::clicked 带一个 checked 参数，这里始终不需要
    // 用 lambda 是因为只需转发点击动作本身，不做任何业务判断
    connect(play_pause_button_, &QPushButton::clicked, this, [this]() {
        LOG_INFO("Play / Pause button clicked");
        emit PlayPauseClicked();
    });

    // QSlider::sliderReleased 不带参数，而 Seek 信号需要携带位置，因此在 lambda 内
    // 读取 progress_slider_->value() 后再发出
    connect(progress_slider_, &QSlider::sliderReleased, this, [this]() {
        const int position = progress_slider_->value();
        LOG_INFO("Progress slider released, position {}", position);
        emit Seek(position);
    });

    LOG_INFO("ControllerWidget constructed");
}

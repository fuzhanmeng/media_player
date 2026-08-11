/**
 * @file controller_widget.cpp
 * @brief 构造子控件、布局、转发信号
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
    // 进度条用 [0, 1000] 整数刻度表示百分比，避免浮点数精度丢失问题
    progress_slider_->setRange(0, 1000);

    auto* layout = new QHBoxLayout(this);
    layout->addWidget(play_pause_button_);
    layout->addWidget(progress_slider_);

    // 按钮的 clicked(bool check) 我们不关心 checked 参数，
    // 用lambda 显示表达式 "只是转发动作"，而不是业务判断
    connect(play_pause_button_, &QPushButton::clicked, this, [this]() {
        LOG_INFO("Play / Pause button clicked");
        emit PlayPauseClicked();
    });

    // sliderReleased()无参，Seek(int) 需要当前位置，必须在 lambda 手动读取 progress_slider_->value
    connect(progress_slider_, &QSlider::sliderReleased, this, [this]() {
        const int position = progress_slider_->value();
        LOG_INFO("Progress slider released, position {}", position);
        emit Seek(position);
    });

    LOG_INFO("ControllerWidget constructed");
}

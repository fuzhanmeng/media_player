/**
 * @file player_widget.cpp
 * @brief 连接 UI 和 Iplayer
 */
#include "player_widget.h"

#include <QVBoxLayout>

#include "controller_widget.h"

/**
 * @brief 构造函数：创建子控件 OpenGLView/ControllerWidget，并把它的信号接到本类的槽函数上
 * @details QBoxLayout 分配剩余空间时优先看 stretch factor（伸展因子），SizePolicy 只在没有
 *          stretch factor 竞争时才起作用：gl_view_ 传 stretch=1 独占剩余空间，controller_
 *          不传（默认 0，停在自身 sizeHint）自然贴底，无需额外的 addStretch() 占位弹簧。
 *          - 第 1/2 个参数：谁发出信号（sender）/发出的是哪个信号（成员函数指针）
 *          - 第 3/4 个参数：谁来响应（receiver）/调用它的哪个成员函数（槙函数）
 */
PlayerWidget::PlayerWidget(mediacore::IPlayer* player, QWidget* parent)
    : player_(player), controller_(new ControllerWidget(this)), gl_view_(new OpenGLView(this)) {
    auto* layout = new QVBoxLayout(this);

    // 1. 让幕布（gl_view_）拥有拉伸权重（stretch = 1）
    layout->addWidget(gl_view_, 1);
    // 2. 控制条（controller_，包含进度条和按钮）默认拉伸权重为 0
    layout->addWidget(controller_);

    connect(controller_, &ControllerWidget::PlayPauseClicked, this, &PlayerWidget::OnPlayPauseClicked);

    connect(controller_, &ControllerWidget::Seek, this, &PlayerWidget::OnSeek);

    // 把 OpenGLView 绑定到 播放器
    if (player_ != nullptr) {
        player_->AttachDisplayView(gl_view_);
    }
}

/**
 * @brief 析构函数，将 OpenGLView 解绑
 */
PlayerWidget::~PlayerWidget() {
    if (player_ != nullptr) {
        player_->DetachDisplayView(gl_view_);
    }
}

/**
 * @brief 播放/暂停
 */
void PlayerWidget::OnPlayPauseClicked() {
    if (player_ == nullptr) {
        return;
    }

    if (player_->GetState() == mediacore::PlaybackState::kPlaying) {
        player_->Pause();
    } else {
        player_->Play();
    }
}

/**
 * @brief 跳转
 */
void PlayerWidget::OnSeek(int position) {
    if (player_ == nullptr) {
        return;
    }

    // TODO: 拿到媒体时长元数据后，把 [0, 1000] 百分比换算成精确的微秒时间戳
    static_cast<void>(position);
}
/**
 * @file player_widget.cpp
 * @brief 连接 UI 和 Iplayer
 * @details 持有一个非拥有的 IPlayer* 指针， 只是调用但是不拥有其管理权限

 */
#include "player_widget.h"

#include <QVBoxLayout>

#include "controller_widget.h"
#include "mediacore/logger.h"

/**
 * @brief 将播放状态转换为日志可读字符串
 */
namespace {
const char* PlaybackStateToString(mediacore::PlaybackState state) {
    switch (state) {
        case mediacore::PlaybackState::kIdle:
            return "Idle";
        case mediacore::PlaybackState::kOpening:
            return "Opening";
        case mediacore::PlaybackState::kPlaying:
            return "Playing";
        case mediacore::PlaybackState::kPaused:
            return "Paused";
        case mediacore::PlaybackState::kStopped:
            return "Stopped";
        case mediacore::PlaybackState::kError:
            return "Error";
    }
    return "Unknown";
}
}  // namespace

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
        LOG_INFO("OpenGLView attached to player");
    } else {
        LOG_WARN("PlayerWidget constructed without player");
    }

    LOG_INFO("PlayerWidget constructed");
}

/**
 * @brief 析构函数，将 OpenGLView 解绑
 */
PlayerWidget::~PlayerWidget() {
    if (player_ != nullptr) {
        player_->DetachDisplayView(gl_view_);
        LOG_INFO("OpenGLView detached from player");
    } else {
        LOG_WARN("PlayerWidget destroyed without player");
    }

    LOG_INFO("PlayerWidget destroyed");
}

/**
 * @brief 播放/暂停
 */
void PlayerWidget::OnPlayPauseClicked() {
    if (player_ == nullptr) {
        LOG_WARN("Ignored Play/Pause request because player is null");
        return;
    }

    const mediacore::PlaybackState state = player_->GetState();
    LOG_INFO("Play/Pause request received, player state {}", PlaybackStateToString(state));

    if (state == mediacore::PlaybackState::kPlaying) {
        LOG_INFO("Pause request sent to player");
        player_->Pause();
    } else {
        LOG_INFO("Play request sent to player");
        player_->Play();
    }
}

/**
 * @brief 跳转
 */
void PlayerWidget::OnSeek(int position) {
    if (player_ == nullptr) {
        LOG_WARN("Ignored seek request {} because player is null", position);
        return;
    }

    LOG_INFO("Seek request sent to player, slider position {}", position);
    player_->Seek(position);
}

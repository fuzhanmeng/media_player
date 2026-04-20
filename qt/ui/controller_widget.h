/**
 * @brief 底部控制面板组件
 * 承担用户的交互输入（播放、暂停、进度跳转等）。
 */
#pragma once

#include <QPushButton> // 按钮控件
#include <memory>

namespace av {
class IPlayer;
}

class ControllerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ControllerWidget(QWidget* parent, std::shared_ptr<av::IPlayer> player);
    ~ControllerWidget() override;

private slots:
    /**
     * @brief 处理播放/暂停按钮点击事件
     */
    void onPlayButtonClicked();

private:
    std::shared_ptr<av::IPlayer> m_player;
    QPushButton* m_buttonPlay{nullptr};
};
/**
 * @file controller_widget.h
 * @brief 播放控制条 UI：播放/暂停按钮 + 进度条
 * @details 只关心 用户操作 -> 发出信号，完全不知道 IPlayer 的存在，信号驱动 IPlayer 是 PlayerWidget
            要做的
 */

#pragma once

#include <QWidget>

class QPushButton;
class QSlider;

/**
 * @brief 播放控制条：纯 UI 控件，不持有任何业务对象
 */
class ControllerWidget : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 显示构造函数
     */
    explicit ControllerWidget(QWidget* parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~ControllerWidget() override = default;

signals:
    /**
     * @brief 用户点击了播放/暂停按钮
     */
    void PlayPauseClicked();

    /**
     * @brief 用户拖动进度条并释放，发生跳转
     * @param position 进度条位置， 范围[0, 1000]
     */
    void Seek(int position);

private:
    QPushButton* play_pause_button_; ///< 播放/暂停按钮
    QSlider* progress_slider_; ///< 进度条
};
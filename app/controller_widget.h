/**
 * @file controller_widget.h
 * @brief 播放控制条 UI：播放/暂停按钮和进度条
 * @details
 * 只负责把用户操作转成信号发出，不直接调用 IPlayer；信号到 IPlayer 调用的转发由 PlayerWidget 完成
 */

#pragma once

#include <QWidget>

class QPushButton;
class QSlider;

/**
 * @brief 播放控制条：纯 UI 控件，不持有 IPlayer 或其他业务对象
 */
class ControllerWidget : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 构造函数
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
    QPushButton* play_pause_button_;  ///< 播放/暂停按钮
    QSlider* progress_slider_;  ///< 进度条
};
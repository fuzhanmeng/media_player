/**
 * @file player_widget.h
 * @brief 连接 UI 和 Iplayer
 * @details
 * 持有一个非拥有的 IPlayer* 指针，只调用不管理其生命周期
 * PlayerWidget 把 ControllerWidget 发出的纯 UI 信号翻译成对 IPlayer 的业务调用
 */

#pragma once

#include <QWidget>

#include "mediacore/iplayer.h"
#include "opengl_view.h"

class ControllerWidget;

/**
 * @class PlayerWidget
 * @brief UI 与 IPlayer 之间的桥梁
 */
class PlayerWidget : public QWidget {
    Q_OBJECT
public:
    /**
     * @brief 构造函数，通过依赖注入接收 IPlayer 实例
     * @param player 非拥有指针：生命周期由调用方管理，PlayerWidget 不负责释放
     * @param parent Qt 父子对象树的父指针
     */
    explicit PlayerWidget(mediacore::IPlayer* player, QWidget* parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~PlayerWidget() override;

private slots:
    /**
     * @brief 响应 ControllerWidget::PlayPauseClicked，按当前状态决定 Playing 还是 Pause
     */
    void OnPlayPauseClicked();

    /**
     * @brief 响应 ControllerWidget::Seek
     * @param position 进度条位置 [0, 1000]
     */
    void OnSeek(int position);

private:
    mediacore::IPlayer* player_;  ///< IPlayer 实例
    ControllerWidget* controller_;  ///< 子控件，Qt 父子对象树会自动管理其生命周期
    OpenGLView* gl_view_;  ///< 视频渲染画布
};
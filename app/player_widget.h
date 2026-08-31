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
     * @brief 创建播放窗口
     * @param player 非拥有指针：生命周期由调用方管理，PlayerWidget 不负责释放
     * @param parent Qt 父子对象树的父指针
     */
    explicit PlayerWidget(mediacore::IPlayer* player, QWidget* parent = nullptr);

    /**
     * @brief 解除显示绑定并销毁窗口子控件
     */
    ~PlayerWidget() override;

private slots:
    /**
     * @brief 转发播放/暂停操作
     */
    void OnPlayPauseClicked();

    /**
     * @brief 将滑块位置换算为媒体时间并请求 Seek
     * @param position 进度条位置 [0, 1000]
     */
    void OnSeek(int position);

private:
    mediacore::IPlayer* player_;  ///< IPlayer 实例
    ControllerWidget* controller_;  ///< 子控件，Qt 父子对象树会自动管理其生命周期
    OpenGLView* gl_view_;  ///< 视频输出控件
};
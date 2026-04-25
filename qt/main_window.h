/**
 * @file main_window
 * @brief Qt主窗口UI控件
 */
#pragma once

#include <QDockWidget> // 可拖拽的浮动面板
#include <QLabel> // 标签控件
#include <QMouseEvent>
#include <QOpenGLContext> // 封装了 OpenGL 的状态机上下文
#include <QResizeEvent>
#include <QSlider> // 滑动条控件
#include <QSplitter>
#include <QWidget>

class QSlider; 
class QResizeEvent;
class QMouseEvent;

class PlayerWidget;
class ControllerWidget;

namespace av {
class IPlayer;
class IPlaybackListener;
} // namespace av

/**
 * @brief 播放器主窗口
 * 采用 QWidget 实现自定义无边框窗口，手动处理鼠标拖拽与双击事件
 */
class MainWindow : public QWidget {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

protected:
    /**
     * @brief 窗口大小变化事件
     * @param event 包含新旧尺寸信息
     */
    void resizeEvent(QResizeEvent* event) override;

    /**
     * @brief 用于实现拖拽窗口：记录按下时的相对坐标
     * @param event 鼠标事件对象，包含鼠标坐标和按键类型
     */
    void mousePressEvent(QMouseEvent* event) override;

    /**
     * @brief 用于实现拖拽窗口：计算偏移量并移动窗口
     * @param event 鼠标事件对象
     */
    void mouseMoveEvent(QMouseEvent* event) override;

    /**
     * @brief 拖拽结束
     * @param event 鼠标事件对象
     */
    void mouseReleaseEvent(QMouseEvent* event) override;

    /**
     * @brief 用于实现双击全屏/退出全屏
     * @param event 鼠标事件对象
     */
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private slots:
    /**
     * @brief 响应用户拖动进度条的槽函数
     * @param value 进度条的当前值，用于控制播放器 Seek
     */
    void onSliderMoved(int value);

private:
    std::shared_ptr<av::IPlayer> m_player; // 核心播放引擎引用
    std::shared_ptr<av::IPlaybackListener> m_listener; // 监听器引用保存（防释放）

    PlayerWidget* m_playerWidget{nullptr}; // 视频渲染区域
    ControllerWidget* m_controllerWidget{nullptr}; // 控制面板区域
    QSlider* m_progressSlider{nullptr}; // 播放进度条
};
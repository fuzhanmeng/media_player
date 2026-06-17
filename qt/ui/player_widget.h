/**
 * @file player_widget.h
 * @brief 视频渲染容器组件 (Qt 视图与底层引擎的翻译中枢)
 * @details 负责管理底层的OpenGL渲染视图，提供视频帧渲染接口，自动处理窗口大小变化
 * 同时作为观察者监听底层引擎状态，并将其转换为 Qt 信号供外部控制面板使用
 */
#pragma once

#include <QWidget>
#include <memory>
#include <string>

// 引入底层播放器接口以继承 Listener
#include "iplayer.h"

namespace av {
class IPlayer;
class OpenGLView;
} // namespace av

/**
 * @class PlayerWidget
 * @brief 视频渲染窗口类
 * @details 作为OpenGL渲染视图的容器，负责与播放器核心引擎的交互，提供视频帧渲染、显示视图绑定和解绑功能。
 * @inherit QWidget 继承自 Qt 基础组件
 * @inherit av::IPlayer::Listener 继承自底层引擎监听器，接收纯 C++ 回调
 */
class PlayerWidget : public QWidget, public av::IPlayer::Listener {
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param parent 父窗口指针
     */
    explicit PlayerWidget(QWidget* parent = nullptr);

    /**
     * @brief 析构函数
     * @details 自动解绑显示视图，释放所有渲染资源，并注销引擎监听器
     */
    ~PlayerWidget() override;

    /**
     * @brief 设置播放器实例
     * @param player 播放器智能指针
     * @details 自动解绑之前的播放器，绑定新的播放器，并建立双向事件流
     */
    void SetPlayer(std::shared_ptr<av::IPlayer> player);

    // ================== 向外暴露的 Qt 事件信道 ==================
signals:
    /**
     * @brief 播放状态变更信号
     * @param state 最新的原子播放状态
     * @details 供外部控制面板 (ControllerWidget) 绑定，用于切换播放/暂停图标
     */
    void sigPlaybackStateChanged(av::PlaybackState state);

    /**
     * @brief 播放进度更新信号
     * @param progress 当前进度百分比 [0.0, 1.0]
     * @param current_time 当前播放时间（秒）
     * @param total_time 视频总时长（秒）
     * @details 供外部控制面板绑定，用于驱动进度条 (QSlider) 和时间文本更新
     */
    void sigPlaybackProgress(float progress, float current_time, float total_time);

    /**
     * @brief 播放流物理结束信号
     * @details 供外部控制面板绑定，用于展示“重播”按钮或切集逻辑
     */
    void sigPlaybackCompleted();

    /**
     * @brief 播放器异常信令
     * @param error_code 错误码
     * @param error_msg 错误信息 (已转换为 Qt 友好的 QString)
     * @details 供主窗口 (MainWindow) 绑定，用于弹出全局错误提示框
     */
    void sigPlaybackError(int error_code, const QString& error_msg);

    /**
     * @brief 鼠标单击切换实现播放/暂停功能
     */
    void sigVideoClicked();

protected:
    /**
     * @brief 窗口大小变化事件处理函数
     * @param event 包含新旧窗口尺寸信息的事件对象
     * @details 自动调整OpenGL渲染视图的大小
     */
    void resizeEvent(QResizeEvent* event) override;

    /**
     * @brief 当鼠标产生中断，操作系统将消息发往应用程序界面,鼠标单击切换实现播放/暂停功能
     */
    void mousePressEvent(QMouseEvent* event) override;

private:
    // ================== IPlayer::Listener 底层回调接口实现 ==================

    /**
     * @brief 统一的状态流转通知
     * @details 彻底替代繁琐的 Started/Paused/Resumed/Stopped 回调。
     * 当底层 PlaybackState 发生原子跃迁时，触发此回调。
     * @param state 跃迁后的最新状态
     */
    void OnPlaybackStateChanged(av::PlaybackState state) override;

    /**
     * @brief 播放流物理结束
     * @details 当媒体文件读取完毕，且所有帧渲染出列时触发
     * @note 这通常意味着 UI 需要展示“重播”按钮，或者自动切入下一个视频
     */
    void OnPlaybackCompleted() override;
    /**
     * @brief 播放错误
     * @param error_code 错误码
     * @param error_msg 错误信息
     */
    void OnPlaybackError(int error_code, const std::string& error_msg) override;

    /**
     * @brief 播放进度更新
     * @param progress      当前播放进度，绝对比例范围 [0.0f, 1.0f]
     * @param current_us    当前播放的绝对物理时间（单位：微秒）
     * @param total_us      媒体全局总时长（单位：微秒）
     */
    void OnPlaybackProgress(float progress, int64_t current_us, int64_t total_us) override;

private:
    /**
     * @brief 初始化UI组件
     */
    void SetupUI();

    /**
     * @brief 解绑当前播放器
     */
    void DetachPlayer();

    /// 播放器智能指针 (掌握底层引擎的生命周期)
    std::shared_ptr<av::IPlayer> m_player;

    /// OpenGL渲染视图纯指针 (实际的黑板/发光二极管)
    av::OpenGLView* m_opengl_view{nullptr};
};
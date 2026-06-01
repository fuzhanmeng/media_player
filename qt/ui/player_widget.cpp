/**
 * @file player_widget.cpp
 * @brief 视频渲染容器组件实现
 * @details 作为Qt UI层与底层OpenGL渲染引擎的桥梁，负责管理OpenGL渲染视图的生命周期，
 * 提供与播放器核心引擎的绑定/解绑接口，自动处理窗口大小变化和资源释放，
 * 确保渲染线程与UI线程的安全交互
 */
#include "player_widget.h"

#include <QResizeEvent>
#include <QVBoxLayout>

#include "iplayer.h"
#include "logger.h"
#include "opengl_view.h"

/**
 * @brief 构造函数
 * @param parent 父窗口指针
 */
PlayerWidget::PlayerWidget(QWidget* parent) : QWidget(parent) { SetupUI(); }

/**
 * @brief 析构函数
 * @details 自动解绑显示视图，释放所有渲染资源，并注销引擎监听器
 */
PlayerWidget::~PlayerWidget() { DetachPlayer(); }

/**
 * @brief 设置播放器实例
 * @param player 播放器智能指针
 * @details 自动解绑之前的播放器，绑定新的播放器，并建立双向事件流
 */
void PlayerWidget::SetPlayer(std::shared_ptr<av::IPlayer> player) {
    LOG_INFO("Setting new player instance");

    // 先安全解绑之前的播放器，避免资源泄漏和悬空指针
    DetachPlayer();

    // 转移所有权，绑定新的播放器实例
    m_player = std::move(player);

    if (m_player) {
        // 将显示视图注册到播放器核心引擎
        m_player->AttachDisplayView(m_opengl_view);

        // 将自己注册为引擎状态监听者，打通底层的事件上行通道
        m_player->SetListener(this);

        LOG_INFO("OpenGL view handle and UI Listener attached successfully");
    }
}

/**
 * @brief 窗口大小变化事件处理函数
 * @param event 包含新旧窗口尺寸信息的事件对象
 * @details 自动调整OpenGL渲染视图的大小
 */
void PlayerWidget::resizeEvent(QResizeEvent* event) {
    // 调用父类的resizeEvent处理基本的窗口大小变化
    QWidget::resizeEvent(event);

    // 同步调整OpenGL渲染视图的大小
    if (m_opengl_view) {
        m_opengl_view->resize(event->size());
    }
}

/**
 * @brief 状态流转通知
 * @details 彻底替代繁琐的 Started/Paused/Resumed/Stopped 回调
 * 当底层 PlaybackState 发生原子跃迁时，触发此回调。
 * @param state 跃迁后的最新状态
 */
void PlayerWidget::OnPlaybackStateChanged(av::PlaybackState state) { emit sigPlaybackStateChanged(state); }

/**
 * @brief 播放流物理结束
 * @details 当媒体文件读取完毕，且所有帧渲染出列时触发
 * @note 这通常意味着 UI 需要展示“重播”按钮，或者自动切入下一个视频
 */
void PlayerWidget::OnPlaybackCompleted() { emit sigPlaybackCompleted(); }

/**
 * @brief 播放错误
 * @param error_code 错误码
 * @param error_msg 错误信息
 */
void PlayerWidget::OnPlaybackError(int error_code, const std::string& error_msg) {
    emit sigPlaybackError(error_code, QString::fromStdString(error_msg));
}

/**
 * @brief 播放进度更新
 * @param progress 当前进度[0.0, 1.0]
 * @param current_time 当前时间(秒)
 * @param total_time 总时长(秒)
 */
void PlayerWidget::OnPlaybackProgress(float progress, float current_time, float total_time) {
    emit sigPlaybackProgress(progress, current_time, total_time);
}

/**
 * @brief 初始化UI组件
 */
void PlayerWidget::SetupUI() {
    // 去除所有边框和边距，确保视频渲染区域占满整个容器
    setContentsMargins(0, 0, 0, 0);

    // 优化渲染性能，减少不必要的重绘
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);

    // 创建垂直布局，用于容纳OpenGL渲染视图
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 创建OpenGL渲染视图实例，加入布局
    m_opengl_view = new av::OpenGLView(this);
    layout->addWidget(m_opengl_view);

    setLayout(layout);

    LOG_INFO("PlayerWidget UI initialized successfully");
}

/**
 * @brief 解绑当前播放器
 */
void PlayerWidget::DetachPlayer() {
    if (m_player) {
        // 注销回调监听，防止底层线程继续回调到即将销毁的 UI
        m_player->SetListener(nullptr);

        if (m_opengl_view) {
            // 通知核心引擎停止向该视图推流
            m_player->DetachDisplayView(m_opengl_view);
        }

        LOG_INFO("OpenGL view handle detached successfully");
    }

    // 释放播放器智能指针，减少引用计数
    m_player.reset();
}
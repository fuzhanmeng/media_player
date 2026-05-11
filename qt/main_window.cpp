/**
 * @brief 播放器主窗口
 * 采用 QWidget 实现自定义无边框窗口，手动处理鼠标拖拽与双击事件
 */

#include "main_window.h"

#include <QMouseEvent>
#include <QOpenGLContext>
#include <QSlider>
#include <QVBoxLayout>

#include "igl_context.h"
#include "iplayer.h"
#include "logger.h"
#include "ui/controller_widget.h"
#include "ui/player_widget.h"

// 具体的类来实现监听器接口
// 具体的类来实现监听器接口
class PlaybackListenerImpl : public av::IPlaybackListener {
public:
    // 构造函数接收 MainWindow 的指针，用于后续的跨线程 UI 更新
    explicit PlaybackListenerImpl(MainWindow* window) : m_window(window) {}
    ~PlaybackListenerImpl() override = default;

    // 1. 播放开始
    void OnPlaybackStarted() override {
        LOG_INFO("Playback started.");
        if (m_window) {
            QMetaObject::invokeMethod(m_window, [window = m_window]() {
                // TODO: 在主线程更新 UI (如切换播放按钮状态)
            });
        }
    }

    // 2. 播放暂停
    void OnPlaybackPaused() override {
        LOG_INFO("Playback paused.");
        if (m_window) {
            QMetaObject::invokeMethod(m_window, [window = m_window]() {
                // TODO: 在主线程更新 UI
            });
        }
    }

    // 3. 播放恢复 (从暂停继续)
    void OnPlaybackResumed() override {
        LOG_INFO("Playback resumed.");
        if (m_window) {
            QMetaObject::invokeMethod(m_window, [window = m_window]() {
                // TODO: 在主线程更新 UI
            });
        }
    }

    // 4. 播放完成 (EOF)
    void OnPlaybackCompleted() override {
        LOG_INFO("Playback completed (EOF reached).");
        if (m_window) {
            QMetaObject::invokeMethod(m_window, [window = m_window]() {
                // TODO: 在主线程重置 UI 状态
            });
        }
    }

    // 5. 播放错误
    void OnPlaybackError(int error_code, const std::string& error_msg) override {
        LOG_ERROR("Playback error: {} - {}", error_code, error_msg);
        if (m_window) {
            QMetaObject::invokeMethod(m_window, [window = m_window, error_msg]() {
                // TODO: 在主线程弹出错误提示框
            });
        }
    }

    // 6. 播放进度更新
    void OnPlaybackProgress(float progress, float current_time, float total_time) override {
        // 降低日志级别避免刷屏
        LOG_TRACE("Playback progress: {:.2f}%, time: {:.2f}s / {:.2f}s", progress * 100.0f, current_time, total_time);

        if (m_window) {
            QMetaObject::invokeMethod(m_window, [window = m_window, current_time, total_time]() {
                // TODO: 在主线程更新进度条
                // 注意：如果用户正在拖拽进度条，此时应避免更新导致滑块跳动
            });
        }
    }

private:
    MainWindow* m_window{nullptr};
};

namespace av {} // namespace av

MainWindow::MainWindow() : QWidget() {
    LOG_INFO("Run MainWindow constructor");
    // 1. 创建共享的 OpenGL 上下文 (包装成项目的 av::GLContext)
    auto sharedContext = std::make_shared<av::IGLContext>(QOpenGLContext::globalShareContext());

    // 2. 创建 Player，并设置播放回调
    m_player = av::IPlayer::Create(sharedContext);
    m_player->SetPlaybackListener(std::make_shared<PlaybackListenerImpl>(this));

    if (m_player) {
        std::string test_url = "/home/xm/Videos/video0.mp4";
        bool ret = m_player->Open(test_url);
        if (!ret) {
            LOG_ERROR("Failed to open media, URL: {}", test_url);
        } else {
            LOG_INFO("Successfully opened media, URL: {}", test_url);
        }
    }
    // 3. 设置布局
    auto* vbox = new QVBoxLayout();
    vbox->setContentsMargins(0, 0, 0, 0);
    setLayout(vbox);

    // 4. 添加主界面
    m_playerWidget = new PlayerWidget(this, m_player);
    vbox->addWidget(m_playerWidget, 1);

    // 5. 添加进度条
    m_progressSlider = new QSlider(Qt::Horizontal, this);
    m_progressSlider->setRange(0, 1000);
    // 连接进度条的sliderMoved信号到自定义的槽函数
    connect(m_progressSlider, &QSlider::sliderMoved, this, &MainWindow::onSliderMoved);
    vbox->addWidget(m_progressSlider);

    // 6. 添加播放控件
    m_controllerWidget = new ControllerWidget(this, m_player);
    vbox->addWidget(m_controllerWidget);

    // 7. 设置窗口最小尺寸
    setMinimumWidth(1440);
    setMinimumHeight(1080);
}

MainWindow::~MainWindow() = default;

// 进度条
void MainWindow::onSliderMoved(int value) {
    LOG_INFO("Slider moved by user, target value: {}", value);
    // if (m_player) m_player->Seek(value);
}

// 窗口大小变化事件
void MainWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // 改变窗口大小
}

// 鼠标按下事件（用于实现拖拽窗口：记录按下时的相对坐标）
void MainWindow::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
    // TODO: 记录鼠标点击时的相对坐标，用于计算拖拽距离
}

// 鼠标移动事件（用于实现拖拽窗口：计算偏移量并移动窗口）
void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    QWidget::mouseMoveEvent(event);
    // TODO: 计算坐标偏移，调用 move() 函数移动整个无边框窗口
}

// 鼠标释放事件（拖拽结束）
void MainWindow::mouseReleaseEvent(QMouseEvent* event) { QWidget::mouseReleaseEvent(event); }

// 简单的双击全屏/还原切换逻辑
void MainWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (isFullScreen()) {
        LOG_INFO("Double click detected, exiting full screen.");
        showNormal();
    } else {
        LOG_INFO("Double click detected, entering full screen.");
        showFullScreen();
    }
    QWidget::mouseDoubleClickEvent(event);
}
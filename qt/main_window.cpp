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
class PlaybackListenerImpl : public av::IPlaybackListener {
public:
    // 构造函数接收 MainWindow 的指针，方便以后在回调里更新 UI
    PlaybackListenerImpl(MainWindow* window) : m_window(window) {}
    ~PlaybackListenerImpl() override = default;

    void NotifyPlaybackTimeChanged(float current_pos, float duration) override {
        // 进度更新太频繁了，保留 LOG_TRACE，等发布时提高日志级别即可屏蔽
        LOG_TRACE("Time changed: pos={:.3f}s, duration={:.3f}s", current_pos, duration);
    }

    void NotifyPlaybackStarted() override { LOG_INFO("Playback started."); }

    void NotifyPlaybackPaused() override { LOG_INFO("Playback paused."); }

    void NotifyPlaybackEOF() override { LOG_INFO("Playback EOF reached."); }

private:
    MainWindow* m_window;
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
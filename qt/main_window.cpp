/**
 * @brief 播放器主窗口
 * 采用 QWidget 实现自定义无边框窗口，手动处理鼠标拖拽与双击事件
 */

#include "main_window.h"

#include <QMouseEvent>
#include <QOpenGLContext>
#include <QSlider>
#include <QVBoxLayout>

#include "../include/igl_context.h"
#include "iplayer.h"
#include "ui/controller_widget.h"
#include "ui/player_widget.h"

// 具体的类来实现监听器接口
class PlaybackListenerImpl : public av::IPlaybackListener {
public:
    // 构造函数接收 MainWindow 的指针，方便以后在回调里更新 UI
    PlaybackListenerImpl(MainWindow* window) : m_window(window) {}
    ~PlaybackListenerImpl() override = default;

    // 进度更新
    void NotifyPlaybackTimeChanged(float current_pos, float duration) override {
        // TODO: 以后在这里调 m_window 的进度条更新函数
    }

    // 播放开始
    void NotifyPlaybackStarted() override {
        // 暂时留空
    }

    // 播放暂停
    void NotifyPlaybackPaused() override {
        // 暂时留空
    }

    // 播放结束
    void NotifyPlaybackEOF() override {
        // 暂时留空
    }

private:
    MainWindow* m_window;
};
namespace av {} // namespace av

MainWindow::MainWindow() : QWidget() {
    // 1. 创建共享的 OpenGL 上下文 (包装成项目的 av::GLContext)
    auto sharedContext = std::make_shared<av::IGLContext>(QOpenGLContext::globalShareContext());

    // 2. 创建 Player，并设置播放回调
    m_player = av::IPlayer::Create(sharedContext);
    m_player->SetPlaybackListener(std::make_shared<PlaybackListenerImpl>(this));

    if (m_player) {
        bool ret = m_player->Open("/home/xm/Videos/video0.mp4");
        if (!ret) {
            // 真实项目中这里可以弹个 MessageBox 报错
            qDebug() << "视频文件打开失败，请检查路径！";
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
    // 调用底层 Seek 接口
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
        showNormal();
    } else {
        showFullScreen();
    }

    QWidget::mouseDoubleClickEvent(event);
}
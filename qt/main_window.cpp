/**
 * @file main_window.cpp
 * @brief 播放器主窗口实现类
 */

#include "main_window.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QVBoxLayout>

#include "igl_context.h"
#include "iplayer.h"
#include "logger.h"
#include "ui/controller_widget.h"
#include "ui/player_widget.h"

/**
 * @brief 构造函数
 */
MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    // 1. 开启自定义无边框窗口标志
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);

    // 2. 提取并包装全局共享的零拷贝 OpenGL 上下文环境
    m_sharedGLContext = std::make_shared<av::IGLContext>(QOpenGLContext::globalShareContext());

    // 3. 实例化播放核心大脑 (最高级生命周期持有者)
    m_player = av::IPlayer::Create(m_sharedGLContext);

    // 4. 初始化 UI 层次与布局管理
    SetupUI();

    // 5. 构建 UI 信号槽路由桥梁
    SetupConnections();

    // 6. 设置初始窗口标题
    setWindowTitle("AI Media Player");
}

/**
 * @brief 析构函数
 */
MainWindow::~MainWindow() {
    CloseMedia();
    LOG_INFO("MainWindow destroyed successfully.");
}

/**
 * @brief 重写窗口标题设置方法，同步更新自定义标题栏
 */
void MainWindow::setWindowTitle(const QString& title) {
    QWidget::setWindowTitle(title);
    if (m_titleBar_) {
        m_titleBar_->SetWindowTitle(title);
    }
}

/**
 * @brief 初始化UI组件
 */
void MainWindow::SetupUI() {
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // 1. 添加自定义标题栏到最顶部
    m_titleBar_ = new TitleBarWidget(this);
    vbox->addWidget(m_titleBar_);

    // 2. 实例化视频像素画布视图
    m_playerWidget = new PlayerWidget(this);
    m_playerWidget->SetPlayer(m_player);
    vbox->addWidget(m_playerWidget, 1);

    // 3. 实例化播放交互面板
    m_controllerWidget = new ControllerWidget(this);
    vbox->addWidget(m_controllerWidget);

    // 规范多媒体终端标准展现窗格尺寸
    setMinimumWidth(1280);
    setMinimumHeight(720);
}

/**
 * @brief 建立信号槽连接
 */
void MainWindow::SetupConnections() {
    // ================== 标题栏控制信号 ==================
    connect(m_titleBar_, &TitleBarWidget::sigMinimizeClicked, this, &MainWindow::showMinimized);
    connect(m_titleBar_, &TitleBarWidget::sigMaximizeClicked, this, [this]() {
        if (isMaximized() || isFullScreen()) {
            showNormal();
        } else {
            showMaximized();
        }
        UpdateMaximizeButtonState();
    });
    connect(m_titleBar_, &TitleBarWidget::sigCloseClicked, this, &MainWindow::close);

    // ================== 上行：捕获用户交互，指挥底层 ==================
    connect(m_controllerWidget, &ControllerWidget::sigImportClicked, this, &MainWindow::onUserImportRequested);
    connect(m_controllerWidget, &ControllerWidget::sigPlayClicked, this, &MainWindow::onUserPlayRequested);
    connect(m_controllerWidget, &ControllerWidget::sigPauseClicked, this, &MainWindow::onUserPauseRequested);
    connect(m_controllerWidget, &ControllerWidget::sigSeekRequested, this, &MainWindow::onUserSeekRequested);

    // ================== 下行：捕获底层反馈，更新UI ==================
    connect(m_playerWidget, &PlayerWidget::sigPlaybackStateChanged, this, &MainWindow::onEngineStateChanged);
    connect(m_playerWidget, &PlayerWidget::sigPlaybackCompleted, this, &MainWindow::onEnginePlaybackCompleted);
    connect(m_playerWidget, &PlayerWidget::sigPlaybackError, this, &MainWindow::onEngineError);

    // 特殊路由：将 PlayerWidget 吐出的高频进度信号，直接通过 Lambda 表达式转发给 ControllerWidget 更新视图
    connect(m_playerWidget, &PlayerWidget::sigPlaybackProgress, this,
            [this](float progress, float current, float total) {
                m_controllerWidget->SetSliderProgress(progress);
                m_controllerWidget->SetTimeDisplay(current, total);
            });
}

/**
 * @brief 更新最大化/还原按钮的显示状态
 */
void MainWindow::UpdateMaximizeButtonState() {
    if (!m_titleBar_)
        return;

    QPushButton* maximizeBtn = m_titleBar_->findChild<QPushButton*>("maximizeBtn");
    if (maximizeBtn) {
        if (isMaximized() || isFullScreen()) {
            maximizeBtn->setText("❐");
        } else {
            maximizeBtn->setText("□");
        }
    }
}

/**
 * @brief 打开媒体文件
 */
bool MainWindow::OpenMedia(const QString& url) {
    if (!m_player)
        return false;

    std::string stdUrl = url.toStdString();
    bool ret = m_player->Open(stdUrl);

    if (!ret) {
        LOG_ERROR("Failed to open media via engine, URL: {}", stdUrl);
        return false;
    }

    LOG_INFO("Successfully opened media asset, URL: {}", stdUrl);
    // 打开成功后更新窗口标题为文件名
    setWindowTitle(QFileInfo(url).fileName() + " - AI Media Player");

    return true;
}

/**
 * @brief 关闭当前媒体
 */
void MainWindow::CloseMedia() {
    LOG_INFO("CloseMedia requested, resetting client pipeline state.");
    if (m_player) {
        m_player->Stop();
    }
    // 关闭媒体后恢复默认窗口标题
    setWindowTitle("AI Media Player");
}

// ================== 接收 ControllerWidget UI 交互事件 ==================

/**
 * @brief 响应用户导入请求
 * @details 弹出系统原生文件选择对话框，获取绝对路径后触发底层流的装载
 */
void MainWindow::onUserImportRequested() {
    LOG_INFO("open media");
    QString filePath = QFileDialog::getOpenFileName(this, tr("选择视频文件"), "",
                                                    tr("视频文件 (*.mp4 *.flv *.mkv *.avi);;所有文件 (*.*)"));

    if (!filePath.isEmpty()) {
        OpenMedia(filePath);
    }
}

/**
 * @brief 响应用户播放请求
 * @details 驱动底层状态机从 kOpened 或 kPaused 跃迁至 kPlaying
 */
void MainWindow::onUserPlayRequested() {
    if (m_player)
        m_player->Play();
}

/**
 * @brief 响应用户暂停请求
 * @details 驱动底层状态机从 kPlaying 跃迁至 kPaused，挂起解码管线
 */
void MainWindow::onUserPauseRequested() {
    if (m_player)
        m_player->Pause();
}

/**
 * @brief 响应进度条拖拽完毕进行媒体时间轴跳转
 * @param target_progress 目标进度百分比 [0.0, 1.0]
 * @details 计算绝对时间(秒)并向底层派发 Seek 指令
 */
void MainWindow::onUserSeekRequested(float target_progress) {
    if (m_player) {
        LOG_INFO("Seek to: {}", target_progress);
        double target_time_sec = target_progress * m_player->GetDuration();
        m_player->SeekTo(target_time_sec);
    }
}

// ================== 接收 PlayerWidget 底层状态反馈事件 ==================

/**
 * @brief 响应播放器核心原子状态改变
 * @param state 最新的播放机状态枚举
 * @details 严格将被动属性同步给控制面板，避免 UI 状态与底层物理状态脱节
 */
void MainWindow::onEngineStateChanged(av::PlaybackState state) {
    bool isPlaying = (state == av::PlaybackState::kPlaying);
    m_controllerWidget->SetPlayState(isPlaying);
}

/**
 * @brief 响应播放器物理流读取完毕 (EOF)
 * @details 物理流排空后，强制 UI 回归未播放状态并重置所有进度数据
 */
void MainWindow::onEnginePlaybackCompleted() {
    m_controllerWidget->SetPlayState(false);
    m_controllerWidget->SetSliderProgress(0.0f);
    m_controllerWidget->SetTimeDisplay(0.0f, m_player->GetDuration());
}

/**
 * @brief 响应播放器内部致命错误
 * @param error_code 错误标识码
 * @param error_msg 详细报错信息
 * @details 发生致命错误时，切断播放状态并弹出模态对话框阻塞提示用户
 */
void MainWindow::onEngineError(int error_code, const QString& error_msg) {
    m_controllerWidget->SetPlayState(false);
    QMessageBox::critical(this, tr("播放器错误"),
                          tr("底层引擎抛出异常:\n错误码: %1\n信息: %2").arg(error_code).arg(error_msg));
}

// ================== 无边框窗口原生事件处理 ==================

/**
 * @brief 窗口大小变化事件处理函数
 * @param event 包含新旧窗口尺寸信息的事件对象
 */
void MainWindow::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); }

/**
 * @brief 鼠标按下事件处理函数
 * @param event 包含鼠标坐标和按键类型的事件对象
 * @details 用于实现无边框窗口的拖拽功能，记录按下时相对左上角的物理坐标差值
 */
void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_titleBar_ && m_titleBar_->geometry().contains(event->pos())) {
        m_isDragging = true;
        m_dragStartPosition = event->globalPosition().toPoint() - this->pos();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

/**
 * @brief 鼠标移动事件处理函数
 * @param event 包含鼠标坐标和按键状态的事件对象
 * @details 检查拖拽标志，根据全局坐标与初始偏移量实时重绘窗口位置
 */
void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_isDragging && (event->buttons() & Qt::LeftButton)) {
        this->move(event->globalPosition().toPoint() - m_dragStartPosition);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

/**
 * @brief 鼠标释放事件处理函数
 * @param event 包含鼠标坐标和按键类型的事件对象
 * @details 取消拖拽标志，终止坐标更新逻辑
 */
void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_isDragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

/**
 * @brief 鼠标双击事件处理函数
 * @param event 包含鼠标坐标和按键类型的事件对象
 * @details 实现双击切换全屏与普通窗口模式的业务逻辑
 */
void MainWindow::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_playerWidget && m_playerWidget->geometry().contains(event->pos())) {
        if (isFullScreen()) {
            LOG_INFO("Double click detected, exiting full screen mode.");
            showNormal();
        } else {
            LOG_INFO("Double click detected, entering full screen mode.");
            showFullScreen();
        }
        UpdateMaximizeButtonState();
        event->accept();
    }
    QWidget::mouseDoubleClickEvent(event);
}

/**
 * @brief 窗口关闭事件处理函数
 * @param event 包含关闭标志的事件对象
 * @details 在窗口销毁前，安全调用 CloseMedia 触发底层物理资源释放
 */
void MainWindow::closeEvent(QCloseEvent* event) {
    CloseMedia();
    QWidget::closeEvent(event);
}
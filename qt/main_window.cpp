/**
 * @file main_window.cpp
 * @brief 播放器主窗口实现类
 */

#include "main_window.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QMessageBox>
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
    // 1. 提取并包装全局共享的零拷贝 OpenGL 上下文环境
    m_sharedGLContext = std::make_shared<av::IGLContext>(QOpenGLContext::globalShareContext());

    // 2. 实例化播放核心大脑 (最高级生命周期持有者)
    m_player = av::IPlayer::Create(m_sharedGLContext);

    // 3. 初始化 UI 层次与布局管理
    SetupUI();

    // 4. 构建 UI 信号槽路由桥梁
    SetupConnections();

    // 5. 设置初始窗口标题
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
 * @brief 初始化UI组件
 */
void MainWindow::SetupUI() {
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // 1. 实例化视频像素画布视图
    m_playerWidget = new PlayerWidget(this);
    m_playerWidget->SetPlayer(m_player);
    vbox->addWidget(m_playerWidget, 1);

    // 2. 实例化播放交互面板
    m_controllerWidget = new ControllerWidget(this);
    vbox->addWidget(m_controllerWidget);

    // 规范多媒体终端标准展现窗格尺寸
    setMinimumWidth(800);
    setMinimumHeight(600);
    resize(1280, 720); // 默认打开尺寸
}

/**
 * @brief 建立信号槽连接
 */
void MainWindow::SetupConnections() {
    // ================== 上行：捕获用户交互，指挥底层 ==================
    connect(m_controllerWidget, &ControllerWidget::sigImportClicked, this, &MainWindow::onUserImportRequested);
    connect(m_controllerWidget, &ControllerWidget::sigPlayClicked, this, &MainWindow::onUserPlayRequested);
    connect(m_controllerWidget, &ControllerWidget::sigPauseClicked, this, &MainWindow::onUserPauseRequested);
    connect(m_controllerWidget, &ControllerWidget::sigSeekRequested, this, &MainWindow::onUserSeekRequested);

    // 捕获画面点击以切换播放/暂停
    connect(m_playerWidget, &PlayerWidget::sigVideoClicked, this, &MainWindow::onVideoAreaClicked);

    // ================== 下行：捕获底层反馈，更新UI ==================
    connect(m_playerWidget, &PlayerWidget::sigPlaybackStateChanged, this, &MainWindow::onEngineStateChanged);
    connect(m_playerWidget, &PlayerWidget::sigPlaybackCompleted, this, &MainWindow::onEnginePlaybackCompleted);
    connect(m_playerWidget, &PlayerWidget::sigPlaybackError, this, &MainWindow::onEngineError);

    // 特殊路由：将 PlayerWidget 吐出的高频进度信号，直接通过 Lambda 表达式转发给 ControllerWidget 更新视图
    connect(m_playerWidget, &PlayerWidget::sigPlaybackProgress, this,
            [this](float progress, int64_t current_us, int64_t total_us) {
                m_controllerWidget->SetSliderProgress(progress);

                // 微秒转换为秒
                int64_t current_sec = current_us / 1000000;
                int64_t total_sec = total_us / 1000000;
                m_controllerWidget->SetTimeDisplay(current_sec, total_sec); // 已修复为传入秒数
            });
}

/**
 * @brief 打开媒体文件
 */
bool MainWindow::OpenMedia(const QString& url) {
    // 记录路径，播放结束后重播用
    m_currentFilePath = url;

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
    if (!m_player) {
        return;
    }

    av::PlaybackState state = m_player->GetState();

    if (state == av::PlaybackState::kStopped) {
        // 播放完毕后处于Stop状态
        if (!m_currentFilePath.isEmpty()) {
            bool ok = m_player->Open(m_currentFilePath.toStdString());
            if (!ok) {
                LOG_ERROR("Failed to reopen media for replay: {}", m_currentFilePath.toStdString());
                return;
            }

            m_player->Play();
        }
    } else {
        // 首次播放
        m_player->Play();
    }
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
 * @details 计算绝对时间(微秒)并向底层派发 Seek 指令
 */
void MainWindow::onUserSeekRequested(float target_progress) {
    if (m_player) {
        LOG_INFO("Seek to: {}", target_progress);
        int64_t target_time_us = target_progress * m_player->GetDuration();
        m_player->SeekTo(target_time_us);
    }
}

/**
 * @brief 响应视频渲染区域的鼠标点击
 * @details 判断当前引擎状态，智能切换播放与暂停
 */
void MainWindow::onVideoAreaClicked() {
    if (!m_player)
        return;

    // 此处需要你在 av::IPlayer 中提供一个 GetState() 方法
    // 如果没有，你可以把控制栏的 m_isPlaying 状态透传出来做判断
    av::PlaybackState current_state = m_player->GetState();

    if (current_state == av::PlaybackState::kPlaying) {
        m_player->Pause();
    } else if (current_state == av::PlaybackState::kPaused || current_state == av::PlaybackState::kOpened) {
        m_player->Play();
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
    // 1. 重置 UI
    m_controllerWidget->SetPlayState(false);
    m_controllerWidget->SetSliderProgress(0.0f);
    m_controllerWidget->SetTimeDisplay(0LL, m_player->GetDuration());
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

// ================== 原生事件处理 ==================

/**
 * @brief 窗口关闭事件处理函数
 * @param event 包含关闭标志的事件对象
 * @details 在窗口销毁前，安全调用 CloseMedia 触发底层物理资源释放
 */
void MainWindow::closeEvent(QCloseEvent* event) {
    CloseMedia();
    QWidget::closeEvent(event);
}
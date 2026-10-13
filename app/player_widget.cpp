/**
 * @file player_widget.cpp
 * @brief 播放器核心窗口组件实现（视频选择、视图绑定、操作转发与状态同步）
 * @details
 * 负责整合 GUI 控件与 mediacore 异步控制流：
 * 1. 窗口布局：自适应组合 OpenGLView 画布、状态诊断标签与 ControllerWidget 控制条；
 * 2. 交互转发：将打开文件、播放/暂停、进度条拖拽 Seek 等事件转换为对 IPlayer 的异步调用；
 * 3. 拖拽支持：支持直接将操作系统中的本地视频文件拖入窗口起播；
 * 4. 线程安全防护：通过 QTimer 在 GUI 主线程定期拉取 IPlayer 状态快照，杜绝跨线程直接操纵 Qt 控件。
 */

#include "player_widget.h"

#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <string>

#include "controller_widget.h"
#include "mediacore/logger.h"
#include "mediacore/playback_state.h"
#include "opengl_view.h"

/**
 * @brief 构造函数：初始化子控件布局并绑定视频渲染输出
 * @param player 注入的 IPlayer 抽象控制器实例指针（非拥有指针）
 * @param parent Qt 父控件指针
 * @details
 * 执行流程：
 * 1. 实例化主垂直布局管理器 QVBoxLayout 并挂载系统菜单栏（提供文件打开快捷键 Ctrl+O）；
 * 2. 开启窗口的拖拽接收能力（setAcceptDrops(true)）；
 * 3. 按照“画面区（拉伸权重 1）-> 状态标签（权重 0）-> 底部控制条（权重 0）”的顺序装配布局；
 * 4. 建立 UI 信号槽连接，将控制条的交互事件转发至本类的响应槽；
 * 5. 将 OpenGLView 渲染终端挂载至 IPlayer，建立后台工作线程与画面呈现的通信链路；
 * 6. 启动 100ms 周期的高频定时器，在 GUI 线程中拉取 IPlayer 状态快照并同步界面表现。
 */
PlayerWidget::PlayerWidget(mediacore::IPlayer* player, QWidget* parent)
    : QWidget(parent),
      player_(player),
      controller_(new ControllerWidget(this)),
      gl_view_(new OpenGLView(this)),
      playback_status_label_(new QLabel(this)) {
    auto* layout = new QVBoxLayout(this);

    // 1. 初始化顶部菜单栏与快捷键操作
    auto* menu_bar = new QMenuBar(this);
    auto* file_menu = menu_bar->addMenu(tr("文件"));
    auto* open_action = file_menu->addAction(tr("打开视频…"));
    open_action->setShortcut(QKeySequence::Open);
    open_action->setEnabled(player_ != nullptr);
    connect(open_action, &QAction::triggered, this, &PlayerWidget::OnOpenFileClicked);
    layout->setMenuBar(menu_bar);

    // 开启窗口的外部拖拽投放接收支持
    setAcceptDrops(true);

    // 2. 布局组装与权重分配
    // 画布（gl_view_）设置拉伸因子 stretch = 1，随窗口缩放自适应占满所有剩余空间
    layout->addWidget(gl_view_, 1);
    layout->addWidget(playback_status_label_);

    // 控制条（controller_，包含进度条与控制按钮）拉伸因子为 0，保持固定最佳高度
    layout->addWidget(controller_);

    // 3. 连接控制条交互信号
    connect(controller_, &ControllerWidget::PlayPauseClicked, this, &PlayerWidget::OnPlayPauseClicked);
    connect(controller_, &ControllerWidget::Seek, this, &PlayerWidget::OnSeek);

    // 4. 将 OpenGLView 渲染终端挂载至播放器管线
    if (player_ != nullptr) {
        player_->AttachDisplayView(gl_view_);
        LOG_INFO("OpenGLView attached to player");
    } else {
        LOG_WARN("PlayerWidget constructed without player");
    }

    // 5. 启动状态定时同步机制
    // IPlayer 提供纯查询接口，在 GUI 线程中按 100ms 周期轮询刷新，保证 UI 控件变更严格限制在 Qt 主线程
    auto* state_timer = new QTimer(this);
    connect(state_timer, &QTimer::timeout, this, &PlayerWidget::RefreshPlaybackState);
    state_timer->start(100);
    RefreshPlaybackState();

    LOG_INFO("PlayerWidget constructed");
}

/**
 * @brief 析构函数：解除视频输出绑定并安全回收
 * @details
 * 析构安全保护：
 * 1. 显式调用 player_->DetachDisplayView(gl_view_)；
 * 2. 阻塞等待在途（In-flight）渲染投递任务完全退出，防止后台线程向正在销毁的 gl_view_ 发送帧导致段错误；
 * 3. 随后由 Qt 父子对象树自动析构 gl_view_、controller_ 等全部托管子控件。
 */
PlayerWidget::~PlayerWidget() {
    if (player_ != nullptr) {
        player_->DetachDisplayView(gl_view_);
        LOG_INFO("OpenGLView detached from player");
    } else {
        LOG_WARN("PlayerWidget destroyed without player");
    }

    LOG_INFO("PlayerWidget destroyed");
}

/**
 * @brief 打开指定的本地媒体文件并在完成解封装探测后起播
 * @param file_path 媒体文件的本地文件系统路径
 * @details
 * 执行步骤：
 * 1. 拦截空路径与空指针防御；
 * 2. 提取文件规范化绝对路径，并将所属目录记录到 last_media_directory_ 中，方便下次快速定位；
 * 3. 设置主窗口标题显示当前播放的文件名称；
 * 4. 向 IPlayer 发起异步 Open 请求建立管线，并立即衔接异步 Play 命令。
 */
void PlayerWidget::OpenMediaFile(const QString& file_path) {
    if (player_ == nullptr || file_path.isEmpty()) {
        return;
    }

    const QFileInfo file_info(file_path);
    const std::string media_path = file_info.absoluteFilePath().toUtf8().toStdString();
    last_media_directory_ = file_info.absolutePath();
    setWindowTitle(tr("%1 — media_player").arg(file_info.fileName()));

    LOG_INFO("Opening selected video '{}'", media_path);
    player_->Open(media_path);
    // Open 为异步任务；底层管线会在格式探测构建完毕后响应并执行此处的起播请求
    player_->Play();
}

/**
 * @brief 响应用户点击“打开文件”按钮或菜单动作
 * @details
 * 交互步骤：
 * 1. 弹出系统原生文件选择对话框（QFileDialog）；
 * 2. 过滤器覆盖主流封装格式（mp4, mkv, mov, avi, webm, flv, ts, y4m 等）；
 * 3. 校验用户是否选定了有效文件，并将结果转交至 OpenMediaFile 执行实际打开逻辑。
 */
void PlayerWidget::OnOpenFileClicked() {
    if (player_ == nullptr) {
        return;
    }

    const QString file_path = QFileDialog::getOpenFileName(
        this,
        tr("打开视频"),
        last_media_directory_,
        tr("视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.flv *.m4v *.ts *.mts *.mpeg *.mpg *.wmv *.y4m);;所有文件 (*)"));
    OpenMediaFile(file_path);
}

/**
 * @brief 拖拽进入事件处理（QDragEnterEvent）
 * @param event Qt 拖拽进入事件载荷
 * @details
 * 校验规则：
 * 1. 检查拖拽载荷是否包含单一有效的 URL；
 * 2. 校验该 URL 是否为操作系统本地文件系统中的普通物理文件；
 * 3. 校验通过则接受建议操作（acceptProposedAction），界面呈现允许投放光标。
 */
void PlayerWidget::dragEnterEvent(QDragEnterEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (player_ != nullptr && urls.size() == 1 && urls.front().isLocalFile() &&
        QFileInfo(urls.front().toLocalFile()).isFile()) {
        event->acceptProposedAction();
    }
}

/**
 * @brief 拖拽释放事件处理（QDropEvent）
 * @param event Qt 拖拽释放事件载荷
 * @details
 * 触发逻辑：
 * 1. 提取拖入的本地物理文件路径；
 * 2. 接受拖拽动作完成闭环；
 * 3. 调用 OpenMediaFile 立即加载并启动播放。
 */
void PlayerWidget::dropEvent(QDropEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (player_ != nullptr && urls.size() == 1 && urls.front().isLocalFile() &&
        QFileInfo(urls.front().toLocalFile()).isFile()) {
        OpenMediaFile(urls.front().toLocalFile());
        event->acceptProposedAction();
    }
}

/**
 * @brief 响应播放/暂停切换按钮点击
 * @details
 * 状态机联动处理：
 * 1. 若当前处于 Idle（空闲）、Stopped（已停止）或 Error（错误）状态，点击视为“打开新媒体”，触发文件选择器；
 * 2. 若当前正处于 Playing（播放中）状态，向 IPlayer 发送 Pause 请求；
 * 3. 其他可恢复状态（如 Paused、Ended），向 IPlayer 发送 Play 请求以继续或从头重播。
 */
void PlayerWidget::OnPlayPauseClicked() {
    if (player_ == nullptr) {
        LOG_WARN("Ignored Play/Pause request because player is null");
        return;
    }

    const mediacore::PlaybackState state = player_->GetState();
    LOG_INFO("Play/Pause request received, player state {}", mediacore::PlaybackStateToString(state));

    if (state == mediacore::PlaybackState::kIdle || state == mediacore::PlaybackState::kStopped ||
        state == mediacore::PlaybackState::kError) {
        OnOpenFileClicked();
    } else if (state == mediacore::PlaybackState::kPlaying) {
        LOG_INFO("Pause request sent to player");
        player_->Pause();
    } else {
        LOG_INFO("Play request sent to player");
        player_->Play();
    }
}

/**
 * @brief 响应进度条拖拽定位操作（Seek）
 * @param position 归一化进度条数值，取值区间为 [0, 1000]（千分比刻度）
 * @details
 * 时间换算与校验：
 * 1. 读取媒体总时长 duration_us（微秒）；若总时长无效（<= 0）则忽略本次跳转；
 * 2. 按比例换算目标微秒时间戳：position * duration_us / 1000；
 * 3. 通过 std::min 做防越界截断，确保目标时间戳不会超出媒体时长上限；
 * 4. 向 IPlayer 投递异步 Seek 请求。
 */
void PlayerWidget::OnSeek(int position) {
    if (player_ == nullptr) {
        LOG_WARN("Ignored seek request {} because player is null", position);
        return;
    }

    const std::int64_t duration_us = player_->GetDurationUs();
    if (duration_us <= 0) {
        LOG_WARN("Ignored seek request {}, because media duration is unknown", position);
        return;
    }

    // 将千分比刻度位置 [0, 1000] 换算为微秒时间戳
    const std::int64_t target_us = std::min<std::int64_t>(duration_us, position * duration_us / 1000);

    LOG_INFO("Seek request sent to player, slider position {} -> target {} us", position, target_us);
    player_->Seek(target_us);
}

/**
 * @brief 定时/主动刷新界面显示状态
 * @details
 * 同步逻辑：
 * 1. 若 player_ 为空，彻底禁用控制条并呈现错误提示文本；
 * 2. 查询当前 PlaybackState 快照：
 *    - 同步播放/暂停按钮的高亮与图标（kPlaying 时显示暂停图标，其余显示播放图标）；
 *    - 在 Opening 与 Seeking 状态下禁用播放/暂停按钮，防止非法并发控制打乱状态机；
 *    - 仅在 Playing、Paused 或 Ended 且总时长有效的前提下激活进度条交互；
 * 3. 依据状态机枚举，更新底部状态提示标签（playback_status_label_）的用户指引文本。
 */
void PlayerWidget::RefreshPlaybackState() {
    if (player_ == nullptr) {
        controller_->SetPlayPauseEnabled(false);
        controller_->SetSeekEnabled(false);
        playback_status_label_->setText(tr("播放器创建失败"));
        return;
    }

    const mediacore::PlaybackState state = player_->GetState();
    controller_->SetPlaying(state == mediacore::PlaybackState::kPlaying);
    controller_->SetPlayPauseEnabled(state != mediacore::PlaybackState::kOpening &&
                                     state != mediacore::PlaybackState::kSeeking);

    const bool can_seek = state == mediacore::PlaybackState::kPlaying || state == mediacore::PlaybackState::kPaused ||
        state == mediacore::PlaybackState::kEnded;
    controller_->SetSeekEnabled(can_seek && player_->GetDurationUs() > 0);

    switch (state) {
        case mediacore::PlaybackState::kIdle:
            playback_status_label_->setText(tr("点击播放、选择“文件 → 打开视频”，或拖入视频文件"));
            break;
        case mediacore::PlaybackState::kOpening:
            playback_status_label_->setText(tr("正在打开视频…"));
            break;
        case mediacore::PlaybackState::kPlaying:
            playback_status_label_->setText(tr("正在播放"));
            break;
        case mediacore::PlaybackState::kPaused:
            playback_status_label_->setText(tr("已暂停"));
            break;
        case mediacore::PlaybackState::kSeeking:
            playback_status_label_->setText(tr("正在跳转…"));
            break;
        case mediacore::PlaybackState::kEnded:
            playback_status_label_->setText(tr("播放结束，点击播放可从头重播"));
            break;
        case mediacore::PlaybackState::kStopped:
            playback_status_label_->setText(tr("已停止，点击播放可重新选择视频"));
            break;
        case mediacore::PlaybackState::kError:
            playback_status_label_->setText(tr("打开或播放失败，请重新选择视频"));
            break;
    }
}
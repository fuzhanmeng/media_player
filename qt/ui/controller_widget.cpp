/**
 * @file controller_widget.cpp
 * @brief 底部控制面板组件实现文件
 * @details 承担用户的交互输入（播放、暂停、进度跳转等）
 * 当前组件已重构为纯视图，严格剥离底层事件回调逻辑
 */
#include "controller_widget.h"

#include <QHBoxLayout> // 水平布局管理器
#include <QLabel> // 标签部件
#include <QPushButton> // 按钮
#include <QSlider> // 滑块部件

/**
 * @brief 构造函数实现
 * @param parent 父级窗口指针
 */
ControllerWidget::ControllerWidget(QWidget* parent) : QWidget(parent) {
    // 1. 设置自身 UI 属性 (固定高度)
    setFixedHeight(60);

    // 2. 调用专门的 UI 组装函数
    SetupUI();
}

/**
 * @brief 布局内部 UI 元素
 */
void ControllerWidget::SetupUI() {
    // 1. 创建 UI 控件
    m_buttonImport = new QPushButton(tr("导入"), this);
    m_buttonPlay = new QPushButton(tr("Play"), this);
    m_labelTime = new QLabel(("00:00 / 00:00"), this);

    // 创建水平方向的进度条
    m_sliderProgress = new QSlider(Qt::Horizontal, this);
    // 为了保证精度，我们将进度条的范围设置为 0 ~ 1000 (代表千分比)
    m_sliderProgress->setRange(0, 1000);

    // 2. 固定播放按钮的宽度，防止中英文切换或文本改变时按钮忽大忽小
    m_buttonPlay->setFixedWidth(80);

    // 3. 初始化布局管理器(水平排列)
    auto* layout = new QHBoxLayout(this);
    // 左右预留10像素安全边距，上下撑满
    layout->setContentsMargins(10, 0, 10, 0);

    // 4. 组装 UI 控件
    layout->addWidget(m_buttonImport);
    layout->addWidget(m_buttonPlay);
    layout->addWidget(m_sliderProgress); // 将进度条放在播放按钮和时间中间
    layout->addWidget(m_labelTime);

    setLayout(layout);

    // 5. 连接 用户点击 物理信号到我们自己的槽函数
    connect(m_buttonImport, &QPushButton::clicked, this, &ControllerWidget::onImportButtonClicked);
    connect(m_buttonPlay, &QPushButton::clicked, this, &ControllerWidget::onPlayPauseButtonClicked);

    // 连接进度条的拖拽松开信号
    connect(m_sliderProgress, &QSlider::sliderReleased, this, &ControllerWidget::onSliderReleased);
}

// ================= 供 MainWindow 调用的状态更新接口 =================

/**
 * @brief 更新播放按钮显示的文本与内部状态
 */
void ControllerWidget::SetPlayState(bool is_playing) {
    // 1. 记录真实的物理状态
    m_isPlaying = is_playing;

    // 2. 更新 UI 图标/文字
    if (m_isPlaying) {
        m_buttonPlay->setText(tr("Pause"));
    } else {
        m_buttonPlay->setText(tr("Play"));
    }
}

/**
 * @brief 设置时间文本显示
 * @param current_sec 当前时间（单位：秒）
 * @param total_sec   总时长（单位：秒）
 */
void ControllerWidget::SetTimeDisplay(int64_t current_sec, int64_t total_sec) {
    // 保护性归零，防止出现负数时间
    if (current_sec < 0)
        current_sec = 0;
    if (total_sec < 0)
        total_sec = 0;

    // 将秒数转换为 分:秒 格式
    int cur_m = static_cast<int>(current_sec / 60);
    int cur_s = static_cast<int>(current_sec % 60);
    int tot_m = static_cast<int>(total_sec / 60);
    int tot_s = static_cast<int>(total_sec % 60);

    // 格式化为 00:00 / 00:00 的字符串
    QString timeStr = QString::asprintf("%02d:%02d / %02d:%02d", cur_m, cur_s, tot_m, tot_s);
    m_labelTime->setText(timeStr);
}

/**
 * @brief 更新进度条的位置
 * @param progress 当前进度百分比 [0.0, 1.0]
 */
void ControllerWidget::SetSliderProgress(float progress) {
    if (!m_sliderProgress)
        return;

    // 防御底层抢夺鼠标控制权
    // 如果用户当前正在按住滑块进行拖拽，必须完全无视底层发来的进度更新，让鼠标拥有 100% 的控制权
    if (m_sliderProgress->isSliderDown()) {
        return; 
    }

    // 计算实际应该设置的值 (0 ~ 1000)
    int value = static_cast<int>(progress * 1000.0f);

    // 如果直接 setValue，会触发 QSlider 自己的 valueChanged 信号，这可能导致死循环或者误触发 Seek 事件
    // 因此，在被动更新 UI 时，必须暂时堵住屏蔽信号
    m_sliderProgress->blockSignals(true); // 信号屏蔽
    m_sliderProgress->setValue(value);    // 修改数值
    m_sliderProgress->blockSignals(false); // 解除屏蔽
}
// ================= 内部响应 UI 物理点击的槽函数 =================

/**
 * @brief 导入按钮点击槽函数
 */
void ControllerWidget::onImportButtonClicked() {
    // 不做任何业务逻辑，只负责通知
    emit sigImportClicked();
}

/**
 * @brief 播放按钮点击槽函数
 */
void ControllerWidget::onPlayPauseButtonClicked() {
    // 根据当前记录的状态，决定执行哪个操作
    if (m_isPlaying) {
        // 当前在播放，用户点击意味着他想暂停
        emit sigPauseClicked();
    } else {
        // 当前在暂停，用户点击意味着他想播放
        emit sigPlayClicked();
    }
}

/**
 * @brief 进度条拖拽松开事件
 */
void ControllerWidget::onSliderReleased() {
    // 1. 获取用户拖拽到的物理数值 (0 ~ 1000)
    int current_val = m_sliderProgress->value();

    // 2. 转换为进度百分比 [0.0, 1.0]
    float target_progress = static_cast<float>(current_val) / 1000.0f;

    emit sigSeekRequested(target_progress);
}
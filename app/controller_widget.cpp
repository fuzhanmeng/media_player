/**
 * @file controller_widget.cpp
 * @brief 播放控制工具栏 UI 组件实现（装配子控件、布局管理与用户交互信号转发）
 * @details
 * 负责呈现底部的操作面板并解耦具体业务逻辑：
 * 1. 界面装配：组合 QPushButton（播放/暂停切换）与水平 QSlider（时间轴拖拽进度条）；
 * 2. 刻度量化：进度条采用 [0, 1000] 的千分比整型量程，彻底消除浮点数带来的计算抖动与精度误差；
 * 3. 交互事件解耦：通过 Lambda 表达式拦截底层原生 Qt 事件（clicked、sliderReleased），
 *    转换为带有清晰业务意图的自定义信号（PlayPauseClicked、Seek）向外广播；
 * 4. 状态同步接口：提供轻量的只读状态更新方法，由上层 PlayerWidget 控制其可用性与文案显示。
 */

#include "controller_widget.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>

#include "mediacore/logger.h"

/**
 * @brief 构造函数：初始化控制条子控件、配置水平布局并建立信号转发连接
 * @param parent Qt 父对象指针，生命周期由 Qt 父子对象树托管
 * @details
 * 执行步骤：
 * 1. 实例化播放/暂停按钮并初始化文本为“播放”；
 * 2. 实例化水平方向滑动条（progress_slider_），将其取值范围锁定为 [0, 1000] 的千分比刻度，
 *    并在初始未加载媒体阶段置为禁用状态；
 * 3. 使用水平布局（QHBoxLayout）将按钮与进度条由左至右排布；
 * 4. 建立 play_pause_button_ 的 clicked 信号监听，触发时通过 emit PlayPauseClicked() 通知上层；
 * 5. 建立 progress_slider_ 的 sliderReleased 信号监听，仅在用户松开滑块时读取当前刻度值并
 *    通过 emit Seek(position) 触发定位，避免在连续拖动过程中引发无意义的高频 Seek。
 */
ControllerWidget::ControllerWidget(QWidget* parent)
    : QWidget(parent),
      play_pause_button_(new QPushButton(tr("播放"), this)),
      progress_slider_(new QSlider(Qt::Horizontal, this)) {
    // 1. 配置进度条量程
    // 使用 [0, 1000] 整数刻度表达播放相对进度（千分比），规避浮点传递可能导致的精度截断误差
    progress_slider_->setRange(0, 1000);
    progress_slider_->setEnabled(false);

    // 2. 装配水平布局
    auto* layout = new QHBoxLayout(this);
    layout->addWidget(play_pause_button_);
    layout->addWidget(progress_slider_);

    // 3. 绑定播放/暂停按钮点击事件
    // QPushButton::clicked(bool checked) 自带 checked 参数，此处使用无参 Lambda 适配并隔离参数，
    // 记录日志后广播抽象的 PlayPauseClicked 信号
    connect(play_pause_button_, &QPushButton::clicked, this, [this]() {
        LOG_INFO("Play / Pause button clicked");
        emit PlayPauseClicked();
    });

    // 4. 绑定滑块释放事件（Seek 定位）
    // QSlider::sliderReleased 信号本身不携带当前刻度数值，而在拖拽定位交互中，
    // 上层只需在用户松开鼠标（释放）的瞬间获得最终目标位置；
    // 因此在 Lambda 内部提取 progress_slider_->value() 并作为整型参数随 Seek 信号发出
    connect(progress_slider_, &QSlider::sliderReleased, this, [this]() {
        const int position = progress_slider_->value();
        LOG_INFO("Progress slider released, position {}", position);
        emit Seek(position);
    });

    LOG_INFO("ControllerWidget constructed");
}

/**
 * @brief 根据当前实际播放状态更新按钮显示文本
 * @param is_playing 当前媒体是否处于正常播放中（Playing 态）
 * @details
 * 文案展示逻辑：
 * 1. is_playing 为 true 时，当前画面正在前进，按钮提示切换为“暂停”；
 * 2. is_playing 为 false 时（暂停、空闲、结束或停止），按钮提示切换为“播放”。
 */
void ControllerWidget::SetPlaying(bool is_playing) {
    play_pause_button_->setText(is_playing ? tr("暂停") : tr("播放"));
}

/**
 * @brief 设置播放/暂停按钮的可用交互状态（Enabled / Disabled）
 * @param enabled 是否允许用户点击按钮
 * @details
 * 在媒体正在打开（Opening）或底层管线执行 Seek 阶段，由于状态机处于过渡状态，
 * 外部应将其置为 false 进行置灰保护，防止重入或并发操作破坏内部状态机。
 */
void ControllerWidget::SetPlayPauseEnabled(bool enabled) {
    play_pause_button_->setEnabled(enabled);
}

/**
 * @brief 设置进度条滑块的可用交互状态
 * @param enabled 是否允许用户拖动进度条
 * @details
 * 仅在媒体已正常打开、总时长已知且媒体容器声明支持 Seek（can_seek_ 为 true）的前提下置为 true；
 * 直播流（Live Stream）或未知时长的流媒体应保持置灰禁用。
 */
void ControllerWidget::SetSeekEnabled(bool enabled) {
    progress_slider_->setEnabled(enabled);
}
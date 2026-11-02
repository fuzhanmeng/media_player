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
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>

#include <algorithm>

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
      time_label_(new QLabel(tr("--:-- / --:--"), this)),
      progress_slider_(new QSlider(Qt::Horizontal, this)) {
    // 1. 配置进度条量程
    // 使用 [0, 1000] 整数刻度表达播放相对进度（千分比），规避浮点传递可能导致的精度截断误差
    progress_slider_->setRange(0, 1000);
    progress_slider_->setEnabled(false);

    // 2. 装配水平布局
    auto* layout = new QHBoxLayout(this);
    layout->addWidget(play_pause_button_);
    layout->addWidget(time_label_);
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

    // 拖动过程中仅更新文本预览；真正的 Seek 仍只在释放滑块时发出。
    connect(progress_slider_, &QSlider::sliderMoved, this, [this](int) { UpdateTimeDisplay(); });

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

/**
 * @brief 设置播放器推算出的当前播放进度
 * @param value 归一化进度，范围为 [0, 1000]
 * @details
 * 这是播放器状态到 Qt 控件的单向同步入口。进度条只在用户松开滑块时发出
 * Seek，因此这里即使设置值也不会产生 Seek；信号屏蔽用于避免未来增加
 * valueChanged 连接后出现程序更新反向触发用户操作的问题。
 */
void ControllerWidget::SetProgress(int value) {
    const int clamped_value = std::clamp(value, 0, 1000);
    if (progress_slider_->value() == clamped_value) {
        return;
    }

    const QSignalBlocker blocker(progress_slider_);
    progress_slider_->setValue(clamped_value);
}

/**
 * @brief 更新当前播放位置和总时长，并刷新控制条上的时间文本
 * @param position_us 当前媒体位置，单位微秒
 * @param duration_us 媒体总时长，单位微秒
 * @details 播放位置由 PlayerWidget 的 100ms 定时器持续传入。若用户正拖动滑块，
 *          UpdateTimeDisplay 会优先将滑块比例换算为预览时间，避免真实播放时间覆盖预览。
 */
void ControllerWidget::SetTimeDisplay(std::int64_t position_us, std::int64_t duration_us) {
    duration_us_ = std::max<std::int64_t>(duration_us, 0);
    position_us_ = duration_us_ > 0 ? std::clamp<std::int64_t>(position_us, 0, duration_us_) : 0;
    UpdateTimeDisplay();
}

/**
 * @brief 按当前滑块状态格式化并显示“当前位置 / 总时长”
 * @details 时间精度显示到秒；超过一小时的媒体使用“小时:分钟:秒”，较短媒体使用“分钟:秒”。
 */
void ControllerWidget::UpdateTimeDisplay() {
    if (duration_us_ <= 0) {
        time_label_->setText(tr("--:-- / --:--"));
        return;
    }

    const auto format_time = [](std::int64_t time_us) {
        const std::int64_t total_seconds = std::max<std::int64_t>(time_us, 0) / 1'000'000;
        const std::int64_t hours = total_seconds / 3600;
        const std::int64_t minutes = (total_seconds / 60) % 60;
        const std::int64_t seconds = total_seconds % 60;
        if (hours > 0) {
            return QStringLiteral("%1:%2:%3")
                .arg(hours, 2, 10, QLatin1Char('0'))
                .arg(minutes, 2, 10, QLatin1Char('0'))
                .arg(seconds, 2, 10, QLatin1Char('0'));
        }
        return QStringLiteral("%1:%2")
            .arg(total_seconds / 60, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    };

    std::int64_t display_position_us = position_us_;
    if (IsSliderDown()) {
        display_position_us = static_cast<std::int64_t>(progress_slider_->value()) * duration_us_ / 1000;
    }
    display_position_us = std::clamp<std::int64_t>(display_position_us, 0, duration_us_);
    time_label_->setText(QStringLiteral("%1 / %2")
                             .arg(format_time(display_position_us), format_time(duration_us_)));
}

/**
 * @brief 返回进度条是否处于用户拖动状态
 * @return QSlider::isSliderDown() 的当前值
 */
bool ControllerWidget::IsSliderDown() const {
    return progress_slider_ != nullptr && progress_slider_->isSliderDown();
}

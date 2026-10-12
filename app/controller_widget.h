/**
 * @file controller_widget.h
 * @brief 播放控制工具栏 UI 组件定义（包含播放/暂停按钮与进度滑块）
 * @details
 * 负责呈现底部的用户交互控制条并采集操作事件：
 * 1. 架构定位：纯 GUI 表现层控件，仅负责捕获用户交互并转化为抽象 Qt 信号向外抛出；
 * 2. 依赖隔离：不直接依赖 mediacore 核心命名空间，亦不直接调用 IPlayer 控制接口，
 *    所有信号到业务逻辑的翻译与中转均由上层的 PlayerWidget 协同完成；
 * 3. 归一化刻度：进度条采用 [0, 1000] 的千分比整型量程，解耦具体媒体时长的内部换算。
 */

#pragma once

#include <QWidget>

class QPushButton;
class QSlider;

/**
 * @class ControllerWidget
 * @brief 播放控制工具栏视窗控件
 * @details 提供播放/暂停状态切换输入与基于时间轴比例的跳转定位（Seek）交互。
 */
class ControllerWidget : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 构造函数：初始化控制条及其子控件
     * @param parent Qt 父对象指针，生命周期由 Qt 父子对象树自动托管
     */
    explicit ControllerWidget(QWidget* parent = nullptr);

    /**
     * @brief 虚析构函数，保证安全释放子控件资源
     */
    ~ControllerWidget() override = default;

    Q_DISABLE_COPY_MOVE(ControllerWidget)

    /**
     * @brief 根据当前播放器实际状态更新按钮呈现（显示“播放”或“暂停”）
     * @param is_playing true 表示当前处于播放中状态，按钮切换为“暂停”提示；
     *                   false 表示当前处于非播放状态，按钮切换为“播放”提示
     */
    void SetPlaying(bool is_playing);

    /**
     * @brief 设置播放/暂停按钮的可用交互状态（Enabled / Disabled）
     * @param enabled 媒体未打开或正在执行关键状态流转（如 Seeking/Opening）时应置为 false
     */
    void SetPlayPauseEnabled(bool enabled);

    /**
     * @brief 设置进度条滑块的可用交互状态
     * @param enabled 仅在媒体总时长已知、有效且媒体流声明支持 Seek 时置为 true，否则禁用
     */
    void SetSeekEnabled(bool enabled);

signals:
    /**
     * @brief 用户点击了播放/暂停按钮时触发
     * @note 仅表达用户发起了切换意图，具体下发 Play 还是 Pause 由上层状态机裁决
     */
    void PlayPauseClicked();

    /**
     * @brief 用户完成进度条拖动并释放（或直接点击时间轴）时触发
     * @param position 归一化后的进度条位置，取值区间为 [0, 1000]（千分比刻度）
     */
    void Seek(int position);

private:
    QPushButton* play_pause_button_{nullptr};  ///< 播放/暂停控制按钮（Qt 对象树托管）
    QSlider* progress_slider_{nullptr};        ///< 时间轴定位进度条（Qt 对象树托管）
};
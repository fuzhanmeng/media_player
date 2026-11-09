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

#include <cstdint>
#include <chrono>

class QPushButton;
class QSlider;
class QLabel;

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

    /**
     * @brief 更新进度条当前值，不触发用户 Seek 信号
     * @param value 归一化进度，范围为 [0, 1000]
     * @details 播放器定时刷新时调用；使用信号屏蔽避免程序同步进度被误认为用户操作。
     */
    void SetProgress(int value);

    /**
     * @brief 更新时间轴旁的当前时间和总时长文本
     * @param position_us 播放器报告的当前位置，单位微秒
     * @param duration_us 媒体总时长，单位微秒；未知或无效时显示占位符
     * @details 用户拖动滑块时，当前时间显示滑块对应的预览时间；未拖动时显示实际播放时间。
     */
    void SetTimeDisplay(std::int64_t position_us, std::int64_t duration_us);

    /**
     * @brief 查询用户当前是否正在拖动进度条
     * @return true 表示滑块仍由用户按住拖动，刷新线程应暂时保留其预览位置
     */
    [[nodiscard]] bool IsSliderDown() const;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void UpdateTimeDisplay();

    QPushButton* play_pause_button_{nullptr};  ///< 播放/暂停控制按钮（Qt 对象树托管）
    QLabel* time_label_{nullptr};  ///< 当前播放时间与媒体总时长（Qt 对象树托管）
    QSlider* progress_slider_{nullptr};        ///< 时间轴定位进度条（Qt 对象树托管）
    std::int64_t position_us_{0};  ///< 最近一次从播放器读取的媒体位置
    std::int64_t duration_us_{0};  ///< 最近一次从播放器读取的媒体总时长
    std::uint64_t slider_move_events_{0};
    std::chrono::steady_clock::time_point last_slider_move_log_{};
};

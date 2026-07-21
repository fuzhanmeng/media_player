/**
 * @file controller_widget.h
 * @brief 底部控制面板组件 (纯净的 Dumb Component)
 * @details 承担用户的交互输入（播放、暂停、进度条拖拽、导入等）
 */
#pragma once

#include <QString>
#include <QWidget>

// 前置声明 Qt 控件
class QPushButton;
class QLabel;
class QSlider; // 【新增】进度条控件

class ControllerWidget : public QWidget {
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param parent 父级窗口
     */
    explicit ControllerWidget(QWidget* parent = nullptr);

    /**
     * @brief 析构函数
     */

    ~ControllerWidget() override = default;

    // ================= 供 MainWindow 调用的“被动”状态更新接口 =================

    /**
     * @brief 设置播放按钮的 UI 状态
     * @param is_playing 是否正在播放 (true: 按钮显示"暂停", false: 按钮显示"播放")
     */
    void SetPlayState(bool is_playing);

    /**
     * @brief 设置时间文本显示
     * @param current_time 当前时间(秒)
     * @param total_time 总时长(秒)
     */
    void SetTimeDisplay(int64_t current_time, int64_t total_time);

    /**
     * @brief 更新进度条的位置
     * @param progress 当前进度百分比 [0.0, 1.0]
     * @note 内部必须处理好信号屏蔽，防止被动更新时触发用户的 Seek 信号
     */
    void SetSliderProgress(float progress);

    /**
     * @brief 设置录制按钮的 UI 状态
     * @param is_recording true: 按钮显示"停止录制", false: 按钮显示"录制"
     */
    void SetRecordState(bool is_recording);

    // ================= 向 MainWindow 汇报用户意图的信号 =================
signals:
    /**
     * @brief 请求导入媒体文件的信号
     * @details 由导入按钮触发，交由 MainWindow 统一弹出 QFileDialog
     */
    void sigImportClicked();

    /**
     * @brief 用户点击了播放按钮
     */
    void sigPlayClicked();

    /**
     * @brief 用户点击了暂停按钮
     */
    void sigPauseClicked();

    /**
     * @brief 用户拖动松开了进度条，请求精准跳转
     * @param target_progress 目标进度的百分比 [0.0, 1.0]
     */
    void sigSeekRequested(float target_progress);

    /**
     * @brief 用户点击了录制按钮
     * @param should_start true 表示请求开始录制，false 表示请求停止录制
     */
    void sigRecordToggled(bool should_start);

private slots:
    // ================= 内部响应 UI 物理点击的槽函数 =================

    /**
     * @brief 导入按钮点击
     */
    void onImportButtonClicked();

    /**
     * @brief 播放/暂停 状态切换按钮点击
     */
    void onPlayPauseButtonClicked();

    /**
     * @brief 进度条拖拽松开事件
     */
    void onSliderReleased();

    /**
     * @brief 录制按钮点击
     */
    void onRecordButtonClicked();

private:
    /**
     * @brief 布局内部 UI 元素
     */
    void SetupUI();

private:
    // UI 控件指针 (不需要智能指针，Qt 的对象树系统会自动回收 parent 相同的组件)
    QPushButton* m_buttonImport{nullptr};
    QPushButton* m_buttonPlay{nullptr};
    QPushButton* m_buttonRecord{nullptr};
    QLabel* m_labelTime{nullptr};
    QSlider* m_sliderProgress{nullptr};

    bool m_isPlaying{false};
    bool m_isRecording{false};
};
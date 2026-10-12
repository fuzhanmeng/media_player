/**
 * @file player_widget.h
 * @brief 播放器核心复合窗口组件定义（组合渲染视窗与控制条）
 * @details
 * 负责组装 UI 表现层控件并将交互事件调度转发至 mediacore 引擎：
 * 1. 复合视图架构：内部组合 OpenGLView（YUV 渲染显示）与 ControllerWidget（播放控制与进度条）；
 * 2. 桥接与信号转发：监听控制条的 UI 交互信号（播放/暂停/拖拽跳转），翻译并下发给 IPlayer 控制接口；
 * 3. 依赖契约：仅持有 mediacore::IPlayer 的非拥有借用指针，不负责 IPlayer 的生命周期创建与销毁；
 * 4. 安全析构：在自身销毁阶段负责同步解除 IPlayer 对内部 OpenGLView 的视图绑定（DetachDisplayView）。
 */

#pragma once

#include <QString>
#include <QWidget>

#include "mediacore/iplayer.h"

class ControllerWidget;
class OpenGLView;
class QDragEnterEvent;
class QDropEvent;
class QLabel;

/**
 * @class PlayerWidget
 * @brief 组合视频画面渲染与播放控制界面的顶级播放窗口
 */
class PlayerWidget : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 构造播放器主交互窗口
     * @param player 非拥有指针：外部注入的 IPlayer 控制器实例，生命周期由调用方管理（不可为空）
     * @param parent Qt 对象树父控件指针
     */
    explicit PlayerWidget(mediacore::IPlayer* player, QWidget* parent = nullptr);

    /**
     * @brief 析构函数：解除显示端绑定并清理窗口控件
     * @note 析构时会同步调用 player_->DetachDisplayView(gl_view_)，确保在途渲染完全停止后再销毁子控件。
     */
    ~PlayerWidget() override;

    Q_DISABLE_COPY_MOVE(PlayerWidget)

    /**
     * @brief 请求打开指定的本地音视频文件，就绪后按需起播
     * @param file_path 本地媒体文件绝对或相对路径；空路径时静默忽略
     */
    void OpenMediaFile(const QString& file_path);

protected:
    /**
     * @brief 拖拽进入事件处理：检验拖入的数据是否包含有效本地文件路径
     */
    void dragEnterEvent(QDragEnterEvent* event) override;

    /**
     * @brief 拖拽释放事件处理：提取拖入的文件路径并触发播放
     */
    void dropEvent(QDropEvent* event) override;

private slots:
    /**
     * @brief 响应打开文件按钮点击：弹出系统文件选择对话框
     */
    void OnOpenFileClicked();

    /**
     * @brief 响应播放/暂停切换按钮点击：根据当前状态转发 Play() 或 Pause()
     */
    void OnPlayPauseClicked();

    /**
     * @brief 响应进度条拖拽/点击定位：将相对比例位置换算为微秒时间戳并下发 Seek
     * @param position 归一化进度条数值（区间范围 [0, 1000]）
     */
    void OnSeek(int position);

    /**
     * @brief 定时/事件驱动刷新：拉取 IPlayer 状态快照，更新按钮图标与状态文本提示
     */
    void RefreshPlaybackState();

private:
    mediacore::IPlayer* player_{nullptr};  ///< 注入的 IPlayer 借用指针（非拥有所有权）
    ControllerWidget* controller_{nullptr};  ///< 底部控制工具栏子控件（生命周期由 Qt 父子对象树托管）
    OpenGLView* gl_view_{nullptr};  ///< 硬件加速 YUV 视频渲染子控件（实现 IVideoSink 接口）
    QLabel* playback_status_label_{nullptr};  ///< 播放状态与错误诊断提示标签
    QString last_media_directory_{};  ///< 上次成功打开文件的目录缓存（优化文件选择器交互体验）
};
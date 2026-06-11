/**
 * @file main_window.h
 * @brief 播放器主窗口类头文件
 * @details 采用系统原生窗口，包含视频渲染区域和控制面板
 * 负责管理UI组件生命周期和用户交互事件转发 (事件路由机制)
 */
#pragma once

#include <QString>
#include <QWidget>
#include <memory>

// 前置声明 Qt 官方事件类
class QCloseEvent;

// 前置声明子视图组件
class PlayerWidget;
class ControllerWidget;

// 前置声明底层引擎组件
namespace av {
class IPlayer;
class IGLContext;
enum class PlaybackState; // 引入枚举结构以处理状态机的信号映射
} // namespace av

/**
 * @class MainWindow
 * @brief 播放器主窗口类
 * @details 采用标准系统原生窗口，抛弃无边框负担，聚焦音视频核心管线。
 */
class MainWindow : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param parent 父窗口指针，默认为nullptr
     */
    explicit MainWindow(QWidget* parent = nullptr);

    /**
     * @brief 析构函数
     * @details 确保所有媒体资源正确释放，监听器正确解绑
     */
    ~MainWindow() override;

    /**
     * @brief 打开媒体文件
     * @param url 媒体文件本地路径或网络URL
     * @return bool 打开成功返回true，失败返回false
     */
    bool OpenMedia(const QString& url);

    /**
     * @brief 关闭当前正在播放的媒体
     * @details 停止播放，释放解码器资源，重置所有UI状态
     */
    void CloseMedia();

protected:
    /**
     * @brief 窗口关闭事件处理函数
     * @param event 包含关闭标志的事件对象
     * @details 在窗口关闭前确保所有媒体资源正确释放
     */
    void closeEvent(QCloseEvent* event) override;

private slots:
    // ================== 接收 ControllerWidget UI 交互事件 ==================

    /**
     * @brief 响应用户导入请求
     */
    void onUserImportRequested();

    /**
     * @brief 响应用户播放请求
     */
    void onUserPlayRequested();

    /**
     * @brief 响应用户暂停请求
     */
    void onUserPauseRequested();

    /**
     * @brief 响应进度条拖拽完毕进行媒体时间轴跳转
     * @param target_progress 目标进度百分比
     */
    void onUserSeekRequested(float target_progress);

    /**
     * @brief 响应视频渲染区域的鼠标点击
     * @details 实现点击画面即可切换播放/暂停的便捷功能
     */
    void onVideoAreaClicked();

    // ================== 接收 PlayerWidget 底层状态反馈事件 ==================

    /**
     * @brief 响应播放器核心原子状态改变
     * @param state 最新的播放机状态枚举
     */
    void onEngineStateChanged(av::PlaybackState state);

    /**
     * @brief 响应播放器物理流读取完毕 (EOF)
     */
    void onEnginePlaybackCompleted();

    /**
     * @brief 响应播放器内部致命错误
     * @param error_code 错误标识码
     * @param error_msg 详细报错信息
     */
    void onEngineError(int error_code, const QString& error_msg);

private:
    /**
     * @brief 初始化UI组件
     * @details 创建所有UI控件，设置布局和样式
     */
    void SetupUI();

    /**
     * @brief 建立信号槽连接
     * @details 连接UI组件之间以及UI组件与播放器之间的信号槽
     */
    void SetupConnections();

private:
    /// 核心播放引擎智能指针 (最高层级生命周期管理)
    std::shared_ptr<av::IPlayer> m_player;

    /// 共享OpenGL上下文智能指针
    std::shared_ptr<av::IGLContext> m_sharedGLContext;

    /// 视频渲染区域控件指针 (负责画面输出与引擎状态反馈)
    PlayerWidget* m_playerWidget{nullptr};

    /// 播放控制面板控件指针 (负责UI展示与物理交互信号发送)
    ControllerWidget* m_controllerWidget{nullptr};
};
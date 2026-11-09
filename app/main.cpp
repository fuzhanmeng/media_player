/**
 * @file main.cpp
 * @brief 应用程序主入口
 * @details
 * 负责引导图形界面与播放引擎核心的生命周期：
 * 1. 日志子系统：在所有 GUI 及管线组件构造前优先初始化，并在进程退出前确保全部缓冲落盘；
 * 2. 栈析构定序（RAII Order of Destruction）：利用 C++ 作用域倒序析构机制，严格确保
 *    PlayerWidget 先于 IPlayer 销毁，从而在 IPlayer 被释放前安全完成视频渲染终端解绑（DetachDisplayView）；
 * 3. 命令行参数驱动：支持通过 CLI 传参实现启动时即刻打开指定媒体文件。
 */

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QStringList>
#include <QtGlobal>
#include <memory>

#include "mediacore/logger.h"
#include "mediacore/av_types/hardware_frame.h"
#include "mediacore/player_factory.h"
#include "player_widget.h"

/**
 * @brief 程序主执行入口：初始化运行时环境、挂载界面并驱动 Qt 事件循环
 * @param argc 命令行参数总数量（透传至 QApplication 以支持平台内置标志，如 -platform offscreen 等）
 * @param argv 命令行参数字符指针数组
 * @return 进程退出码；正常退出返回 QApplication::exec() 的返回值，核心创建失败返回 1
 * @details
 * 执行步骤与安全设计：
 * 1. 引导全局日志：调用 Logger::Init()，确保后续所有模块加载期日志可追溯；
 * 2. 显式局部作用域包围（Block Scope）：
 *    在 Logger::Shutdown() 之前使用花括号限制所有 Qt 控件与智能指针生命周期，
 *    确保当退出事件循环时，全部 QWidget、OpenGL 显存资源、工作线程与管线句柄已被彻底释放，
 *    杜绝退出时因日志器提前注销引发的访问冲突（Static Destruction Order Fiasco）；
 * 3. 栈对象生命周期定序：
 *    - 先声明 player（std::unique_ptr<IPlayer>）；
 *    - 后声明 window（PlayerWidget）；
 *    - 退出作用域时，遵循 C++ 栈对象构造与析构严格镜像原则（后进先出 LIFO），
 *      window 先行调用析构函数（执行 Detach 并回收子控件），player 随后才触发析构与管线终止，
 *      完全消除了野指针与在途线程崩溃风险。
 */
int main(int argc, char* argv[]) {
    // 1. 初始化全局日志系统（控制台输出与异步日志文件）
    mediacore::Logger::Init();

    int exit_code = 0;
    {
        // 2. 选择 Qt 的 xcb OpenGL 后端，再启动 Qt 应用环境
        // DMA-BUF → EGLImage 零拷贝要求 QOpenGLWidget 运行在 EGL 上下文里（同一个 EGLDisplay 才能导入 DMA-BUF）；
        // Qt 的 xcb 平台默认走 GLX，必须用 QT_XCB_GL_INTEGRATION=xcb_egl 切换到 EGL 后端。
        // 这个环境变量必须在 QApplication 构造之前设置，之后改无效。
        // 只在用户没有显式设置时才填默认值：用户设了就尊重用户（例如想强制回到 GLX 做对比）。
        // 切到 EGL 后端不会改变软件帧渲染；EGL 不可用时 OpenGLView 会自动退回软件下载路径。
        if (!qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION")) {
            qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
        }

        // QApplication 负责捕获与分发底层窗口系统的原生输入、重绘和定时器事件
        QApplication app(argc, argv);
        LOG_INFO("QApplication created: Qt compile {}, runtime {}, QPA platform {}",
                 QT_VERSION_STR,
                 qVersion(),
                 QGuiApplication::platformName().toStdString());

        QCommandLineParser parser;
        parser.addHelpOption();
        const QCommandLineOption decoder_mode_option(
            "video-decoder", "Select the video decoder mode (auto, software, or vaapi).", "mode", "auto");
        parser.addOption(decoder_mode_option);
        parser.addPositionalArgument("media-file", "Media file to open.", "[media-file]");
        parser.process(app);

        mediacore::PlayerConfig player_config = mediacore::PlayerConfig::Default();
        const QString decoder_mode = parser.value(decoder_mode_option).trimmed().toLower();
        if (decoder_mode == "software") {
            player_config.decode_.hw_accel_ = mediacore::HardwareAccel::kOff;
        } else if (decoder_mode == "vaapi") {
            player_config.decode_.hw_accel_ = mediacore::HardwareAccel::kVaapi;
        } else if (decoder_mode != "auto") {
            LOG_ERROR("Unsupported --video-decoder mode '{}'; expected auto, software, or vaapi", decoder_mode.toStdString());
            exit_code = 2;
        }

        if (parser.positionalArguments().size() > 1) {
            LOG_ERROR("Only one media file may be specified on the command line");
            exit_code = 2;
        }

        // 3. 借助抽象工厂创建 IPlayer 控制核心实例
        // 返回 std::unique_ptr 独占指针，掌控媒体引擎主生命周期
        std::unique_ptr<mediacore::IPlayer> player;
        if (exit_code == 0) {
            player = mediacore::CreatePlayer(std::move(player_config));
        }
        if (!player) {
            LOG_ERROR("Player creation failed");
            exit_code = 1;
        } else {
            LOG_INFO("Player instance acquired");
        }

        // 4. 实例化主交互视窗并注入播放控制器借用指针
        // 重要契约：window 声明在 player 之后，保证析构时 window 优先释放并解除所有弱引用
        PlayerWidget window(player.get());
        window.setWindowTitle("media_player");
        window.resize(1920, 1080);
        window.show();
        LOG_INFO("Main window shown");

        // 5. 命令行启动参数解析
        // 若启动命令携带媒体文件路径，自动直接起播
        if (player && !parser.positionalArguments().isEmpty()) {
            const QString media_path = parser.positionalArguments().front();
            LOG_INFO("Opening file from command line argument: {}", media_path.toStdString());
            window.OpenMediaFile(media_path);
        }

        // 6. 移交主控制权，阻塞进入 Qt 事件主循环
        LOG_INFO("Qt event loop started");
        if (exit_code == 0) {
            exit_code = app.exec();
            LOG_INFO("Qt event loop finished with code {}", exit_code);
        }
    }

    // 7. 关闭日志引擎，刷新未落盘缓冲并注销资源
    const auto releases = mediacore::GetHardwareFrameReleaseStats();
    LOG_INFO("[HardwareFrame][ReleaseSummary] scope=process owned_fd_close_success={} "
             "owned_fd_close_failed={} av_frame_refs_released={}",
             releases.fd_close_success, releases.fd_close_failed, releases.av_frame_refs_released);
    LOG_INFO("Application exited with code {}", exit_code);
    mediacore::Logger::Shutdown();

    return exit_code;
}

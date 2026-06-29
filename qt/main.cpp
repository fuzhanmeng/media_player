/**
 * @file main.cpp
 * @brief 程序的入口
 */
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <atomic>
#include <csignal>

#include "logger.h"
#include "main_window.h"
#include "playback_state.h"

// 全局标志，用于优雅退出
static std::atomic<bool> g_should_exit = false;

// 信号处理函数，捕获Ctrl+C
static void SignalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        // 信号处理函数中只能调用异步安全的函数
        // 先设置原子标志，然后通过管道或事件通知Qt主线程
        // 这里简化处理：QCoreApplication::exit()在Qt5/6中是线程安全的
        g_should_exit = true;

        // 注意：在信号处理函数中不要调用任何日志函数！
        // 日志函数可能会分配内存或持有锁，在信号处理中是不安全的

        // 通知Qt事件循环退出
        QCoreApplication::exit(0);
    }
}

// 静态回调函数，用来接收来自QT模块的日志
static void QtLogInterceptor(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    // 1. 把 Qt 的 QString 转成标准的 std::string
    std::string localMsg = msg.toStdString();

    // 2. 提取 Qt 日志发生的文件名、行号和函数名
    const char *file = context.file ? context.file : "Qt_Internal";
    const char *function = context.function ? context.function : "Unknown";
    int line = context.line;

    // 3. 组装成 spdlog 能看懂的上下文位置对象
    spdlog::source_loc loc{file, line, function};

    // 4. 根据 Qt 的日志级别，通过 loc 精准转发给我们自己的 spdlog 引擎
    // 注意：此处必须显式调用底层 log() 并传入 loc，若直接使用宏会丢失 Qt 原始的文件和行号信息
    switch (type) {
        case QtDebugMsg:
            av::Logger::GetCoreLogger()->log(loc, spdlog::level::debug, "[Qt] {}", localMsg);
            break;
        case QtInfoMsg:
            av::Logger::GetCoreLogger()->log(loc, spdlog::level::info, "[Qt] {}", localMsg);
            break;
        case QtWarningMsg:
            av::Logger::GetCoreLogger()->log(loc, spdlog::level::warn, "[Qt] {}", localMsg);
            break;
        case QtCriticalMsg:
            av::Logger::GetCoreLogger()->log(loc, spdlog::level::err, "[Qt] {}", localMsg);
            break;
        case QtFatalMsg:
            av::Logger::GetCoreLogger()->log(loc, spdlog::level::critical, "[Qt] {}", localMsg);
            break;
    }
}

int main(int argc, char *argv[]) {
    // 第一步：注册信号处理函数（必须在最前面）
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    try {
        // 1. 初始化日志系统
        av::Logger::Init();

        // 2. 把刚刚写好的拦截器函数注册给 Qt
        qInstallMessageHandler(QtLogInterceptor);

        LOG_INFO("======================================");
        LOG_INFO(" AI Media Player Starting... ");
        LOG_INFO(" Version: 1.0.0 ");
        LOG_INFO(" Build: {} {}", __DATE__, __TIME__);
        LOG_INFO(" QtLogInterceptor Starting ");
        LOG_INFO("======================================");

        // 3. Qt 全局配置
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

        // 【新增】使用整数缩放因子，避免界面模糊
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

        // 应用程序元信息
        QCoreApplication::setApplicationName("AI Media Player");
        QCoreApplication::setOrganizationName("AVPlayer");
        QCoreApplication::setApplicationVersion("1.0.0");

        // 4. 配置全局默认的 OpenGL Surface 格式
        QSurfaceFormat fmt;
        fmt.setDepthBufferSize(24); // 24位深度缓冲区
        fmt.setStencilBufferSize(8); // 8位模板缓冲区，几乎无性能开销
        fmt.setVersion(3, 3); // 强制使用 OpenGL 3.3
        fmt.setProfile(QSurfaceFormat::CoreProfile); // 设置默认格式

        // 新增双缓冲（默认开启，显式声明更清晰）
        fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        // 新增垂直同步，避免画面撕裂
        fmt.setSwapInterval(1);
        // 新增请求RGBA8888颜色格式，保证颜色精度
        fmt.setRedBufferSize(8);
        fmt.setGreenBufferSize(8);
        fmt.setBlueBufferSize(8);
        fmt.setAlphaBufferSize(8);

        // 将配置好的格式设置为 Qt 的全局默认格式
        QSurfaceFormat::setDefaultFormat(fmt);

        // 5. 创建应用程序对象
        QApplication a(argc, argv);

        // 注册自定义枚举为 Qt 元类型：状态变更信号可能从后台线程（如 Seek 完成后由解复用线程触发）
        // 发出，跨线程 queued connection 要求参数类型已注册，否则事件被静默丢弃
        qRegisterMetaType<av::PlaybackState>("av::PlaybackState");

        // 新增命令行参数解析
        QCommandLineParser parser;
        parser.setApplicationDescription("AI Media Player - A professional media player");
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addPositionalArgument("file", "Media file to play", "[file]");

        parser.process(a);

        QString mediaFile;
        QStringList args = parser.positionalArguments();
        if (!args.isEmpty()) {
            mediaFile = args.first();
            LOG_INFO("Command line media file: {}", mediaFile.toStdString());
        }

        // 6. 创建主窗口对象
        MainWindow main_window;

        // 【新增】如果有命令行参数，自动打开文件
        if (!mediaFile.isEmpty()) {
            // 这里需要MainWindow提供一个openFile的公共接口
            // 你可以在MainWindow类中添加：
            // public slots:
            //     void openFile(const QString& filePath);
            QMetaObject::invokeMethod(&main_window, "openFile", Qt::QueuedConnection, Q_ARG(QString, mediaFile));
        }

        main_window.show(); // 显示主窗口

        LOG_INFO("Application event loop started...");

        // 7. 进入应用程序事件循环
        int exitCode = a.exec();

        LOG_INFO("======================================");
        LOG_INFO(" AI Media Player Exiting normally ");
        LOG_INFO(" Exit Code: {}", exitCode);
        LOG_INFO("======================================");

        // 8. 清理全局资源
        // 说明：鉴于目前提供的 logger.h 中尚未声明 Shutdown 静态函数，此处直接调用 spdlog 原生全局清理
        spdlog::shutdown();

        return exitCode;
    } catch (const std::exception &e) {
        // 捕获所有标准异常
        LOG_FATAL("Fatal uncaught exception: {}", e.what());

        // 显示错误对话框给用户
        QMessageBox::critical(nullptr, "Fatal Error",
                              QString("Application encountered a fatal error:\n%1\n\n"
                                      "Please check the log file for more details.")
                                  .arg(e.what()));

        // 确保日志被刷新
        spdlog::shutdown();

        return 1;
    } catch (...) {
        // 捕获所有未知异常
        LOG_FATAL("Fatal unknown uncaught exception");

        QMessageBox::critical(nullptr, "Fatal Error",
                              "Application encountered an unknown fatal error.\n\n"
                              "Please check the log file for more details.");

        spdlog::shutdown();

        return 1;
    }
}
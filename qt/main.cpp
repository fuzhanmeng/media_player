/**
 * @file main.cpp
 * @brief 程序的入口
 */
#include <QApplication>
#include <QSurfaceFormat>

#include "logger.h"
#include "main_window.h"

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

    // 4. 根据 Qt 的日志级别，转发给我们自己的 spdlog 引擎
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
            // 如果是致命错误，Qt 马上就会崩溃，所以这里顺其自然
            break;
    }
}

int main(int argc, char *argv[]) {
    // 1. 初始化
    av::Logger::Init();

    // 2. 把刚刚写好的拦截器函数注册给 Qt
    qInstallMessageHandler(QtLogInterceptor);

    LOG_INFO("======================================");
    LOG_INFO(" AI Media Player Starting... ");
    LOG_INFO(" QtLogInterceptor Starting ");
    LOG_INFO("======================================");
    // 允许所有 QOpenGLWidget 之间共享 OpenGL 上下文
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // 配置全局默认的 OpenGL Surface 格式
    QSurfaceFormat fmt;
    fmt.setDepthBufferSize(24); // 24位深度缓冲区
    fmt.setVersion(3, 3); // 强制使用 OpenGL 3.3
    fmt.setProfile(QSurfaceFormat::CoreProfile); // 设置默认格式

    QApplication a(argc, argv); // 创建QApplication对象

    MainWindow main_window; // 创建主窗口对象
    main_window.show(); // 显示主窗口

    return a.exec(); // 进入应用程序事件循环
}
/**
 * @file main
 * @brief 程序的入口。负责初始化 Qt 环境，配置全局的 OpenGL 参数，并启动主窗口事件循环
 */
#include <QApplication>
#include <QSurfaceFormat>

#include "main_window.h"

int main(int argc, char* argv[]) {
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
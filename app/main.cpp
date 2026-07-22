/**
 * @file main.cpp
 * @brief 应用层入口：创建 Qt 事件循环并弹出主窗口空壳（D1 黑框验证）。
 * @details 本文件仅用于验证 CMake + Conan + Qt 工具链完全打通。
 *          当前主窗口为最朴素的 QWidget，D3 起将替换为自定义 MainWindow。
 * @author  xm
 * @date    2026-07
 */

#include <QApplication>
#include <QWidget>

/**
 * @file main.cpp
 * @brief 程序主入口 构建 QT 应用并进入事件循环
 * @param argc 命令行参数个数，透传给 QAppliaction 以支持 Qt 内置命令行开关
 * @param argv 命令行参数数组，透传给 QApplication
 * @return int 进程退出码， 由 Qt 事件循环 QApplication::exec() 返回
 */
int main(int argc, char* argv[]) {
    
    // QApplication 承载 Qt 事件循环，必须最先创建且全程序唯一；
    // 它接管 argc/argv 并负责 GUI 事件（鼠标、键盘、重绘）的分发。
    QApplication app(argc, argv);

    // 创建一个顶层窗口
    QWidget window;
    window.setWindowTitle("ai_media_player");
    window.resize(1920, 1080);
    window.show();

    // 启动事件循环
    return app.exec();
}
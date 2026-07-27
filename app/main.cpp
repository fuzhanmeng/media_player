/**
 * @file main.cpp
 * @brief 应用层入口：创建 Qt 事件循环并弹出主窗口空壳（D1 黑框验证）。
 * @details 本文件仅用于验证 CMake + Conan + Qt 工具链完全打通
 * @author  xm
 * @date    2026-07
 */


#include <QApplication>

#include "player_widget.h"
#include "mediacore/player_factory.h"

/**
 * @brief 程序主入口 构建 QT 应用并进入事件循环
 * @param argc 命令行参数个数，透传给 QAppliaction 以支持 Qt 内置命令行开关
 * @param argv 命令行参数数组，透传给 QApplication
 * @return int 进程退出码， 由 Qt 事件循环 QApplication::exec() 返回
 */
int main(int argc, char* argv[]) {
    
    // QApplication 承载 Qt 事件循环，必须最先创建且全程序唯一；
    // 它接管 argc/argv 并负责 GUI 事件（鼠标、键盘、重绘）的分发
    QApplication app(argc, argv);


    // 通过工厂函数创建 Player 实例
    std::unique_ptr<mediacore::IPlayer> player = mediacore::CreatePlayer();

    // 声明顺序即析构逆序：player 先于 window 声明 → window 先析构（触发 Detach），
    // player 后析构，保证 PlayerWidget 解绑渲染出口时 player 仍存活，避免悬空。

    // 创建顶层窗口，并把 player 注入：PlayerWidget 内部据此调用 AttachDisplayView，
    // 把渲染画布（IVideoSink）绑定到播放器。注意首参是 IPlayer*，非 parent。
    PlayerWidget window(player.get());
    window.setWindowTitle("ai_media_player");
    window.resize(1920, 1080);
    window.show();

    player->Play();

    // 启动事件循环
    return app.exec();
}
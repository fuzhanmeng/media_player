/**
 * @file main.cpp
 * @brief 应用层入口
 * @details Logger 在 QApplication 之前初始化，确保启动阶段也有日志；退出时在媒体对象析构后关闭日志
 */

#include <QApplication>

#include "mediacore/logger.h"
#include "mediacore/player_factory.h"
#include "player_widget.h"

/**
 * @brief 程序主入口，构建 Qt 应用并进入事件循环
 * @param argc 命令行参数个数，透传给 QApplication 以支持 Qt 内置命令行开关
 * @param argv 命令行参数数组，透传给 QApplication
 * @return int 进程退出码，由 Qt 事件循环 QApplication::exec() 返回
 */
int main(int argc, char* argv[]) {
    // 初始化日志
    mediacore::Logger::Init();

    int exit_code = 0;
    {
        // QApplication 承载 Qt 事件循环，必须最先创建且全程序唯一；
        // 它接管 argc/argv 并负责 GUI 事件（鼠标、键盘、重绘）的分发
        QApplication app(argc, argv);
        LOG_INFO("QApplication created");

        // 通过工厂函数创建 Player 实例
        std::unique_ptr<mediacore::IPlayer> player = mediacore::CreatePlayer();
        if (!player) {
            LOG_ERROR("Player creation failed");
            exit_code = 1;
        } else {
            LOG_INFO("Player instance acquired");
        }

        // 声明顺序即析构逆序：player 先于 window 声明，因此 window 先析构（触发 Detach）
        // player 后析构，保证 PlayerWidget 解绑渲染出口时 player 仍存活，避免悬空指针

        // 创建顶层窗口，并把 player 注入：PlayerWidget 内部据此调用
        // AttachDisplayView，把渲染画布（IVideoSink）绑定到播放器
        PlayerWidget window(player.get());
        window.setWindowTitle("ai_media_player");
        window.resize(1920, 1080);
        window.show();

        LOG_INFO("Main window shown");

        if (player) {
            LOG_INFO("Initial playback request sent");
            player->Play();
        } else {
            LOG_WARN("Initial playback request skipped because player is null");
        }

        // 启动事件循环
        LOG_INFO("Qt event loop started");
        exit_code = app.exec();
    }

    LOG_INFO("Application exited with code {}", exit_code);
    mediacore::Logger::Shutdown();
    return exit_code;
}

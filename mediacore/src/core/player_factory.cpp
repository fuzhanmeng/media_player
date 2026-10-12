/**
 * @file player_factory.cpp
 * @brief 播放器实例工厂函数实现
 * @details
 * 负责实例化具体的 Player 核心实现，并向上层（如 GUI、CLI 工具）返回抽象的 IPlayer 接口指针：
 * 1. 接口与实现解耦：上层调用者仅依赖 mediacore/iplayer.h，不直接感知内部 Pipeline、Node 及 FFmpeg 等底层细节；
 * 2. 异常隔离：内部捕获构造期间可能出现的 std::bad_alloc 或其他标准异常，失败时记录日志并安全返回 nullptr。
 */

#include "mediacore/player_factory.h"

#include <exception>
#include <memory>

#include "core/player.h"
#include "mediacore/iplayer.h"
#include "mediacore/logger.h"

namespace mediacore {

/**
 * @brief 创建并返回一个全新的播放器实例
 * @return 成功返回由 std::unique_ptr 托管的 IPlayer 抽象接口，失败返回 nullptr
 */
std::unique_ptr<IPlayer> CreatePlayer() {
    LOG_INFO("Creating player instance");
    try {
        auto player = std::make_unique<Player>();
        LOG_INFO("Player instance created successfully: {}", static_cast<const void*>(player.get()));
        return player;
    } catch (const std::exception& error) {
        LOG_ERROR("CreatePlayer failed due to exception: {}", error.what());
    } catch (...) {
        LOG_ERROR("CreatePlayer failed due to an unknown exception");
    }

    return nullptr;
}

}  // namespace mediacore
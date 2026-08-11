/**
 * @file player_factory.cpp
 * @brief 工厂函数，返回一个 IPlayer 接口
 */
#include "mediacore/player_factory.h"

#include <exception>

#include "mediacore/iplayer.h"
#include "mediacore/logger.h"
#include "stub_player.h"

namespace mediacore {

std::unique_ptr<IPlayer> CreatePlayer() {
    LOG_INFO("Creating player instance");
    try {
        auto player = std::make_unique<StubPlayer>();
        LOG_INFO("Player instance created: {}", static_cast<const void*>(player.get()));
        return player;
    } catch (const std::exception& error) {
        LOG_ERROR("CreatePlayer failed: {}", error.what());
    } catch (...) {
        LOG_ERROR("CreatePlayer failed with unknown exception");
    }

    return nullptr;
}

} // namespace mediacore

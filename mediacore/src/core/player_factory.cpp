/**
 * @file player_factory.cpp
 * @brief 工厂函数，返回一个 IPlayer 接口
 */
#include "mediacore/player_factory.h"

#include "mediacore/iplayer.h"
#include "stub_player.h"

namespace mediacore {

/**
 * @brief Description
 */

std::unique_ptr<IPlayer> CreatePlayer() { return std::make_unique<StubPlayer>(); }

} // namespace mediacore

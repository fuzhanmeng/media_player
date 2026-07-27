/**
 * @file player_factory.h
 * @brief IPlayer 工厂,向 UI 返回 IPlayer 实例
 */

#pragma once

#include <memory>

#include "iplayer.h"

namespace mediacore {

/**
 * @brief 创建一个播放器实例
 * @return 持有 IPlayer 的 unique_ptr,独占所有权 调用方负责生命周期
 * @note 返回的是抽象接口而非具体类型
 */
std::unique_ptr<IPlayer> CreatePlayer();
} // namespace mediacore

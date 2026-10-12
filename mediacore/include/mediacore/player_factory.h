/**
 * @file player_factory.h
 * @brief 播放器实例工厂接口定义
 * @details
 * 对外暴露的顶层工厂接口，承担抽象工厂角色：
 * 1. 依赖隔离：UI 层（如 Qt 窗口）或上层业务仅通过此工厂获取 IPlayer 抽象接口，
 *    完全隔离底层 Pipeline、DAG 节点拓扑及 FFmpeg 等具体实现类型；
 * 2. 所有权移交：返回 std::unique_ptr<IPlayer>，调用方独占该播放器实例的生命周期管理；
 * 3. 异常安全：工厂内部已对内存分配及构建阶段的标准异常进行了隔离捕获，失败时安全返回 nullptr。
 */

#pragma once

#include <memory>

#include "mediacore/iplayer.h"

namespace mediacore {

/**
 * @brief 创建并初始化一个全新的播放器实例
 * @return 成功返回由 std::unique_ptr 独占管理的 IPlayer 抽象接口指针；
 *         内部初始化或内存分配失败时返回 nullptr
 * @note 调用方接管返回实例的所有权，其析构操作将触发底层 Pipeline 与关联后台线程的彻底清理。
 */
std::unique_ptr<IPlayer> CreatePlayer();

}  // namespace mediacore
/**
 * @file iseek_hold_releasable.h
 * @brief 解除 Seek 定位后终端节点画面定格（Seek Hold）挂起状态的抽象接口
 * @details
 * 负责建立控制层（Player / Pipeline）与终端渲染节点（VideoSinkNode）之间的定格握手机制：
 * 1. 机制背景：精准 Seek 命中首张目标呈现帧并交付上屏后，终端节点会主动挂起自身工作线程（Hold），
 *    防止在 Player 判定是否继续播放前过早向屏幕吐出后续帧导致画面偷跑；
 * 2. 握手放行：Player 控制线程在处理完 SeekCompleted 事件、确认保持暂停或恢复播放后，
 *    通过本接口跨线程解除终端节点的挂起状态；
 * 3. 并发契约：本接口方法必须支持跨线程安全调用，且实现应保持轻量非阻塞（仅置位标记并唤醒条件变量）
 */

#pragma once

namespace mediacore {

/**
 * @class ISeekHoldReleasable
 * @brief 可解除 Seek 目标帧交付后挂起阻塞的终端节点抽象接口
 * @note 派生类（如 VideoSinkNode）通过实现本接口，供 Pipeline 统一协调画面定格的解除
 */
class ISeekHoldReleasable {
public:
    /**
     * @brief 默认构造函数
     */
    ISeekHoldReleasable() = default;

    /**
     * @brief 虚析构函数，保证多态派生类的安全释放
     */
    virtual ~ISeekHoldReleasable() = default;

    // 严禁拷贝与移动，防止抽象接口产生对象切片（Object Slicing）
    ISeekHoldReleasable(const ISeekHoldReleasable&) = delete;
    ISeekHoldReleasable& operator=(const ISeekHoldReleasable&) = delete;
    ISeekHoldReleasable(ISeekHoldReleasable&&) = delete;
    ISeekHoldReleasable& operator=(ISeekHoldReleasable&&) = delete;

    /**
     * @brief 跨线程解除当前终端节点的 Seek 挂起状态
     * @note
     * 1. 必须保证 noexcept，且严禁执行长时阻塞操作；
     * 2. 调用后应即刻唤醒终端节点的工作线程，使其恢复从输入队列消费后续视频帧。
     */
    virtual void ReleaseSeekHold() noexcept = 0;
};

}  // namespace mediacore
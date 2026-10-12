/**
 * @file iseek_source.h
 * @brief 可响应 Seek 跳转请求的数据源节点抽象接口
 * @details
 * 负责打通控制层（Pipeline / Player）到底层解封装源节点（如 FfmpegDemuxerNode）的时间跳转通道：
 * 1. 职责边界：纯虚控制接口，仅负责异步登记跳转请求与唤醒工作线程，不直接阻塞执行底层 I/O 定位；
 * 2. 线程契约：由外部控制线程调用，内部实现必须保证非阻塞（立即返回），避免拖拽进度条时阻塞调用方；
 * 3. 世代隔离：每次请求均绑定新分配的 epoch 版本号，促使源节点在底层完成定位后正确广播 StreamReset。
 */

#pragma once

#include <cstdint>

namespace mediacore {

/**
 * @class ISeekSource
 * @brief 可响应时间跳转（Seek）请求的源节点接口
 * @note 派生类（如 FfmpegDemuxerNode）通过实现本接口，接管管线的跳转调度。
 */
class ISeekSource {
public:
    /**
     * @brief 默认构造函数
     */
    ISeekSource() = default;

    /**
     * @brief 虚析构函数，保证多态派生类的安全释放
     */
    virtual ~ISeekSource() = default;

    // 严禁拷贝与移动，防止抽象接口产生对象切片（Object Slicing）
    ISeekSource(const ISeekSource&) = delete;
    ISeekSource& operator=(const ISeekSource&) = delete;
    ISeekSource(ISeekSource&&) = delete;
    ISeekSource& operator=(ISeekSource&&) = delete;

    /**
     * @brief 异步向源节点提交时间跳转请求
     * @param epoch Pipeline 为本次 Seek 分配的全局唯一媒体流世代版本号
     * @param target_us 目标时间戳（单位：微秒，正数或零）
     * @note
     * 本函数必须以非阻塞方式快速返回（例如仅更新 pending_seek_ 并触发中断），
     * 耗时的底层文件/网络定位必须交由源节点的工作线程执行。
     */
    virtual void RequestSeek(std::uint64_t epoch, std::int64_t target_us) = 0;
};

}  // namespace mediacore
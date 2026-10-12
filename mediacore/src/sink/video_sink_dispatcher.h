/**
 * @file video_sink_dispatcher.h
 * @brief 在 VideoSinkNode 与外部 IVideoSink 之间安全转发视频帧的中介分发器
 * @details
 * 负责解耦视频渲染节点与外部 UI 显示端（IVideoSink*）：
 * 1. 跨线程分发：VideoSinkNode 在渲染工作线程调用 DeliverFrame() 同步投递帧；
 *    UI / 控制线程通过 Attach() / Detach() 动态挂载或卸载目标显示端；
 * 2. 弱引用生命周期安全（In-flight Tracking）：
 *    本类不持有 IVideoSink 的强所有权。Detach() 与 Attach() 切换时，会先断开新帧投递，
 *    随后挂起阻塞等待所有已进入用户 Sink 回调的在途线程全部执行完毕；
 *    一旦返回，调用方即可 100% 安全地销毁该 IVideoSink 实例（彻底杜绝 Use-After-Free）；
 * 3. 死锁防御（Reentrancy Guard）：
 *    利用基于 thread_local 链表的 RAII 作用域追踪，在运行期严密拦截在 OnReceiveVideoFrame
 *    内部反向调用 Attach/Detach 导致的自等待死锁。
 */

#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "mediacore/ivideo_sink.h"
#include "mediacore/logger.h"

namespace mediacore {

/**
 * @class VideoSinkDispatcher
 * @brief 为非拥有的 IVideoSink* 提供安全的同步绑定、解绑及低延迟帧投递
 * @note
 * 1. 外部在析构销毁 IVideoSink 对象前，必须显式调用 Detach(sink) 并等待其返回；
 * 2. Attach 与 Detach 的并发调用应由调用方在控制层（如 Player）保证串行时序；
 * 3. 严禁在 IVideoSink::OnReceiveVideoFrame 回调内同步反向调用 Attach 或 Detach。
 */
class VideoSinkDispatcher final {
public:
    /**
     * @brief 默认构造函数
     */
    VideoSinkDispatcher() = default;

    /**
     * @brief 析构函数：释放分发器
     */
    ~VideoSinkDispatcher() = default;

    // 禁用拷贝与移动，保证互斥锁与回调追踪容器生命周期唯一
    VideoSinkDispatcher(const VideoSinkDispatcher&) = delete;
    VideoSinkDispatcher& operator=(const VideoSinkDispatcher&) = delete;
    VideoSinkDispatcher(VideoSinkDispatcher&&) = delete;
    VideoSinkDispatcher& operator=(VideoSinkDispatcher&&) = delete;

    /**
     * @brief 挂载绑定新的外部显示端
     * @param sink 外部拥有完整生命周期的有效显示端指针，不可为空
     * @throws std::invalid_argument 当 sink 为 nullptr 时抛出
     * @throws std::logic_error 当试图在 OnReceiveVideoFrame 回调中重入调用时抛出
     * @details 若此前已绑定旧显示端，会先断开投递并阻塞等待旧显示端中所有正在执行的回调全部返回后再切换。
     */
    void Attach(IVideoSink* sink) {
        // 1. 检查并拦截回调内部的致命重入
        RejectCallbackReentrancy();

        // 2. 基础入参校验
        if (sink == nullptr) {
            LOG_ERROR("VideoSinkDispatcher::Attach requires a non-null IVideoSink");
            throw std::invalid_argument("VideoSinkDispatcher requires a non-null IVideoSink");
        }

        // 3. 若目标 Sink 与当前已绑定的完全一致，直接幂等返回
        std::unique_lock<std::mutex> lock(mutex_);
        if (sink_ == sink) {
            return;
        }

        // 4. 断开当前旧显示端的新帧入口
        IVideoSink* const previous = sink_;
        sink_ = nullptr;

        // 5. 释放对旧 Sink 的投递，并阻塞等待已进入其回调的在途线程全部归零
        WaitForNoInFlight(lock, previous);

        // 6. 挂载并启用新显示端
        sink_ = sink;
    }

    /**
     * @brief 解绑指定的显示端，并阻塞等待其所有正在执行的在途回调安全退出
     * @param sink 待解绑的目标显示端指针；若为 nullptr 则直接跳过
     * @throws std::logic_error 当试图在 OnReceiveVideoFrame 回调中重入调用时抛出
     */
    void Detach(IVideoSink* sink) {
        // 1. 检查并拦截回调重入
        RejectCallbackReentrancy();

        if (sink == nullptr) {
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);

        // 2. 若待解绑目标恰好为当前活跃 Sink，立即置空断开新帧投递
        if (sink_ == sink) {
            sink_ = nullptr;
        }

        // 3. 阻塞等待该 Sink 的所有在途回调执行完毕
        WaitForNoInFlight(lock, sink);
    }

    /**
     * @brief 同步投递一帧视频数据至当前绑定的显示端
     * @param frame 待交付的共享视频帧句柄；若当前未挂载 Sink 或为 nullptr 则直接丢弃
     */
    void DeliverFrame(std::shared_ptr<VideoFrame> frame) {
        if (frame == nullptr) {
            return;
        }

        IVideoSink* target_sink = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target_sink = sink_;

            // 当前未连接任何显示端，安全静默丢弃
            if (target_sink == nullptr) {
                return;
            }

            // 登记在途调用计数
            ++in_flight_callbacks_[target_sink];
        }

        // RAII 标记：在当前线程调用栈中压入本 Dispatcher 的执行范围，用于检测并阻止回调重入
        DeliveryCallbackScope callback_scope(this);

        try {
            target_sink->OnReceiveVideoFrame(std::move(frame));
        } catch (...) {
            // 异常安全保证：即使用户实现抛出异常，也必须扣减在途计数并唤醒等待的 Detach/Attach
            FinishDelivery(target_sink);
            throw;
        }

        FinishDelivery(target_sink);
    }

private:
    /**
     * @class DeliveryCallbackScope
     * @brief 基于 thread_local 链表的 RAII 作用域守卫，记录当前线程正在执行的回调上下文
     */
    class DeliveryCallbackScope final {
    public:
        explicit DeliveryCallbackScope(const VideoSinkDispatcher* dispatcher) noexcept
            : video_dispatcher_(dispatcher), outer_scope_(thread_top_scope) {
            thread_top_scope = this;
        }

        ~DeliveryCallbackScope() { thread_top_scope = outer_scope_; }

        DeliveryCallbackScope(const DeliveryCallbackScope&) = delete;
        DeliveryCallbackScope& operator=(const DeliveryCallbackScope&) = delete;

        /**
         * @brief 检查当前调用栈中是否已存在指定 Dispatcher 的帧回调
         */
        static bool Contains(const VideoSinkDispatcher* dispatcher) noexcept {
            for (const DeliveryCallbackScope* scope = thread_top_scope; scope != nullptr; scope = scope->outer_scope_) {
                if (scope->video_dispatcher_ == dispatcher) {
                    return true;
                }
            }
            return false;
        }

    private:
        inline static thread_local DeliveryCallbackScope* thread_top_scope{nullptr};
        const VideoSinkDispatcher* video_dispatcher_{nullptr};
        DeliveryCallbackScope* outer_scope_{nullptr};
    };

    /**
     * @brief 检查并拦截回调重入，避免当前线程自我等待形成死锁
     */
    void RejectCallbackReentrancy() const {
        if (DeliveryCallbackScope::Contains(this)) {
            throw std::logic_error("VideoSinkDispatcher::Attach/Detach cannot be called from OnReceiveVideoFrame");
        }
    }

    /**
     * @brief 阻塞挂起当前线程，直到目标 Sink 的在途回调计数彻底清空
     * @note 调用方必须已持有 lock
     */
    void WaitForNoInFlight(std::unique_lock<std::mutex>& lock, IVideoSink* sink) {
        if (sink == nullptr) {
            return;
        }

        in_flight_cv_.wait(lock,
                           [this, sink]() { return in_flight_callbacks_.find(sink) == in_flight_callbacks_.end(); });
    }

    /**
     * @brief 标记单次交付结束：递减在途计数，在归零时清理条目并唤醒等待者
     */
    void FinishDelivery(IVideoSink* sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = in_flight_callbacks_.find(sink);
        if (found == in_flight_callbacks_.end()) {
            return;
        }

        if (--found->second == 0) {
            in_flight_callbacks_.erase(found);
            // 仅在完全归零（条目被擦除）时唤醒等待中的 Attach/Detach
            in_flight_cv_.notify_all();
        }
    }

private:
    std::mutex mutex_;  ///< 保护 sink_ 与 in_flight_callbacks_ 的互斥锁
    std::condition_variable in_flight_cv_;  ///< 在途回调计数归零通知条件变量
    IVideoSink* sink_{nullptr};  ///< 当前绑定的外部目标显示端（弱引用指针）
    std::unordered_map<IVideoSink*, std::size_t>
        in_flight_callbacks_;  ///< 各 Sink 实例当前正在执行回调的并发计数追踪表
};

}  // namespace mediacore
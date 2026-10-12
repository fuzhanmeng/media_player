/**
 * @file pipeline.h
 * @brief 媒体流处理拓扑图（Pipeline）及其 Node、Link 全生命周期协调控制器
 * @details
 * Pipeline 负责统筹整个媒体播放引擎的拓扑结构与并发调度：
 * 1. Building 阶段：创建并登记所有 Node 与 Link；调用者须按 Source -> ... -> Sink 的正向拓扑顺序创建；
 * 2. 反向启动机制（Reverse Order Start）：按登记顺序的逆序启动工作线程，确保下游消费者先于上游生产者运行，
 *    彻底避免数据过早涌入导致队列溢出或信号丢失；启动失败时自动执行级联回滚；
 * 3. 五阶段无死锁停止协议（Five-stage Stop Protocol）：
 *    - 阶段 1：向所有 Node 发布停止信令（SignalStop）；
 *    - 阶段 2：强制打断（Abort）所有 Link 队列，唤醒因背压或等数据而阻塞的线程；
 *    - 阶段 3：解除 Seek Hold 与阻塞式外部 I/O（如网络/文件读取）；
 *    - 阶段 4：广播唤醒暂停门控条件变量（WakeAllPausedThreads）；
 *    - 阶段 5：逆序安全 Join 并回收全部 Node 线程；
 * 4. 世代控制（Epoch Isolation）：Seek 时原子递增 Epoch 并清空 Link 内部队列，阻断过期数据。
 *
 * @note CreateLink / EmplaceNode / Start / Pause / Resume / Seek / Stop 必须由同一个控制线程（如 Player
 * 控制线程）串行调用。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/inode.h"
#include "core/iseek_hold_releasable.h"
#include "core/iseek_source.h"
#include "core/link.h"
#include "core/pipeline_event.h"
#include "core/sync_context.h"
#include "mediacore/logger.h"

namespace mediacore {

/**
 * @enum PipelineState
 * @brief Pipeline 整体的一次性生命周期状态机
 */
enum class PipelineState : std::uint8_t {
    kBuilding,  ///< 拓扑构建中：允许创建、登记 Node 与 Link
    kStarting,  ///< 启动中：正在按逆向登记顺序启动 Node 工作线程
    kRunning,  ///< 运行中：所有 Node 已成功启动，拓扑结构彻底锁定
    kStopping,  ///< 停止中：正在执行停止协议或仍有线程尚未完成 Join
    kStopped,  ///< 已停止：所有 Node 线程已 Join 回收，管线不可重新启动
};

/**
 * @brief 将 Pipeline 生命周期状态转换为日志可读字符串
 * @param state 状态枚举
 * @return 对应的常量字符串
 */
inline const char* PipelineStateToString(PipelineState state) noexcept {
    switch (state) {
        case PipelineState::kBuilding:
            return "Building";
        case PipelineState::kStarting:
            return "Starting";
        case PipelineState::kRunning:
            return "Running";
        case PipelineState::kStopping:
            return "Stopping";
        case PipelineState::kStopped:
            return "Stopped";
    }
    return "Unknown";
}

/**
 * @enum PipelineStartResult
 * @brief Pipeline::Start() 的执行结果状态
 */
enum class PipelineStartResult : std::uint8_t {
    kStarted,  ///< 全部 Node 工作线程成功启动
    kInvalidState,  ///< 当前不处于 Building 状态，拒绝启动
    kNoNodes,  ///< 尚未登记任何 Node，管线保持 Building 状态
    kNodeStartFailed,  ///< 某个 Node 启动失败，且后续 Stop 回滚清理成功
    kRollbackFailed,  ///< 某个 Node 启动失败，且回滚清理过程中至少一个步骤发生异常
};

/**
 * @brief 将 Pipeline 启动结果转换为日志可读字符串
 * @param result 启动结果枚举
 * @return 对应的常量字符串
 */
inline const char* PipelineStartResultToString(PipelineStartResult result) noexcept {
    switch (result) {
        case PipelineStartResult::kStarted:
            return "Started";
        case PipelineStartResult::kInvalidState:
            return "InvalidState";
        case PipelineStartResult::kNoNodes:
            return "NoNodes";
        case PipelineStartResult::kNodeStartFailed:
            return "NodeStartFailed";
        case PipelineStartResult::kRollbackFailed:
            return "RollbackFailed";
    }
    return "Unknown";
}

/**
 * @class Pipeline
 * @brief 拥有媒体图拓扑并调度其 Node 和 Link 的控制核心
 * @details 所有公共控制接口应由同一外部控制线程串行驱动。
 */
class Pipeline final {
public:
    /**
     * @brief 默认构造函数：创建使用默认 std::thread 工厂的空拓扑图
     */
    Pipeline() : Pipeline(PipelineEventCallbacks{}, CreateDefaultThreadFactory()) {}

    /**
     * @brief 构造函数：指定全局事件回调集合
     * @param event_callbacks 事件回调载荷
     */
    explicit Pipeline(PipelineEventCallbacks event_callbacks)
        : Pipeline(std::move(event_callbacks), CreateDefaultThreadFactory()) {}

    /**
     * @brief 构造函数：指定线程创建工厂
     * @param thread_factory 自定义线程工厂
     */
    explicit Pipeline(NodeThreadFactory thread_factory)
        : Pipeline(PipelineEventCallbacks{}, std::move(thread_factory)) {}

    /**
     * @brief 完整构造函数：指定事件出口与线程创建工厂
     * @param event_callbacks 事件回调集合
     * @param thread_factory 自定义线程创建函数
     * @throws std::invalid_argument 当 thread_factory 为空时抛出
     */
    Pipeline(PipelineEventCallbacks event_callbacks, NodeThreadFactory thread_factory)
        : sync_ctx_(std::make_shared<SyncContext>()),
          event_callbacks_(NormalizeCallbacks(std::move(event_callbacks))),
          thread_factory_(std::move(thread_factory)) {
        if (!thread_factory_) {
            LOG_ERROR("Pipeline construction failed: empty NodeThreadFactory");
            throw std::invalid_argument("Pipeline requires a non-empty NodeThreadFactory");
        }
    }

    /**
     * @brief 析构函数：保证在管线销毁前彻底停止并 Join 全部线程
     */
    ~Pipeline() {
        LOG_INFO("Pipeline destroying, current state {}", PipelineStateToString(state_));

        const bool stop_succeeded = Stop();

        // 若离开 Stop() 后状态未达到 kStopped，说明仍有悬挂线程未 Join，直接强制 terminate 防止野指针崩溃
        if (state_ != PipelineState::kStopped) {
            LOG_ERROR("Pipeline destroyed before reaching kStopped; state is {}; terminating",
                      PipelineStateToString(state_));
            std::terminate();
        }

        if (!stop_succeeded) {
            LOG_ERROR("Pipeline stopped, but cleanup reported errors");
        }

        LOG_INFO("Pipeline destroyed");
    }

    // 禁用拷贝与移动，保证图拓扑的生命周期独占性
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    /**
     * @brief 创建并登记一条有界 Link 通道
     * @tparam T Link 传输的数据类型（必须是非引用、非数组、非 cv 限定的普通对象）
     * @param capacity 队列缓冲容量上限，必须大于 0
     * @return 创建完成的 Link 共享指针
     * @throws std::logic_error 当 Pipeline 未处于 Building 状态时抛出
     */
    template <typename T>
    std::shared_ptr<Link<T>> CreateLink(std::size_t capacity) {
        static_assert(std::is_object<T>::value, "Link<T> requires a non-reference object type");
        static_assert(!std::is_array<T>::value, "Link<T> does not support array element types");
        static_assert(!std::is_const<T>::value && !std::is_volatile<T>::value,
                      "Link<T> requires an unqualified mutable value type");
        static_assert(!std::is_same<T, NoPort>::value,
                      "NoPort marks a missing Node port and cannot be transported by a Link");

        RequireBuilding("Link can only be created while Pipeline is building");

        auto link = std::make_shared<Link<T>>(capacity, LinkCreationToken{});
        if (!link) {
            LOG_ERROR("Create link failed due to memory allocation failure");
        }
        links_.emplace_back(link);

        LOG_DEBUG("Pipeline link created, capacity {}, total links {}", capacity, links_.size());
        return link;
    }

    /**
     * @brief 创建并登记一个拓扑 Node
     * @tparam NodeT 具体的节点类型（必须继承自 INodeBase 且不可为抽象类）
     * @tparam Args 传递给 NodeT 构造函数的业务参数类型（不包含 SyncContext 与事件回调）
     * @param args 业务实参
     * @return 新创建 Node 的借用引用
     * @note 必须按 Source -> ... -> Sink 的正向拓扑顺序调用此函数登记。
     * @throws std::logic_error 当 Pipeline 未处于 Building 状态时抛出
     */
    template <typename NodeT, typename... Args>
    NodeT& EmplaceNode(Args&&... args) {
        static_assert(std::is_base_of<INodeBase, NodeT>::value, "NodeT must derive from INodeBase");
        static_assert(std::is_convertible<NodeT*, INodeBase*>::value,
                      "NodeT must publicly and unambiguously derive from INodeBase");
        static_assert(!std::is_abstract<NodeT>::value, "NodeT must be a concrete, non-abstract Node type");

        // 校验构造函数契约：必须前置接收 SyncContext 与 PipelineEventCallbacks
        static_assert(
            std::is_constructible<NodeT, std::shared_ptr<SyncContext>, PipelineEventCallbacks, Args...>::value,
            "NodeT must be constructible from shared_ptr<SyncContext> followed by PipelineEventCallbacks and Args...");

        RequireBuilding("Node can only be created while Pipeline is building");

        auto node = std::make_unique<NodeT>(sync_ctx_, MakeNodeCallbacks(), std::forward<Args>(args)...);

        NodeT& ref = *node;
        nodes_.emplace_back(std::move(node));
        LOG_DEBUG("Pipeline node emplaced: {}", ref.GetName());
        return ref;
    }

    /**
     * @brief 按 nodes_ 登记顺序的逆序启动全部节点线程
     * @return 明确的启动结果枚举；若任一 Node 启动失败，将立即触发完整 Stop 回滚并返回错误
     * @details 逆序启动确保消费者先于生产者就绪，防止上游过早投递导致数据堆积。
     */
    PipelineStartResult Start() {
        LOG_INFO("Pipeline start requested, state {}, nodes {}, links {}",
                 PipelineStateToString(state_),
                 nodes_.size(),
                 links_.size());

        if (state_ != PipelineState::kBuilding) {
            LOG_WARN("Pipeline start rejected: state is {}", PipelineStateToString(state_));
            return PipelineStartResult::kInvalidState;
        }

        if (nodes_.empty()) {
            LOG_WARN("Pipeline start rejected: no nodes");
            return PipelineStartResult::kNoNodes;
        }

        state_ = PipelineState::kStarting;
        LOG_INFO("Pipeline state changed: Building -> Starting");

        // 逆向启动：优先拉起消费者，最后拉起源节点
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
            LOG_DEBUG("Starting node '{}'", (*it)->GetName());

            const NodeStartResult node_result = (*it)->Start(thread_factory_);
            if (node_result != NodeStartResult::kStarted) {
                LOG_ERROR("Node '{}' failed to start: {}", (*it)->GetName(), NodeStartResultToString(node_result));

                const PipelineStartResult rollback_result =
                    Stop() ? PipelineStartResult::kNodeStartFailed : PipelineStartResult::kRollbackFailed;

                LOG_ERROR("Pipeline start rollback result: {}", PipelineStartResultToString(rollback_result));
                return rollback_result;
            }
        }

        state_ = PipelineState::kRunning;
        LOG_INFO("Pipeline started");
        return PipelineStartResult::kStarted;
    }

    /**
     * @brief 暂停管线中所有节点的后续处理（门控拦截）
     * @return 处于运行态并成功暂停返回 true，否则返回 false
     */
    bool Pause() {
        if (state_ != PipelineState::kRunning) {
            LOG_WARN("Pipeline pause rejected: state is {}", PipelineStateToString(state_));
            return false;
        }

        sync_ctx_->SetPaused();
        return true;
    }

    /**
     * @brief 恢复管线中所有节点的处理
     * @return 处于运行态并成功恢复返回 true，否则返回 false
     */
    bool Resume() {
        if (state_ != PipelineState::kRunning) {
            LOG_WARN("Pipeline resume rejected: state is {}", PipelineStateToString(state_));
            return false;
        }

        sync_ctx_->SetRunning();
        return true;
    }

    /**
     * @brief 开启新媒体世代并清空所有 Link 的滞留数据
     * @return 新生成的 Epoch 版本号
     * @details Seek 的第一阶段：递增 Epoch 并执行全图 Link::Flush()。
     * @throws std::logic_error 当管线未处于 Running 状态时抛出
     */
    std::uint64_t BeginNewEpoch() {
        if (state_ != PipelineState::kRunning) {
            throw std::logic_error("A new epoch requires a running Pipeline");
        }

        const std::uint64_t epoch = sync_ctx_->BumpEpoch();
        for (const auto& link : links_) {
            link->Flush();
        }

        return epoch;
    }

    /**
     * @brief 注册管线的 Seek 数据源节点
     * @param source 实现了 ISeekSource 接口的源节点非拥有指针
     */
    void SetSeekSource(ISeekSource* source) noexcept { seek_source_ = source; }

    /**
     * @brief 注册 Seek Hold 挂起释放者（通常为 VideoSinkNode）
     * @param releasable 实现了 ISeekHoldReleasable 接口的节点非拥有指针
     */
    void SetSeekHoldReleasable(ISeekHoldReleasable* releasable) noexcept { hold_releasable_ = releasable; }

    /**
     * @brief 解除当前的 Seek Hold 挂起状态
     */
    void ReleaseSeekHold() noexcept {
        if (hold_releasable_) {
            hold_releasable_->ReleaseSeekHold();
        }
    }

    /**
     * @brief 向注册的源节点请求时间跳转（Seek）
     * @param target_us 目标时间戳（微秒）
     * @return 若成功下发则返回新世代 Epoch，未注册 Seek 源时返回 std::nullopt
     * @throws std::logic_error 当管线未处于 Running 状态时抛出
     */
    std::optional<std::uint64_t> Seek(std::int64_t target_us) {
        if (state_ != PipelineState::kRunning) {
            throw std::logic_error("Seek requires a running Pipeline");
        }

        if (!seek_source_) {
            LOG_WARN("Seek rejected: no SeekSource registered");
            return std::nullopt;
        }

        const std::uint64_t epoch = BeginNewEpoch();
        seek_source_->RequestSeek(epoch, target_us);
        sync_ctx_->WakeAllPausedThreads();
        return epoch;
    }

    /**
     * @brief 五阶段协作停止协议：彻底停止并回收所有 Node 线程与 Link
     * @return 所有 Node 成功 Join 退出时返回 true
     */
    bool Stop() noexcept {
        if (state_ == PipelineState::kStopped) {
            return stop_succeeded_;
        }

        if (stop_in_progress_) {
            return false;
        }

        stop_in_progress_ = true;
        state_ = PipelineState::kStopping;
        LOG_INFO("Pipeline stopping: {} nodes, {} links", nodes_.size(), links_.size());

        // 阶段 1：向所有 Node 发布停止信令
        for (auto& node : nodes_) {
            node->SignalStop();
        }

        // 阶段 2：Abort 所有 Link，解除读写阻塞
        bool success = true;
        for (auto& link : links_) {
            try {
                link->Abort();
            } catch (...) {
                LOG_ERROR("Pipeline stop failed to abort a link");
                success = false;
            }
        }

        // 解除 Seek hold，避免 Sink 节点因等待 Player 决策而永久挂起
        ReleaseSeekHold();

        // 阶段 3：打断底层可能阻塞的外部非 Link I/O（如网络读取或编解码操作）
        for (auto& node : nodes_) {
            node->InterruptBlockingIo();
        }

        // 阶段 4：唤醒正阻塞在暂停门控条件变量上的所有工作线程
        try {
            sync_ctx_->WakeAllPausedThreads();
        } catch (...) {
            LOG_ERROR("Pipeline stop failed to wake paused threads");
        }

        // 阶段 5：逐一 Join 回收所有工作线程
        bool all_nodes_joined = true;
        for (auto& node : nodes_) {
            const NodeJoinResult result = node->Join();
            if (result == NodeJoinResult::kCannotJoinSelf || result == NodeJoinResult::kJoinFailed) {
                LOG_ERROR("Node '{}' failed to join", node->GetName());
                success = false;
                all_nodes_joined = false;
            }
        }

        stop_in_progress_ = false;
        if (!all_nodes_joined) {
            LOG_ERROR("Pipeline stop incomplete: at least one node was not joined");
            return false;
        }

        state_ = PipelineState::kStopped;
        stop_succeeded_ = success;
        LOG_INFO("Pipeline stopped");
        return stop_succeeded_;
    }

    /**
     * @brief 获取当前 Pipeline 的生命周期状态
     */
    PipelineState GetState() const noexcept { return state_; }

    /**
     * @brief 获取当前全局数据世代号（Epoch）
     */
    std::uint64_t GetCurrentEpoch() const noexcept { return sync_ctx_->GetEpoch(); }

    /**
     * @brief 获取共享同步上下文的只读视图
     */
    std::shared_ptr<const SyncContext> GetSyncContext() const noexcept { return sync_ctx_; }

private:
    /**
     * @brief 创建默认的 std::thread 线程工厂
     */
    static NodeThreadFactory CreateDefaultThreadFactory() {
        return [](const std::string&, NodeThreadEntry entry) { return std::thread(std::move(entry)); };
    }

    /**
     * @brief 规范化事件回调集合，为未配置的项提供空操作兜底
     */
    static PipelineEventCallbacks NormalizeCallbacks(PipelineEventCallbacks callbacks) {
        if (!callbacks.on_media_ended_) {
            callbacks.on_media_ended_ = [](const MediaEnded&) {};
        }
        if (!callbacks.on_node_error_) {
            callbacks.on_node_error_ = [](const NodeError&) {};
        }
        if (!callbacks.on_seek_completed_) {
            callbacks.on_seek_completed_ = [](const SeekCompleted&) {};
        }
        return callbacks;
    }

    /**
     * @brief 为每个 Node 构造经过 Epoch 世代校验的回调代理
     */
    PipelineEventCallbacks MakeNodeCallbacks() {
        return PipelineEventCallbacks{[this](MediaEnded event) {
                                          if (event.epoch_ == sync_ctx_->GetEpoch()) {
                                              event_callbacks_.on_media_ended_(std::move(event));
                                          }
                                      },
                                      [this](NodeError event) { event_callbacks_.on_node_error_(std::move(event)); },
                                      [this](SeekCompleted event) {
                                          if (event.epoch_ == sync_ctx_->GetEpoch()) {
                                              event_callbacks_.on_seek_completed_(event);
                                          }
                                      }};
    }

    /**
     * @brief 校验当前是否处于 Building 状态，非法时抛出异常
     * @param message 异常描述
     * @throws std::logic_error 当管线状态不是 kBuilding 时抛出
     */
    void RequireBuilding(const char* message) const {
        if (state_ != PipelineState::kBuilding) {
            LOG_ERROR("Pipeline topology mutation rejected: {}", message);
            throw std::logic_error(message);
        }
    }

    std::shared_ptr<SyncContext> sync_ctx_;  ///< 全拓扑共享的同步与世代上下文
    PipelineEventCallbacks event_callbacks_;  ///< 对外汇报的事件回调集合
    ISeekSource* seek_source_{nullptr};  ///< 负责响应 Seek 的源节点（非拥有弱引用）
    ISeekHoldReleasable* hold_releasable_{nullptr};  ///< 负责响应 Seek Hold 解除的节点（非拥有弱引用）
    NodeThreadFactory thread_factory_;  ///< 工作线程创建工厂
    std::vector<std::unique_ptr<INodeBase>> nodes_;  ///< 拓扑中所有 Node 的独占所有权集合
    std::vector<std::shared_ptr<ILinkControl>> links_;  ///< 拓扑中所有 Link 的多态控制接口集合
    PipelineState state_{PipelineState::kBuilding};  ///< 管线生命周期状态
    bool stop_in_progress_{false};  ///< 停止操作防重入标志
    bool stop_succeeded_{false};  ///< 停止操作的最终判定结果
};

}  // namespace mediacore
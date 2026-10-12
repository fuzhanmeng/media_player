/**
 * @file inode.h
 * @brief DAG 节点线程生命周期基类与强类型数据端口模板定义
 * @details
 * 本文件定义了 DAG 节点的两层架构抽象：
 *
 * 1. INodeBase（职责：工作线程与执行生命周期管理）
 *    - 负责管理单个后台工作线程、原子状态流转、统一事件循环（ThreadMain）以及退出原因记录；
 *    - 自身不持有任何图边（Link），析构函数严禁隐式停止线程（仅断言线程已被安全 Join 回收）；
 *    - 生命周期受 Pipeline 统一调度：遵循 SignalStop -> Abort Links -> Interrupt I/O -> Wake Paused -> Join 协议。
 *
 * 2. INode<InT, OutT> 及其偏特化（职责：强类型数据端口绑定）
 *    - 为常见的数据流节点形态提供自动化的强类型 Link 绑定与安全的收发封装（ReceiveInput / SendOutput）；
 *    - 引入 NoPort 占位类型触发偏特化，精确表达 Source（无输入）与 Sink（无输出）节点，规避非法的 Link<void> 实例化；
 *    - 具备多输入/多输出的复杂节点（如多路分离的 Demuxer），可直接继承 INodeBase 并自行持有所需的 Link。
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "core/link.h"
#include "core/pipeline_event.h"
#include "core/sync_context.h"
#include "mediacore/logger.h"

namespace mediacore {

class Pipeline;

/**
 * @brief 节点工作线程入口函数的可调用包装类型
 */
using NodeThreadEntry = std::function<void()>;

/**
 * @brief 创建节点工作线程的工厂函数类型
 * @details
 * 第一个参数为 Node 名称，第二个参数为线程入口函数；
 * 成功时必须返回处于 joinable 状态的 std::thread，不得在调用者线程同步执行入口函数；
 * 失败时应抛出异常或返回不可 Join 的 std::thread。
 */
using NodeThreadFactory = std::function<std::thread(const std::string&, NodeThreadEntry)>;

/**
 * @enum NodeState
 * @brief 节点工作线程的一次性生命周期状态
 */
enum class NodeState : std::uint8_t {
    kCreated,  ///< Node 已构造，尚未创建工作线程
    kRunning,  ///< 工作线程已启动并在运行
    kStopRequested,  ///< 已收到 Pipeline 的停止请求
    kFinished,  ///< 工作线程循环已退出，等待被回收 Join
    kJoined,  ///< 工作线程已被 Join 回收，Node 可安全析构
};

/**
 * @enum NodeStartResult
 * @brief 节点 Start() 操作的执行结果
 */
enum class NodeStartResult : std::uint8_t {
    kStarted,  ///< 工作线程成功创建并启动
    kAlreadyStarted,  ///< 节点已经启动或已经结束，禁止重复启动
    kStopAlreadyRequested,  ///< 启动前已收到停止请求，放弃启动
    kThreadCreationFailed,  ///< 线程工厂抛出异常或返回了不可 Join 的无效线程
};

/**
 * @enum NodeJoinResult
 * @brief 节点工作线程回收 Join() 的执行结果
 */
enum class NodeJoinResult : std::uint8_t {
    kJoined,  ///< 成功回收线程资源
    kNoThread,  ///< 节点此前未创建过线程
    kAlreadyJoined,  ///< 线程此前已被回收，无需重复 Join
    kCannotJoinSelf,  ///< 禁止工作线程自我 Join（死锁防御）
    kJoinFailed,  ///< 底层 std::thread::join() 抛出异常执行失败
};

/**
 * @enum TickResult
 * @brief 派生节点执行单次 Tick() 处理的业务结果
 */
enum class TickResult : std::uint8_t {
    kContinue,  ///< 本轮处理顺利完成，允许继续进入下一次 Tick()
    kStopped,  ///< 因收到停机或取消请求而正常结束
    kError,  ///< 遭遇不可恢复的业务异常，要求终止工作线程
};

/**
 * @enum NodeFinishReason
 * @brief 工作线程退出原因枚举，供 Pipeline 汇报与状态诊断
 */
enum class NodeFinishReason : std::uint8_t {
    kNotFinished,  ///< 工作线程尚未退出
    kStopRequested,  ///< 响应外部停止信号正常退出
    kError,  ///< 因 Tick() 或控制流程返回 kError 终止
    kUnhandledException,  ///< 捕获到底层逃逸的未处理 C++ 异常
};

/**
 * @brief 将 NodeState 转换为日志可读字符串
 */
inline const char* NodeStateToString(NodeState state) noexcept {
    switch (state) {
        case NodeState::kCreated:
            return "Created";
        case NodeState::kRunning:
            return "Running";
        case NodeState::kStopRequested:
            return "StopRequested";
        case NodeState::kFinished:
            return "Finished";
        case NodeState::kJoined:
            return "Joined";
    }
    return "Unknown";
}

/**
 * @brief 将 NodeStartResult 转换为日志可读字符串
 */
inline const char* NodeStartResultToString(NodeStartResult result) noexcept {
    switch (result) {
        case NodeStartResult::kStarted:
            return "Started";
        case NodeStartResult::kAlreadyStarted:
            return "AlreadyStarted";
        case NodeStartResult::kStopAlreadyRequested:
            return "StopAlreadyRequested";
        case NodeStartResult::kThreadCreationFailed:
            return "ThreadCreationFailed";
    }
    return "Unknown";
}

/**
 * @brief 将 NodeJoinResult 转换为日志可读字符串
 */
inline const char* NodeJoinResultToString(NodeJoinResult result) noexcept {
    switch (result) {
        case NodeJoinResult::kJoined:
            return "Joined";
        case NodeJoinResult::kNoThread:
            return "NoThread";
        case NodeJoinResult::kAlreadyJoined:
            return "AlreadyJoined";
        case NodeJoinResult::kCannotJoinSelf:
            return "CannotJoinSelf";
        case NodeJoinResult::kJoinFailed:
            return "JoinFailed";
    }
    return "Unknown";
}

/**
 * @brief 将 TickResult 转换为日志可读字符串
 */
inline const char* TickResultToString(TickResult result) noexcept {
    switch (result) {
        case TickResult::kContinue:
            return "Continue";
        case TickResult::kStopped:
            return "Stopped";
        case TickResult::kError:
            return "Error";
    }
    return "Unknown";
}

/**
 * @brief 将 NodeFinishReason 转换为日志可读字符串
 */
inline const char* NodeFinishReasonToString(NodeFinishReason reason) noexcept {
    switch (reason) {
        case NodeFinishReason::kNotFinished:
            return "NotFinished";
        case NodeFinishReason::kStopRequested:
            return "StopRequested";
        case NodeFinishReason::kError:
            return "Error";
        case NodeFinishReason::kUnhandledException:
            return "UnhandledException";
    }
    return "Unknown";
}

/**
 * @class INodeBase
 * @brief 管理单个工作线程执行与事件循环的核心基类
 * @note 节点析构前必须已完成 Join 回收；具体的数据业务处理由派生类的 Tick() 实现。
 */
class INodeBase {
private:
    /**
     * @struct PendingNodeError
     * @brief 由 Fail() 暂存并延迟到主事件循环统一上报的错误载荷
     */
    struct PendingNodeError {
        std::uint64_t epoch_{0};
        MediaError media_error_{};
        std::string message_{};
    };

public:
    /**
     * @brief 构造函数：初始化未启动的节点
     * @param name 节点名称
     * @param sync 共享同步控制上下文
     * @param event_callbacks 事件回调载荷
     * @throws std::invalid_argument 当 sync 为空或关键回调未设置时抛出
     */
    INodeBase(std::string name, std::shared_ptr<SyncContext> sync, PipelineEventCallbacks event_callbacks)
        : sync_ctx_(std::move(sync)), event_callbacks_(std::move(event_callbacks)), name_(std::move(name)) {
        if (!sync_ctx_) {
            throw std::invalid_argument("INodeBase requires a SyncContext");
        }
        if (!event_callbacks_.on_media_ended_ || !event_callbacks_.on_node_error_) {
            throw std::invalid_argument("INodeBase requires both Pipeline event callbacks");
        }
        LOG_INFO("Node '{}' constructed", name_);
    }

    /**
     * @brief 析构函数：校验工作线程已安全回收
     * @details 断言工作线程已被回收，不在此处隐式停止或 Join，确保符合确定性销毁规范。
     */
    virtual ~INodeBase() { assert(!thread_.joinable() && "Pipeline must join every Node before destruction"); }

    // 禁用拷贝与移动（节点地址在线程启动后被工作线程捕获）
    INodeBase(const INodeBase&) = delete;
    INodeBase& operator=(const INodeBase&) = delete;
    INodeBase(INodeBase&&) = delete;
    INodeBase& operator=(INodeBase&&) = delete;

    /**
     * @brief 查询当前节点是否已收到停止请求
     */
    bool IsStopRequested() const noexcept { return stop_requested_.load(std::memory_order_acquire); }

    /**
     * @brief 获取当前节点的执行状态快照
     */
    NodeState GetState() const noexcept { return state_.load(std::memory_order_acquire); }

    /**
     * @brief 获取工作线程的最终退出原因
     */
    NodeFinishReason GetFinishReason() const noexcept { return finish_reason_.load(std::memory_order_acquire); }

    /**
     * @brief 获取节点名称
     */
    const std::string& GetName() const noexcept { return name_; }

    /**
     * @brief 获取工作线程捕获到的未处理 C++ 异常指针
     * @throws std::logic_error 当节点尚未完成 Join 时抛出
     */
    std::exception_ptr GetUnhandledException() const {
        if (GetState() != NodeState::kJoined) {
            throw std::logic_error("GetUnhandledException requires a joined Node");
        }
        return unhandled_exception_;
    }

protected:
    /**
     * @brief 派生类核心业务单次驱动入口
     * @return TickResult 执行状态；返回 kStopped 或 kError 时工作线程退出循环
     */
    virtual TickResult Tick() = 0;

    /**
     * @brief 检查是否存在外部提交且未被执行的控制命令（如 Seek）
     */
    virtual bool HasPendingControl() const noexcept { return false; }

    /**
     * @brief 提取并执行外部提交的控制命令
     */
    virtual TickResult ProcessPendingControl() { return TickResult::kContinue; }

    /**
     * @brief 登记基于当前全局 Epoch 的通用文本错误并标记失败
     * @param message 错误描述文本
     */
    TickResult Fail(std::string message) { return Fail(std::move(message), GetCurrentEpoch()); }

    /**
     * @brief 登记指定 Epoch 的通用文本错误并标记失败
     * @param message 错误描述文本
     * @param epoch 错误所属的媒体世代号
     */
    TickResult Fail(std::string message, std::uint64_t epoch) {
        pending_error_ = PendingNodeError{epoch, MediaError{}, std::move(message)};
        return TickResult::kError;
    }

    /**
     * @brief 登记指定 Epoch 的结构化 MediaError 并标记失败
     * @param error 结构化媒体错误对象
     * @param epoch 错误所属的媒体世代号
     */
    TickResult Fail(MediaError error, std::uint64_t epoch) {
        pending_error_ = PendingNodeError{epoch, std::move(error), {}};
        return TickResult::kError;
    }

    /**
     * @brief 唤醒可能处于暂停挂起状态的工作线程以优先响应控制指令
     */
    void NotifyControlPending() { sync_ctx_->WakeAllPausedThreads(); }

    /**
     * @brief 中断节点持有的非 Link 阻塞式底层 I/O（如网络读取或音频设备）
     */
    virtual void OnInterruptBlockingIo() noexcept {}

    /**
     * @brief 判定当前是否仍允许配置端口（仅 kCreated 状态允许）
     */
    bool CanConfigure() const noexcept { return GetState() == NodeState::kCreated; }

    /**
     * @brief 判定数据项绑定的 Epoch 是否匹配当前全局活跃世代
     * @param item_epoch 数据项携带的世代号
     * @return 匹配返回 true；否则应判定为过时数据直接丢弃
     */
    bool IsCurrentEpoch(std::uint64_t item_epoch) const noexcept { return item_epoch == sync_ctx_->GetEpoch(); }

    /**
     * @brief 获取当前全局媒体世代号
     */
    std::uint64_t GetCurrentEpoch() const noexcept { return sync_ctx_->GetEpoch(); }

    /**
     * @brief 获取当前运行世代号
     */
    std::uint64_t GetCurrentRunGeneration() const noexcept { return sync_ctx_->GetRunGeneration(); }

    /**
     * @brief 向上层汇报媒体播放完毕事件
     * @param epoch 已完成的媒体世代版本号
     */
    void ReportMediaEnded(std::uint64_t epoch) const noexcept {
        try {
            event_callbacks_.on_media_ended_(MediaEnded{epoch, name_});
        } catch (...) {
        }
    }

    /**
     * @brief 向上层汇报 Seek 定位成功事件
     * @param epoch 完成定位的媒体世代版本号
     * @param actual_pos 交付上屏的首帧 PTS 时间戳（微秒）
     * @param reached_eof 是否触底到达流末尾
     */
    void ReportSeekCompleted(std::uint64_t epoch, std::int64_t actual_pos, bool reached_eof) const noexcept {
        try {
            event_callbacks_.on_seek_completed_(SeekCompleted{epoch, actual_pos, reached_eof});
        } catch (...) {
        }
    }

    /**
     * @brief 向上层汇报当前媒体世代的通用错误
     */
    void ReportNodeError(std::string_view message, std::exception_ptr exception = {}) const noexcept {
        ReportNodeError(GetCurrentEpoch(), message, std::move(exception));
    }

    /**
     * @brief 向上层汇报指定世代的通用文本错误
     */
    void ReportNodeError(std::uint64_t epoch,
                         std::string_view message,
                         std::exception_ptr exception = {}) const noexcept {
        try {
            NodeError event;
            event.node_name_ = name_;
            event.exception_ = std::move(exception);
            event.message_ = std::string(message);
            event.epoch_ = epoch;
            event_callbacks_.on_node_error_(std::move(event));
        } catch (...) {
        }
    }

    /**
     * @brief 向上层汇报指定世代的结构化错误
     */
    void ReportNodeError(std::uint64_t epoch, MediaError error, std::exception_ptr exception = {}) const noexcept {
        try {
            NodeError event;
            event.node_name_ = name_;
            event.epoch_ = epoch;
            event.exception_ = std::move(exception);
            event.message_ = error.Message();
            event.media_error_ = std::move(error);
            event_callbacks_.on_node_error_(std::move(event));
        } catch (...) {
        }
    }

    std::shared_ptr<SyncContext> sync_ctx_;  ///< 共享同步上下文（只读其运行状态与 Epoch）

private:
    friend class Pipeline;

    /**
     * @brief 由 Pipeline 调用的工作线程启动逻辑
     */
    NodeStartResult Start(const NodeThreadFactory& thread_factory) noexcept {
        LOG_INFO("Node '{}' start requested, state {}", name_, NodeStateToString(GetState()));

        NodeState expected = NodeState::kCreated;
        if (!state_.compare_exchange_strong(
                expected, NodeState::kRunning, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (expected == NodeState::kStopRequested) {
                LOG_WARN(
                    "Node '{}' start rejected: Stop already requested, state {}", name_, NodeStateToString(expected));
            } else {
                LOG_WARN(
                    "Node '{}' start rejected: already started/finished, state {}", name_, NodeStateToString(expected));
            }
            return expected == NodeState::kStopRequested ? NodeStartResult::kStopAlreadyRequested
                                                         : NodeStartResult::kAlreadyStarted;
        }

        try {
            std::thread worker = thread_factory(name_, [this]() { ThreadMain(); });
            if (!worker.joinable()) {
                RollBackFailedStart();
                LOG_ERROR("Node '{}' thread creation returned non-joinable thread", name_);
                return NodeStartResult::kThreadCreationFailed;
            }
            thread_ = std::move(worker);
        } catch (...) {
            RollBackFailedStart();
            LOG_ERROR("Node '{}' thread creation threw an exception", name_);
            return NodeStartResult::kThreadCreationFailed;
        }

        LOG_INFO("Node '{}' started", name_);
        return NodeStartResult::kStarted;
    }

    /**
     * @brief 异步发送停止信号（五阶段停止第一阶段）
     */
    void SignalStop() noexcept {
        LOG_INFO("Node '{}' stop signal requested, state {}", name_, NodeStateToString(state_.load()));
        stop_requested_.store(true, std::memory_order_release);

        NodeState current = state_.load(std::memory_order_acquire);
        while (current != NodeState::kStopRequested && current != NodeState::kFinished &&
               current != NodeState::kJoined &&
               !state_.compare_exchange_weak(
                   current, NodeState::kStopRequested, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        LOG_DEBUG("Node '{}' stop signal published, state {}", name_, NodeStateToString(state_.load()));
    }

    /**
     * @brief 请求中断节点持有的底层阻塞式 I/O
     */
    void InterruptBlockingIo() noexcept {
        if (thread_.joinable()) {
            LOG_DEBUG("Node '{}' interrupting blocking I/O", name_);
            OnInterruptBlockingIo();
        } else {
            LOG_TRACE("Node '{}' has no joinable thread, skip blocking I/O interrupt", name_);
        }
    }

    /**
     * @brief 回收并 Join 工作线程（五阶段停止第五阶段）
     */
    NodeJoinResult Join() noexcept {
        LOG_INFO("Node '{}' join requested, state {}", name_, NodeStateToString(state_.load()));

        if (GetState() == NodeState::kJoined) {
            LOG_DEBUG("Node '{}' already joined", name_);
            return NodeJoinResult::kAlreadyJoined;
        }

        if (!thread_.joinable()) {
            state_.store(NodeState::kJoined, std::memory_order_release);
            LOG_DEBUG("Node '{}' has no thread to join", name_);
            return NodeJoinResult::kNoThread;
        }

        if (thread_.get_id() == std::this_thread::get_id()) {
            LOG_ERROR("Node '{}' cannot join itself", name_);
            return NodeJoinResult::kCannotJoinSelf;
        }

        try {
            thread_.join();
        } catch (...) {
            LOG_ERROR("Node '{}' thread join threw an exception", name_);
            return NodeJoinResult::kJoinFailed;
        }

        state_.store(NodeState::kJoined, std::memory_order_release);
        LOG_INFO("Node '{}' joined, finish reason {}", name_, NodeFinishReasonToString(finish_reason_.load()));
        return NodeJoinResult::kJoined;
    }

    /**
     * @brief 启动失败时的状态回滚处理
     */
    void RollBackFailedStart() noexcept {
        const NodeState rollback_state = IsStopRequested() ? NodeState::kStopRequested : NodeState::kCreated;
        NodeState expected = NodeState::kRunning;
        state_.compare_exchange_strong(expected, rollback_state, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    /**
     * @brief 判定单次 Tick 或控制命令结果是否应终结工作线程
     */
    bool FinishForResult(TickResult result, std::string_view error_message, NodeFinishReason& reason) noexcept {
        switch (result) {
            case mediacore::TickResult::kContinue:
                return false;
            case mediacore::TickResult::kStopped:
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    return true;
                }
                ReportNodeError(GetCurrentEpoch(), "Node returned kStopped without a Stop Requested");
                reason = NodeFinishReason::kError;
                return true;
            case mediacore::TickResult::kError:
                if (pending_error_.has_value()) {
                    PendingNodeError pending_error = std::move(*pending_error_);
                    pending_error_.reset();

                    if (pending_error.media_error_.HasError()) {
                        ReportNodeError(pending_error.epoch_, std::move(pending_error.media_error_));
                    } else {
                        const std::string_view message =
                            pending_error.message_.empty() ? error_message : std::string_view(pending_error.message_);
                        ReportNodeError(pending_error.epoch_, message);
                    }
                } else {
                    ReportNodeError(GetCurrentEpoch(), error_message);
                }
                reason = NodeFinishReason::kError;
                return true;
        }

        ReportNodeError(GetCurrentEpoch(), "Node returned an unknown TickResult");
        reason = NodeFinishReason::kError;
        return true;
    }

    /**
     * @brief 工作线程的主事件循环
     */
    void ThreadMain() noexcept {
        NodeFinishReason reason = NodeFinishReason::kNotFinished;

        try {
            while (true) {
                // 1. 最高优先级：检查外部停止请求
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                // 2. 处理挂起的控制命令（如 Seek）
                if (HasPendingControl()) {
                    const TickResult result = ProcessPendingControl();
                    if (IsStopRequested()) {
                        reason = NodeFinishReason::kStopRequested;
                        break;
                    }
                    if (FinishForResult(result, "ProcessPendingControl returned kError", reason)) {
                        break;
                    }
                    continue;
                }

                // 3. 暂停门控拦截：在处于 Paused 状态时挂起等待
                sync_ctx_->WaitWhilePaused([this]() { return IsStopRequested() || HasPendingControl(); });

                // 4. 唤醒后再次检查停止请求与控制命令
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                if (HasPendingControl()) {
                    continue;
                }

                if (sync_ctx_->GetState() == PipelineRunState::kPaused) {
                    continue;
                }

                // 5. 驱动具体的流数据业务处理
                const TickResult result = Tick();
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                if (FinishForResult(result, "Tick returned kError", reason)) {
                    break;
                }
            }
        } catch (...) {
            unhandled_exception_ = std::current_exception();
            ReportNodeError(GetCurrentEpoch(), "Unhandled exception escaped the Node worker", unhandled_exception_);
            reason = NodeFinishReason::kUnhandledException;
        }

        finish_reason_.store(reason, std::memory_order_release);
        state_.store(NodeState::kFinished, std::memory_order_release);
    }

    PipelineEventCallbacks event_callbacks_;  ///< 事件回调输出载荷
    std::string name_;  ///< 节点名称
    std::thread thread_;  ///< 工作线程对象句柄
    std::atomic<NodeState> state_{NodeState::kCreated};  ///< 节点生命周期状态机
    std::atomic<bool> stop_requested_{false};  ///< 停止请求标志位
    std::atomic<NodeFinishReason> finish_reason_{NodeFinishReason::kNotFinished};  ///< 线程退出原因
    std::exception_ptr unhandled_exception_;  ///< 逃逸未捕获异常
    std::optional<PendingNodeError> pending_error_;  ///< 暂存的错误信息（仅在 Worker 线程访问）
};

/**
 * @struct NoPort
 * @brief 占位标签类型，用于触发 INode 模板偏特化（表达 Source 无输入或 Sink 无输出）
 */
struct NoPort final {};

/**
 * @class INode
 * @brief 具备单输入与单输出 Link 的标准节点模板
 * @tparam InT 输入数据项类型
 * @tparam OutT 输出数据项类型
 */
template <typename InT, typename OutT>
class INode : public INodeBase {
public:
    using InputLink = Link<InT>;
    using OutputLink = Link<OutT>;

    INode(std::string name, std::shared_ptr<SyncContext> sync, PipelineEventCallbacks event_callbacks)
        : INodeBase(std::move(name), std::move(sync), std::move(event_callbacks)) {}

    /**
     * @brief 连接输入 Link
     */
    bool ConnectInput(std::shared_ptr<InputLink> input) {
        if (!CanConfigure() || !input) {
            LOG_WARN("Node '{}' ConnectInput rejected: configurable {}, input valid {}",
                     GetName(),
                     CanConfigure(),
                     static_cast<bool>(input));
            return false;
        }
        input_ = std::move(input);
        LOG_DEBUG("Node '{}' input connected", GetName());
        return true;
    }

    /**
     * @brief 连接输出 Link
     */
    bool ConnectOutput(std::shared_ptr<OutputLink> output) {
        if (!CanConfigure() || !output) {
            LOG_WARN("Node '{}' ConnectOutput rejected: configurable {}, output valid {}",
                     GetName(),
                     CanConfigure(),
                     static_cast<bool>(output));
            return false;
        }
        output_ = std::move(output);
        LOG_DEBUG("Node '{}' output connected", GetName());
        return true;
    }

protected:
    /**
     * @brief 从上游 Link 阻塞接收数据
     */
    std::optional<InT> ReceiveInput() { return input_ ? input_->Receive() : std::nullopt; }

    /**
     * @brief 向下游 Link 阻塞发送数据
     */
    bool SendOutput(OutT item) { return output_ && output_->Send(std::move(item)); }

private:
    std::shared_ptr<InputLink> input_;
    std::shared_ptr<OutputLink> output_;
};

/**
 * @class INode<NoPort, OutT>
 * @brief 零输入、单输出的 Source 数据源节点偏特化
 */
template <typename OutT>
class INode<NoPort, OutT> : public INodeBase {
public:
    using OutputLink = Link<OutT>;

    INode(std::string name, std::shared_ptr<SyncContext> sync, PipelineEventCallbacks event_callbacks)
        : INodeBase(std::move(name), std::move(sync), std::move(event_callbacks)) {}

    bool ConnectOutput(std::shared_ptr<OutputLink> output) {
        if (!CanConfigure() || !output) {
            LOG_WARN("Node '{}' ConnectOutput rejected: configurable {}, output valid {}",
                     GetName(),
                     CanConfigure(),
                     static_cast<bool>(output));
            return false;
        }
        output_ = std::move(output);
        LOG_DEBUG("Node '{}' output connected", GetName());
        return true;
    }

protected:
    bool SendOutput(OutT item) { return output_ && output_->Send(std::move(item)); }

private:
    std::shared_ptr<OutputLink> output_;
};

/**
 * @class INode<InT, NoPort>
 * @brief 单输入、零输出的 Sink 终端汇聚节点偏特化
 */
template <typename InT>
class INode<InT, NoPort> : public INodeBase {
public:
    using InputLink = Link<InT>;

    INode(std::string name, std::shared_ptr<SyncContext> sync, PipelineEventCallbacks event_callbacks)
        : INodeBase(std::move(name), std::move(sync), std::move(event_callbacks)) {}

    bool ConnectInput(std::shared_ptr<InputLink> input) {
        if (!CanConfigure() || !input) {
            LOG_WARN("Node '{}' ConnectInput rejected: configurable {}, input valid {}",
                     GetName(),
                     CanConfigure(),
                     static_cast<bool>(input));
            return false;
        }
        input_ = std::move(input);
        LOG_DEBUG("Node '{}' input connected", GetName());
        return true;
    }

protected:
    std::optional<InT> ReceiveInput() { return input_ ? input_->Receive() : std::nullopt; }

private:
    std::shared_ptr<InputLink> input_;
};

/**
 * @class INode<NoPort, NoPort>
 * @brief 无标准数据端口的独立执行节点偏特化
 */
template <>
class INode<NoPort, NoPort> : public INodeBase {
public:
    INode(std::string name, std::shared_ptr<SyncContext> sync, PipelineEventCallbacks event_callbacks)
        : INodeBase(std::move(name), std::move(sync), std::move(event_callbacks)) {}
};

}  // namespace mediacore
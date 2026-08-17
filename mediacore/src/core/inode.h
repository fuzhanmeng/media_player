/**
 * @file inode.h
 * @brief DAG Node 的线程生命周期基类和常用强类型端口模板
 * @details
 * 本文件定义了 DAG 节点的两层架构抽象：
 * 这两部分职责互不影响，因此拆成两个类：
 *
 * INodeBase
 *     1. 负责管理单个工作线程、停止标志、统一的线程循环机制以及退出结果
 *     2. 它自身不持有任何图边（Link），也不在析构函数中尝试隐式停止线程
 *     3. Pipeline 是本图的生命周期协调者，必须按顺序完成 SignalStop、Link Abort、外部 I/O
 *        中断和 Join 后，才能销毁 Node 对象
 *
 * 数据平面 (INode<InT, OutT> 及偏特化):
 *     1. 为常见的单输入/单输出节点形态提供自动化的强类型 Link 管理
 *     2. NoPort 作为一个占位类型，用于触发模板偏特化以表达 Source（无输入）和 Sink（无输出）节点
 *        从而彻底避免编译器尝试实例化非法的 Link<void>
 *     3. 对于多输入或多输出的复杂节点，如 Demuxer，应当直接继承 INodeBase，并自行持有所需的 Link<T>
 *     4. 无论何种拓扑形态，Link 的 Abort 操作始终由 Pipeline 统一调度
 *
 */

#pragma once

#include <atomic>
#include <cassert>  // 运行时断言
#include <cstdint>  // 固定大小整数类型
#include <exception>  // 异常基类
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>  // 标准异常类
#include <string>
#include <thread>
#include <utility>  // std::move

// 私有头：路径相对 mediacore/src（PRIVATE include 目录），不对外暴露
#include "core/link.h"
#include "core/sync_context.h"
#include "mediacore/logger.h"

namespace mediacore {

class Pipeline;

/**
 * @brief Node 工作线程入口函数的可调用类型
 * @details NodeThreadEntry 保存一个无参数、无返回值的可调用对象，通常是绑定具体 Node 的
 *          " ThreadMain() " lambda
 */
using NodeThreadEntry = std::function<void()>;

/**
 * @brief 创建 Node 工作线程的函数类型
 * @details 第一个参数是固定的 Node 名称，第二个参数是必须在线程中执行的入口函数
 * 工厂成功时必须返回 joinable 的 std::thread，不得在调用者线程同步执行入口函数
 * 工厂失败时应抛出异常或返回不可 Join 的 std::thread，并且不能遗留未返回的后台线程
 */
using NodeThreadFactory = std::function<std::thread(const std::string&, NodeThreadEntry)>;

/**
 * @enum NodeState
 * @brief Node 的一次性工作线程状态
 * @details std::uint8_t 指定枚举的底层类型
 */
enum class NodeState : std::uint8_t {
    kCreated,  ///< Node 已经被构造，但是还未创建工作线程
    kRunning,  ///< Node 线程正在创建、执行或者即将退出
    kStopRequested,  ///< Pipeline 已发出停止请求
    kFinished,  ///< 工作线程已经退出，尚未 Join
    kJoined,  ///< 工作线程已经 Join，Node 可以析构
};

/**
 * @enum NodeStartResult
 * @brief Node Start() 的执行结果
 */
enum class NodeStartResult : std::uint8_t {
    kStarted,  ///< 工作线程创建成功
    kAlreadyStarted,  ///< Node 已经启动或已经结束，不能再次启动
    kStopAlreadyRequested,  ///< Node 在启动前已经收到停止请求
    kThreadCreationFailed,  ///< 线程工厂抛出异常或返回了不可 Join 的线程
};

/**
 * @enum NodeJoinResult
 * @brief Node 工作线程回收 Join 的结果
 */
enum class NodeJoinResult : std::uint8_t {
    kJoined,  ///< 成功回收线程
    kNoThread,  ///< Node 没有创建过线程
    kAlreadyJoined,  ///< 工作线程已经被回收
    kCannotJoinSelf,  ///< 工作线程尝试 Join 自己，禁止执行
    kJoinFailed,  ///< std::thread::join() 执行失败
};

/**
 * @enum TickResult
 * @brief 派生 Node 执行一次 Tick() 的结果
 */
enum class TickResult : std::uint8_t {
    kContinue,  ///< 本次处理完成，可以继续下一次 Tick()
    kEndOfStream,  ///< 输入正常结束，需要关闭本 Node 的全部输出
    kStopped,  ///< Node 因取消或停止请求结束
    kError,  ///< Node 遇到不可继续的业务错误
};

/**
 * @enum NodeFinishReason
 * @brief 工作线程退出原因枚举，供 Pipeline 或状态机 (FSM) 进行向上的错误报告与状态反馈
 */
enum class NodeFinishReason : std::uint8_t {
    kNotFinished,  ///< 工作线程尚未退出
    kEndOfStream,  ///< 流结束了
    kStopRequested,  ///< Pipeline 请求停止，或 Tick() 报告 kStopped
    kError,  ///< Tick() 报告 kError
    kUnhandledException,  ///< 工作线程入口捕获了未由派生 Node 处理的 C++ 异常
};

/**
 * @brief 将 Node 生命周期状态转换为日志可读字符串
 */
inline const char* NodeStateToString(NodeState state) {
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
 * @brief 将 Node 启动结果转换为日志可读字符串
 */
inline const char* NodeStartResultToString(NodeStartResult result) {
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
 * @brief 将 Node Join 结果转换为日志可读字符串
 */
inline const char* NodeJoinResultToString(NodeJoinResult result) {
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
 * @brief 将 Tick 返回结果转换为日志可读字符串
 */
inline const char* TickResultToString(TickResult result) {
    switch (result) {
        case TickResult::kContinue:
            return "Continue";
        case TickResult::kEndOfStream:
            return "EndOfStream";
        case TickResult::kStopped:
            return "Stopped";
        case TickResult::kError:
            return "Error";
    }

    return "Unknown";
}

/**
 * @brief 将 Node 线程退出原因转换为日志可读字符串
 */
inline const char* NodeFinishReasonToString(NodeFinishReason reason) {
    switch (reason) {
        case NodeFinishReason::kNotFinished:
            return "NotFinished";
        case NodeFinishReason::kEndOfStream:
            return "EndOfStream";
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
 * @brief 只负责线程生命周期的 Node 基类
 */
class INodeBase {
public:
    /**
     * @brief 构造一个未启动的 Node 实例
     * @param name 节点名
     * @param sync Pipeline 创建并共享给全部 Node 的同步对象
     * @throws std::invalid_argument 当 sync 为空时抛出
     */
    INodeBase(std::string name, std::shared_ptr<SyncContext> sync)
        : sync_ctx_(std::move(sync)), name_(std::move(name)) {
        if (!sync_ctx_) {
            LOG_ERROR("Node '{}' construction failed: null SyncContext", name_);
            throw std::invalid_argument("INodeBase requires a SyncContext");
        }

        LOG_INFO("Node '{}' constructed", name_);
    }

    /**
     * @brief 析构 Node
     * @details
     * 本函数只检查线程是否已经由 Pipeline 回收，不会隐式停止或 Join，调试构建中，违反前置条件会触发
     * assert；关闭 assert 的构建中，joinable 的 std::thread 随后仍会导致 std::terminate()
     */
    virtual ~INodeBase() {
        if (thread_.joinable()) {
            LOG_ERROR("Node '{}' destroyed while thread is still joinable", name_);
        } else {
            LOG_INFO("Node '{}' destroyed", name_);
        }

        assert(!thread_.joinable() && "Pipeline must join every Node before destruction");
    }

    /** @brief Node 拥有线程和同步状态，禁止复制 */
    INodeBase(const INodeBase&) = delete;

    /** @brief Node 拥有线程和同步状态，禁止复制赋值 */
    INodeBase& operator=(const INodeBase&) = delete;

    /** @brief Node 的地址会被工作线程捕获，禁止移动 */
    INodeBase(INodeBase&&) = delete;

    /** @brief Node 的地址会被工作线程捕获，禁止移动赋值 */
    INodeBase& operator=(INodeBase&&) = delete;

    /**
     * @brief 查询当前 Node 是否已经收到停止请求
     * @return true 表示已经请求停止
     */
    bool IsStopRequested() const noexcept { return stop_requested_.load(); }

    /**
     * @brief 查询 Node 当前状态
     * @return NodeState 快照
     */
    NodeState GetState() const noexcept { return state_.load(); }

    /**
     * @brief 查询线程最终退出原因
     * @return 线程未退出时返回 kNotFinished
     */
    NodeFinishReason GetFinishReason() const noexcept { return finish_reason_.load(); }

    /**
     * @brief 查询 Node 名称
     * @return name_ 的只读引用
     */
    const std::string& GetName() const noexcept { return name_; }

    /**
     * @brief 返回工作线程捕获的未处理 C++ 异常
     * @return 没有捕获异常时返回空 std::exception_ptr
     * @throws std::logic_error Node 尚未 Join 时抛出
     */
    std::exception_ptr GetUnhandledException() const {
        if (state_.load(std::memory_order_acquire) != NodeState::kJoined) {
            throw std::logic_error("GetUnhandledException requires a joined Node");
        }

        return unhandled_exception_;
    }

protected:
    /**
     * @brief 执行一次具体媒体处理
     * @return 本轮 Tick 的结果
     * @details
     * 这个函数由 ThreadMain() 调用，派生类只重写 Tick()，不重写 ThreadMain()
     * 例如：
     * - Demuxer: 读一个 AVPacket
     * - Decoder: 解一个 Packet
     * - Sink: 消费一个 Frame
     */
    virtual TickResult Tick() = 0;

    /**
     * @brief 正常 EOF 时，关闭当前 Node 的全部输出 Link
     * @details
     * ThreadMain() 只在 Tick() 返回 kEndOfStream，并且没有 Stop 时调用它
     * 每个 Node 的输出形态不一样，所以这一步由派生类自己实现
     */
    virtual void CloseOutputs() = 0;

    /**
     * @brief 判断当前 Node 有没有要优先处理的控制命令
     * @return true 表示有控制命令，false 表示没有
     * @details
     * 默认没有控制命令，只有 Demuxer 这类节点有，比如 Seek 请求，Pause 期间需要被唤醒
     */
    virtual bool HasPendingControl() const noexcept { return false; }

    /**
     * @brief 执行当前 Node 自己的控制命令
     * @return 本次控制命令的处理结果
     * @details
     * 只有真正拥有控制命令的 Node 才会覆盖它
     * 例如 Demuxer 可以在这里做：
     * - 取出待处理 Seek
     * - 调用 av_seek_frame()
     * - 调用 avformat_flush()
     * - 发出 kFlush Packet 给下游
     */
    virtual TickResult ProcessPendingControl() { return TickResult::kContinue; }

    /**
     * @brief 判断当前 Node 是否还能继续配置 Link
     * @return 只有在 kCreated 状态才允许连接
     */
    bool CanConfigure() const noexcept { return state_.load() == NodeState::kCreated; }

    /**
     * @brief 判断一个数据项是不是旧世代的数据
     * @param item_epoch 数据项携带的 epoch
     * @return true 表示这条数据已经过期，应当丢弃
     */
    bool ShouldDropStaleItem(std::uint64_t item_epoch) const noexcept { return item_epoch < sync_ctx_->GetEpoch(); }

    /**
     * @brief 读取当前 epoch
     * @return SyncContext 里保存的 epoch
     */
    std::uint64_t GetCurrentEpoch() const noexcept { return sync_ctx_->GetEpoch(); }

    /**
     * @brief 中断不经过 Link 的阻塞式外部 I/O
     * @details
     * Link 的阻塞由 Link::Abort() 解除。
     * 这个函数只管 Link 之外的阻塞，比如 av_read_frame()、socket recv()、音频设备写入
     */
    virtual void OnInterruptBlockingIo() noexcept {}

    /// Pipeline 注入的共享运行控制对象；Node 只读它的状态和 epoch
    std::shared_ptr<SyncContext> sync_ctx_;

private:
    friend class Pipeline;

    /**
     * @brief 启动工作线程
     * @param thread_factory Pipeline 传入的函数（Lambda），负责真正去调用操作系统的 API 创建线程
     * @return 启动结果状态码
     *
     * @details
     * 1. 先用 CAS 将 kCtreated 改成 kRunning
     * 2. 如果已经启动过了，或者收到了停止请求，就直接返回
     * 3. 调用线程工厂创建真正的线程
     * 4. 如果线程创建失败，把状态回滚到 kCreated
     */
    NodeStartResult Start(const NodeThreadFactory& thread_factory) noexcept {
        LOG_INFO("Node '{}' start requested, state {}", name_, NodeStateToString(state_.load()));

        // 1. 预期当前状态为初始化完成且未启动 (kCreated)
        NodeState expected = NodeState::kCreated;

        // 2. CAS 原子操作
        // if state_ == expected，则将 state_ 更新为 kRunning 并返回 true
        // if state_ != expected，说明已经被其他操作更改
        // 此时 CAS 失败返回 false，并将 expected 覆盖为 state_ 实际的当前值
        if (!state_.compare_exchange_strong(expected, NodeState::kRunning)) {
            // 检查 CAS 失败的具体原因
            if (expected == NodeState::kStopRequested) {
                // 节点在启动前已接收到 Pipeline 的异步停止信令，拒绝启动
                LOG_WARN("Node '{}' start rejected: stop already requested", name_);
                return NodeStartResult::kStopAlreadyRequested;
            }

            // 其他非 kCreated 状态（如 kRunning, kFinished）均视为已启动过
            LOG_WARN("Node '{}' start rejected: current state {}", name_, NodeStateToString(expected));
            return NodeStartResult::kAlreadyStarted;
        }

        // 3. state_ == kRunning
        try {
            // 调用外部注入的工厂去真正开线程，让线程去跑 ThreadMain() 循环
            thread_ = thread_factory(name_, [this]() { ThreadMain(); });

            // 验证系统线程资源是否有效分配
            if (!thread_.joinable()) {
                // 资源分配失败，必须回滚状态，避免当前 Node 永久停留在虚假的 kRunning 状态
                state_.store(NodeState::kCreated);
                LOG_ERROR("Node '{}' thread creation returned non-joinable thread", name_);
                return NodeStartResult::kThreadCreationFailed;
            }
        } catch (...) {
            // 捕获线程创建时可能抛出的异常（如 std::system_error 内存不足）
            // 执行状态回滚，拦截异常以满足 noexcept 承诺
            state_.store(NodeState::kCreated);
            LOG_ERROR("Node '{}' thread creation threw an exception", name_);
            return NodeStartResult::kThreadCreationFailed;
        }

        LOG_INFO("Node '{}' started", name_);
        return NodeStartResult::kStarted;
    }

    /**
     * @brief 异步发送停止信号：阶段一
     * @details
     * 这一步只做 "投递停止请求"，不负责等线程真的退出
     * 执行顺序：
     * 1. 先写 stop_requested_；
     * 2. 再把状态推进到 kStopRequested；
     * 3. 后续由 InterruptBlockingIo() 和 Join() 完成真正收尾
     */
    void SignalStop() noexcept {
        LOG_INFO("Node '{}' stop signal requested, state {}", name_, NodeStateToString(state_.load()));

        // 1. 投递原子退出标志
        stop_requested_.store(true);

        // 2. 获取现在的 state_ 状态
        NodeState current = state_.load();

        // 若节点已处于终止序列（停止请求中、已结束、已回收）
        while (current != NodeState::kStopRequested && current != NodeState::kFinished &&
               current != NodeState::kJoined &&
               // 状态比较与交换：若当前状态仍为 current，则更新为 kStopRequested
               !state_.compare_exchange_strong(current, NodeState::kStopRequested)) {
            // CAS 失败时，current 会被自动更新为内存中的最新值，继续下一轮评估
        }

        LOG_DEBUG("Node '{}' stop signal published, state {}", name_, NodeStateToString(state_.load()));
    }

    /**
     * @brief 终端可能卡住的外部 I/O
     * @details
     * 没有创建工作线程，或者线程已经 Join 时，不需要中断 I/O
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
     * @brief 回收工作线程：阶段三
     * @return Join 的执行结果
     * @details
     * 没有创建线程的 Node 也会被记为 kJoined，使 Pipeline 可以使用同一套析构前检查
     * 禁止工作线程 Join 自己，否则会死锁；std::thread::join() 成功返回后，控制线程可以读取
     * 工作线程退出前写入的 unhandled_exception_
     */
    NodeJoinResult Join() noexcept {
        LOG_INFO("Node '{}' join requested, state {}", name_, NodeStateToString(state_.load()));

        // 1. 已经完成线程回收
        if (state_.load() == NodeState::kJoined) {
            LOG_DEBUG("Node '{}' already joined", name_);
            return NodeJoinResult::kAlreadyJoined;
        }

        // 2. 没有可回收线程
        if (!thread_.joinable()) {
            state_.store(NodeState::kJoined);
            LOG_DEBUG("Node '{}' has no thread to join", name_);
            return NodeJoinResult::kNoThread;
        }

        // 3. 防止工作线程 Join 自己
        if (thread_.get_id() == std::this_thread::get_id()) {
            LOG_ERROR("Node '{}' cannot join itself", name_);
            return NodeJoinResult::kCannotJoinSelf;
        }

        // 4. 回收线程
        try {
            thread_.join();
        } catch (...) {
            LOG_ERROR("Node '{}' thread join threw an exception", name_);
            return NodeJoinResult::kJoinFailed;
        }

        state_.store(NodeState::kJoined);
        LOG_INFO("Node '{}' joined, finish reason {}", name_, NodeFinishReasonToString(finish_reason_.load()));
        return NodeJoinResult::kJoined;
    }

    /**
     * @brief Node 工作线程统一入口
     * @details
     * 1. 先检查 Stop
     * 2. 再次检查是否需要处理控制命令，如 Seek
     * 3. 有控制命令，先处理控制命令；没有的话去处理 Pause
     * 4. 处理 Tick，根据Tick的返回结果决定是继续还是正常结束/错误退出
     * 5. 所有异常都通过 catch 抓到
     */
    void ThreadMain() noexcept {
        LOG_INFO("Node '{}' thread entered", name_);

        NodeFinishReason reason = NodeFinishReason::kNotFinished;

        try {
            while (true) {
                // Step 1: Stop 的优先级最高
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                // Step 2: 先处理控制命令
                // 只有 Demuxer 这类 Node 才可能进入这里，普通 Node 默认没有控制命令
                if (HasPendingControl()) {
                    const TickResult control_result = ProcessPendingControl();

                    // 控制命令处理完，回到循环顶部
                    if (control_result == TickResult::kContinue) {
                        continue;
                    }

                    LOG_DEBUG("Node {} control returned {}", name_, TickResultToString(control_result));

                    // 正常结束
                    if (control_result == TickResult::kEndOfStream) {
                        CloseOutputs();
                        reason = NodeFinishReason::kEndOfStream;
                        break;
                    }

                    // 如果是被关闭了 Stop
                    if (control_result == TickResult::kStopped) {
                        reason = NodeFinishReason::kStopRequested;
                        break;
                    }

                    reason = NodeFinishReason::kError;
                    break;
                }

                // Step 3: Pause 时等待
                sync_ctx_->WaitWhilePaused([this]() { return IsStopRequested() || HasPendingControl(); });

                // 被 Stop 唤醒后，不再进入 Tick()
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                // 如果是因为控制命令唤醒
                if (HasPendingControl()) {
                    continue;
                }

                // 再确认一次状态，避免刚检查完又被切回 Paused
                if (sync_ctx_->GetState() == PipelineRunState::kPaused) {
                    continue;
                }

                // Step 4: 真正执行一次具体 Node 的媒体处理
                const TickResult result = Tick();

                // Step 5: Tick 结束后，再检查一次 Stop
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                if (result == TickResult::kContinue) {
                    continue;
                }

                LOG_DEBUG("Node '{}' Tick returned {}", name_, TickResultToString(result));

                if (result == TickResult::kEndOfStream) {
                    CloseOutputs();
                    reason = NodeFinishReason::kEndOfStream;
                    break;
                }

                if (result == TickResult::kStopped) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                reason = NodeFinishReason::kError;
                break;
            }
        } catch (...) {
            // Step 6: 不能让异常越过 std::thread 入口
            unhandled_exception_ = std::current_exception();
            reason = NodeFinishReason::kUnhandledException;
            LOG_ERROR("Node '{}' thread caught unhandled exception", name_);
        }

        // 所有退出路径都从这里统一收尾，避免漏写状态和退出原因
        finish_reason_.store(reason);
        state_.store(NodeState::kFinished);
        LOG_INFO("Node '{}' thread exited, reason {}", name_, NodeFinishReasonToString(reason));
    }

    /// 日志和拓扑诊断使用的固定 Node 名称
    std::string name_;

    /// Pipeline 创建并 Join 的工作线程句柄
    std::thread thread_;

    /// Pipeline 控制线程和 Node 工作线程共同更新的生命周期状态
    std::atomic<NodeState> state_{NodeState::kCreated};

    /// Pipeline 写入、工作线程读取的停止请求标志
    std::atomic<bool> stop_requested_{false};

    /// 工作线程写入、其他线程读取的最终退出原因
    std::atomic<NodeFinishReason> finish_reason_{NodeFinishReason::kNotFinished};

    /// 工作线程捕获的异常；只能在成功 Join 后读取
    std::exception_ptr unhandled_exception_;
};

/**
 * @struct NoPort
 * @brief 模板元编程占位符：明确标识缺失的数据端口以触发模板偏特化
 */
struct NoPort final {};

/**
 * @class INode
 * @brief 持有一个输入 Link 和一个输出 Link 的 Node 模板
 * @tparam InT  输入数据项类型（如 std::shared_ptr<Packet>）
 * @tparam OutT 输出数据项类型（如 std::shared_ptr<VideoFrame>）
 * @details
 * 这个模板只负责把 Link 接好，线程生命周期仍然由 INodeBase 管
 * @par 适用范围
 *   一进一出的节点：VideoDecoderNode / AudioDecoderNode / 单输入的 AVSyncNode 等
 *
 * @par 不适用范围（直接继承 INodeBase）
 *   - 一进双出：FfmpegDemuxer（video/audio 双输出）
 *   - 双进一出：Mp4MuxerNode（video/audio 双输入）
 *   - 一进多出：TeeNode（多播）
 *   - 零进一出：源节点（FileSourceNode / HttpSourceNode，无 input_）
 *   - 一进零出：汇节点（VideoSinkNode / AudioSinkNode / Mp4MuxerNode，无 output_）
 *
 */
template <typename InT, typename OutT>
class INode : public INodeBase {
public:
    using InputLink = Link<InT>;  ///< 输入管道类型别名
    using OutputLink = Link<OutT>;  ///< 输出管道类型别名

    /**
     * @brief 构造一个一进一出的 Node
     * @param name 节点名
     * @param sync Pipeline 共享同步对象
     */
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

    /**
     * @brief 连接输入 Link，作为本 Node 的输入
     */
    bool ConnectInput(std::shared_ptr<InputLink> input) {
        // 只能在 Node 启动前连接，并拒绝空 Link
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
     * @brief 连接输出 Link，作为本 Node 的输出
     */
    bool ConnectOutput(std::shared_ptr<OutputLink> output) {
        // 只能在 Node 启动前连接，并拒绝空 Link
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
     * @brief 从输入 Link 阻塞接收一个数据项
     * @return 接收到数据时，返回有值的 std::optional，未连接、EOF 或者 Abort 时返回 nullopt
     */
    std::optional<InT> ReceiveInput() { return input_ ? input_->Receive() : std::nullopt; }

    /**
     * @brief 向输出 Link 阻塞发送一个数据项
     * @param item 要发送的数据项
     * @return 发送成功返回 true；未连接、Link 已关闭或已 Abort 时返回 false
     */
    bool SendOutput(OutT item) { return output_ && output_->Send(std::move(item)); }

    /**
     * @brief 正常关闭一进一出 Node 的全部输出
     * @details
     * 一进一出节点只有一个输出 Link，所以这里只需要关闭 output_
     */
    void CloseOutputs() final {
        if (output_) {
            LOG_INFO("Node '{}' closing output link", GetName());
            output_->Close();
        } else {
            LOG_DEBUG("Node '{}' has no output link to close", GetName());
        }
    }

private:
    /// 上游与本 Node 共享的输入 Link
    std::shared_ptr<InputLink> input_;

    /// 本 Node 与下游共享的输出 Link
    std::shared_ptr<OutputLink> output_;
};

/**
 * @class INode<NoPort, OutT>
 * @brief 持有一个输出 Link、没有输入 Link 的 Source Node 模板
 * @tparam OutT 输出 Link 传递的数据类型
 * @details 本模板拥有输出 Link 的 shared_ptr，不拥有输入端口，并实现 Source 的 CloseOutputs()
 */
template <typename OutT>
class INode<NoPort, OutT> : public INodeBase {
public:
    using OutputLink = Link<OutT>;  ///< 输出 Link 类型

    /**
     * @brief 构造一个零进一出的 Source Node
     * @param name 节点名
     * @param sync Pipeline 共享同步对象
     */
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

    /**
     * @brief 连接本 Node 到下游的输出 Link
     * @param output Pipeline 创建的非空输出 Link
     * @return Node 尚未启动且 output 非空时返回 true，否则返回 false
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
     * @brief 向输出 Link 阻塞发送一个数据项
     * @param item 要发送的数据项
     * @return 发送成功返回 true；未连接、Link 已关闭或已 Abort 时返回 false
     */
    bool SendOutput(OutT item) { return output_ && output_->Send(std::move(item)); }

    /**
     * @brief 正常关闭 Source 的全部输出
     * @details Source 没有输入端口，因此没有 CloseInput()；正常结束只需要关闭它发送数据的 Link
     * Source 自身的收尾应在 Tick() 返回 kEndOfStream 前完成
     */
    void CloseOutputs() final {
        if (output_) {
            LOG_INFO("Node '{}' closing output link", GetName());
            output_->Close();
        } else {
            LOG_DEBUG("Node '{}' has no output link to close", GetName());
        }
    }

private:
    /// 本 Node 与下游共享的输出 Link
    std::shared_ptr<OutputLink> output_;
};

/**
 * @class INode<InT, NoPort>
 * @brief 持有一个输入 Link、没有输出 Link 的 Sink Node 模板
 * @tparam InT 输入 Link 传递的数据类型
 * @details 本模板拥有输入 Link 的 shared_ptr，不拥有输出端口；CloseOutputs() 是空操作
 */
template <typename InT>
class INode<InT, NoPort> : public INodeBase {
public:
    using InputLink = Link<InT>;

    /**
     * @brief 构造函数，构造一个一进零出形态的 Node
     * @param name 节点名称
     * @param sync Pipeline 共享同步对象
     */
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

    /**
     * @brief 连接上游 Link 作为本 Node 的输入
     * @param input Pipeline 创建的非空 Link
     * @return Node 尚未启动且 input 非空返回 true
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

protected:
    /**
     * @brief 从输入 Link 阻塞接收一个数据项
     * @return 接收到数据返回 std::optional<InT>，接收不到数据返回 std::nullopt
     */
    std::optional<InT> ReceiveInput() { return input_ ? input_->Receive() : std::nullopt; }

    /**
     * @brief Sink 没有输出 Link，正常 EOF 时无需关闭任何对象
     */
    void CloseOutputs() final { LOG_TRACE("Node '{}' has no output links to close", GetName()); }

private:
    /// 上游与本 Node 共享的输入 Link
    std::shared_ptr<InputLink> input_;
};

/**
 * @class INode<NoPort, NoPort>
 * @brief 不使用输入或输出 Link 的后台 Node 模板
 * @details 本模板不拥有 Link；派生类仍由 Pipeline 负责启动、停止和 Join
 */
template <>
class INode<NoPort, NoPort> : public INodeBase {
public:
    /**
     * @brief 构造一个无端口 Node
     * @param name Node 固定名称
     * @param sync Pipeline 共享同步对象
     */
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

protected:
    /**
     * @brief 无端口 Node 在正常 EOF 时无需关闭任何对象
     */
    void CloseOutputs() final { LOG_TRACE("Node '{}' has no output links to close", GetName()); }
};

}  // namespace mediacore

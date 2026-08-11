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
 * @brief 与媒体数据类型无关的单线程 Node 基类
 * @details 职责：
 * Pipeline 在控制线程中依次调用 Start()、SignalStop()、InterruptBlockingIo() 和 Join()
 * Node 工作线程执行 ThreadMain() 和 Tick()；正常 EOF 时 ThreadMain() 调用 CloseOutputs() 将 EOF
 * 传给下游，本类拥有 std::thread，Node 析构函数不负责停止线程；Pipeline 必须先完成 Join
 *
 * CloseOutputs() 是纯虚函数，表示所有具体 Node 都必须定义 "正常 EOF 时如何关闭输出" 的行为
 * 纯虚函数是接口要求，不提供可直接使用的基类实现，只要类还没有实现全部纯虚函数，它就是抽象类，
 * 不能创建对象；派生类必须使用完全相同的函数名、参数和限定符进行覆盖，常用端口模板会提供默认实现，
 * 多输入或多输出 Node 则必须直接继承本类并自行实现
 */
class INodeBase {
public:
    /**
     * @brief 构造函数，初始化尚未启动的 Node 实例
     * @param name 节点名，用于诊断日志追踪与拓扑排错的固定标识符
     * @param sync Pipeline 创建并共享给全部 Node 的同步对象
     * @throws std::invalid_argument 当传入的 sync 为 nullptr 时触发
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
     * @brief 析构函数，销毁 Node
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
     * @brief 查询 Pipeline 是否已经请求停止当前 Node
     * @return 已请求停止返回 true
     */
    bool IsStopRequested() const noexcept { return stop_requested_.load(); }

    /**
     * @brief 返回当前 Node 生命周期状态
     * @return 原子读取的状态快照
     */
    NodeState GetState() const noexcept { return state_.load(); }

    /**
     * @brief 返回工作线程退出原因
     * @return 线程未退出时返回 kNotFinished
     */
    NodeFinishReason GetFinishReason() const noexcept { return finish_reason_.load(); }

    /**
     * @brief 返回 Node 固定名称
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
     * @return 本次处理的 TickResult
     * @details 若 Tick() 调用阻塞式第三方 I/O，派生类必须实现 OnInterruptBlockingIo()，保证
     * Pipeline::Stop() 能使调用返回
     */
    virtual TickResult Tick() = 0;

    /**
     * @brief 正常 EOF 时关闭当前 Node 的全部输出 Link
     * @details
     * ThreadMain() 只在 Tick() 返回 kEndOfStream 且没有收到停止请求时调用本函数
     * 这是纯虚函数：每个可实例化的具体 Node 都必须提供实现，函数名即使只少一个复数后缀也不会覆盖
     * 本接口，名称、参数和限定符必须完全一致
     */
    virtual void CloseOutputs() = 0;

    /**
     * @brief 中断 Link 之外的阻塞式 I/O
     * @details
     * Pipeline 控制线程可能在工作线程刚创建后立即调用本函数，也可能与 Tick() 并发调用
     * 覆盖实现只能设置取消标志、调用线程安全的 shutdown() 或使用库提供的取消接口，不能释放
     * Tick() 仍可能访问的媒体对象
     */
    virtual void OnInterruptBlockingIo() noexcept {}

    /**
     * @brief 判断当前状态是否允许连接 Link
     * @return Node 尚未启动时返回 true，否则返回 false
     */
    bool CanConfigure() const noexcept { return state_.load() == NodeState::kCreated; }

    /**
     * @brief 判断媒体项是否属于已经失效的数据世代
     * @param item_epoch 数据项携带的 epoch
     * @return item_epoch 小于当前 epoch 时返回 true
     * @note 这里只比较标记，不会自动清空 Link，也不会刷新解码器或设备
     */
    bool ShouldDropStaleItem(std::uint64_t item_epoch) const noexcept { return item_epoch < sync_ctx_->GetEpoch(); }

    /**
     * @brief 获取当前数据世代号
     * @return SyncContext 中保存的 epoch
     */
    std::uint64_t GetCurrentEpoch() const noexcept { return sync_ctx_->GetEpoch(); }

    /// 由 Pipeline 创建并注入的本图运行控制对象；Node 只读取状态和等待暂停结束
    std::shared_ptr<SyncContext> sync_ctx_;

private:
    friend class Pipeline;

    /**
     * @brief 使用 Pipeline 提供的线程工厂启动底层工作线程
     * @param thread_factory Pipeline 传入的函数（Lambda），负责真正去调用操作系统的 API 创建线程
     * @return 启动结果状态码（成功、已经启动过、已经收到停止信号，或者线程创建失败）
     *
     * @details
     * 1. 先用 CAS 把状态从 kCreated 改为 kRunning，防止同一个 Node 被重复启动或在停止请求后启动
     * 2. 如果线程工厂抛出异常或返回不可 Join 的线程，状态回滚到 kCreated，交由 Pipeline 统一回滚
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
     * @brief 异步发送停止信令：阶段一
     * @details 本函数仅负责原子化地投递停止信号并推进状态机，不阻塞等待底层 OS 线程物理退出
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
     * @brief 调用派生类的外部 I/O 中断函数
     * @details 只有成功创建过且尚未 Join 的线程才需要中断，因此先检查 thread_.joinable()
     * 线程创建后可能还没有进入 Tick() 就收到停止请求，OnInterruptBlockingIo() 也必须能安全调用
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
     * @brief 等待工作线程退出并回收线程资源：阶段三
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
     * @brief 工作线程入口，重复执行暂停等待和 Tick()，并记录最终退出原因
     * @details
     * 每次 Tick() 前检查停止，避免暂停等待被唤醒后又开始处理数据；Tick() 返回后再次检查停止，
     * 避免 Pipeline 已经要求立即停止时，把 Link Abort 或外部 I/O 中断误报成正常 EOF
     * 所有未被派生类处理的 C++ 异常都在本函数中捕获，不能越过线程入口
     */
    void ThreadMain() noexcept {
        LOG_INFO("Node '{}' thread entered", name_);
        NodeFinishReason reason = NodeFinishReason::kNotFinished;

        try {
            while (true) {
                // 1. Pause 时等待；每个 Node 自己的停止标志是条件变量的额外退出谓词
                sync_ctx_->WaitWhilePaused([this]() { return IsStopRequested(); });

                // 2. 被 Stop 唤醒后不再进入 Tick()
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                // 3. 执行一轮具体 Node 的媒体处理
                const TickResult result = Tick();

                // 4. Stop 可能与 Tick() 并发发生停止请求优先于 Tick() 返回的业务结果
                if (IsStopRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                // 5. 根据 Tick() 的结果决定继续处理还是结束线程
                if (result == TickResult::kContinue) {
                    continue;
                }

                LOG_DEBUG("Node '{}' Tick returned {}", name_, TickResultToString(result));

                if (result == TickResult::kEndOfStream) {
                    // EOF 是正常结束：先关闭本 Node 的所有输出 Link，让下游排空后收到 EOF
                    CloseOutputs();
                    reason = NodeFinishReason::kEndOfStream;
                    // 不能继续 while，否则会在 EOF 后再次调用 Tick()，可能空转或重复阻塞
                    break;
                }

                if (result == TickResult::kStopped) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                // kError 或未来新增但尚未处理的结果都按错误结束
                reason = NodeFinishReason::kError;
                break;
            }
        } catch (...) {
            // 在线程入口保存未处理异常，避免异常越过线程边界触发 std::terminate()
            unhandled_exception_ = std::current_exception();
            reason = NodeFinishReason::kUnhandledException;
            LOG_ERROR("Node '{}' thread caught unhandled exception", name_);
        }

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
 * @details 基类 INodeBase 管线程生命周期，本模板叠加 "数据怎么进、怎么出"：
 *          - 持有强类型 Link（input_ / output_）
 *          - 提供连接接口（ConnectInput / ConnectOutput），供组图阶段调用
 *          - 实现 INodeBase::CloseOutputs()；派生 Node 仍必须实现 INodeBase::Tick()
 *
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
     * @brief 连接上游 Link，作为本 Node 的输入
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
     * @brief 连接本 Node，到下游的输出 Link
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
     * 这个模板只有一条输出 Link，所以 "全部输出" 实际就是 output_，没有 CloseInput()：输入 Link
     * 的正常关闭权属于上游发送方，本 Node 作为接收方只负责 Receive() 到排空后的 nullopt
     * Tick() 返回 kEndOfStream 前，派生 Node 应先完成自己的 drain 或资源收尾；这里的实现只负责
     * 把 EOF 传给下游， final 表示所有使用该模板的 Node 都使用同一套 Link 关闭规则
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

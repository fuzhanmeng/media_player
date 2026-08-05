/**
 * @file inode.h
 * @brief 节点线程生命周期基类及基础输入/输出形态的强类型便利模板
 * @details
 * 本文件定义了 DAG 节点的两层架构抽象：
 *
 * 这两部分职责互不影响，因此拆成两个类：
 *
 * INodeBase
 *     1. 负责管理单个工作线程、停止标志、统一的线程循环机制以及退出结果
 *     2. 它自身不持有任何图边（Link），也不在析构函数中尝试隐式停止线程
 *     3. Pipeline 是全局唯一的生命周期协调者，必须按严格顺序完成 SignalStop / Link Abort / 外部
 *        IO Interrupt 和 Join 操作后，方可销毁 Node 对象
 *
 * 数据平面 (INode<InT, OutT> 及偏特化):
 *     1. 为常见的单输入/单输出节点形态提供自动化的强类型 Link 管理
 *     2. NoPort 作为一个占位类型，用于触发模板偏特化以表达 Source（无输入）和 Sink（无输出）节点
 *        从而彻底避免编译器尝试实例化非法的 Link<void>
 *     3. 对于多输入或多输出的复杂节点,如 Demuxer，应当直接继承 INodeBase,并自行持有所需的 Link<T>
 *     4. 无论何种拓扑形态，Link 的 Abort 操作始终由 Pipeline 统一集中调度
 *
 */

#pragma once

#include <atomic>
#include <cassert> // 运行时断言
#include <cstdint> // 固定大小整数类型
#include <exception> // 异常基类
#include <memory>
#include <optional>
#include <stdexcept> // 标准异常类
#include <string>
#include <system_error> // 系统错误处理
#include <thread>
#include <utility> // std::move

// 私有头：路径相对 mediacore/src（PRIVATE include 目录），不对外暴露
#include "core/link.h"
#include "core/sync_context.h"

namespace mediacore {

class Pipeline;

/**
 * @enum NodeState
 * @brief Node 对象内部状态机枚举，用于标示一次性的线程生命周期状态
 * @details std::uint8_t 指定枚举的底层类型
 */
enum class NodeState : std::uint8_t {
    kCreated, // 初始态
    kRunning, // 运行态
    kStopRequested, // 停止挂起态
    kFinished, // 逻辑终止态
    kJoined, // 资源回收态
};

/**
 * @enum NodeStartResult
 * @brief Start() 调用的可观测返回结果，明确标识 Node 实例不支持重复启动。
 */
enum class NodeStartResult : std::uint8_t {
    kStarted,
    kAlreadyStarted,
    kStopAlreadyRequested,
    kThreadCreationFailed,
};

/**
 * @enum NodeJoinResult
 * @brief Pipeline 回收 Node 底层线程时的状态码枚举
 */
enum class NodeJoinResult : std::uint8_t {
    kJoined,
    kNoThread,
    kAlreadyJoined,
    kCalledFromWorkerThread,
    kJoinFailed,
};

/**
 * @enum TickResult
 * @brief 派生类单次执行负载 (Tick) 的返回结果枚举，提供比布尔值更丰富的终止原因语义
 */
enum class TickResult : std::uint8_t {
    kContinue,
    kEndOfStream,
    kStopped,
    kError,
};

/**
 * @enum NodeFinishReason
 * @brief 工作线程退出原因枚举，供 Pipeline 或状态机 (FSM) 进行向上的错误报告与状态反馈
 */
enum class NodeFinishReason : std::uint8_t {
    kNotFinished,
    kEndOfStream,
    kStopRequested,
    kError,
    kUnhandledException,
};

/**
 * @class INodeBase
 * @brief 与具体数据载荷类型无关的节点线程执行器
 * @details 职责：
 * 严格限定生命周期状态跃迁路径为：Created -> Running -> Finished -> Joined
 * 亦可在任意活动阶段接收停止指令，跃迁为：StopRequested -> Finished -> Joined
 * 控制域方法的调用约束：Start / SignalStop / InterruptBlockingIo / Join
 * 必须且只能由持有该节点的 Pipeline 在主控线程中串行调用
 * 工作线程内部仅执行 ThreadMain 骨架与多态函数 Tick
 */
class INodeBase {
public:
    /**
     * @brief 构造函数，初始化尚未启动的 Node 实例
     * @param name 节点名，用于诊断日志追踪与拓扑排错的固定标识符
     * @param sync 由 Pipeline 实例化并注入的全局共享同步总线，严禁为空
     * @throws std::invalid_argument 当传入的 sync 为 nullptr 时触发
     */
    INodeBase(std::string name, std::shared_ptr<SyncContext> sync)
        : sync_ctx_(std::move(sync)), name_(std::move(name)) {
        if (!sync_ctx_) {
            throw std::invalid_argument("INodeBase requires a SyncContext");
        }
    }

    /**
     * @brief 析构函数，断言生命周期已被正确回收
     * @details 若违反析构前置条件（即线程仍在 joinable 状态），调试模式下将立即触发 assert 终止；
     *          Release 模式下 std::thread 的析构仍会抛出异常，强制暴露架构级的生命周期管理违规
     */
    virtual ~INodeBase() { assert(!thread_.joinable() && "Pipeline must be join every Node before destruction"); }

    // 禁止拷贝和移动，节点持有线程与同步原语，语义上不可赋值
    INodeBase(const INodeBase&) = delete;
    INodeBase& operator=(const INodeBase&) = delete;
    INodeBase(INodeBase&&) = delete;
    INodeBase& operator=(INodeBase&&) = delete;

private:
    friend Pipeline;

    /**
     * @brief 分配系统线程资源并启动工作线程（非阻塞）
     * @details 线程创建与状态跃迁结果；若系统资源不足导致异常，状态将原子回滚至 kCreated
     * @note 采用 CAS (Compare-And-Swap) 保障并发幂等性
     */
    NodeStartResult Start() {
        // 预期当前节点的原子状态 state_ 是处于初始化完成但未启动的 kCreated 状态
        NodeState expected = NodeState::kCreated;

        // 如果 state_ 的状态 = expected(NodeState::kCreated)，那么 state_  == NodeState::kRunning
        if (!state_.compare_exchange_strong(expected, NodeState::kRunning)) {
            // 如果交换失败
            return expected == NodeState::kStopRequested ? NodeStartResult::kStopAlreadyRequested
                                                         : NodeStartResult::kAlreadyStarted;
        }

        try {
            thread_ = std::thread(&INodeBase::ThreadMain, this);
        } catch (const std::system_error&) {
            state_.store(NodeState::kCreated);
            return NodeStartResult::kThreadCreationFailed;
        }

        return NodeStartResult::kStarted;
    }

    /**
     * @brief Pipeline 两阶段停止协议的第一步：无阻塞停止信令
     * @details 本操作通过 memory_order 保证内存可见性，且具备高等幂等性，Pipeline 在遍历完所有的 Node 的
     *          SignalStop 后，再统一执行 Link Abort 和 CV 唤醒，此设计从拓扑层面消除了逐节点停止可能引发
     *          的死锁时序问题
     */
    void SignalStop() noexcept {
        // 1. 设置跨线程的布尔标志，供工作线程在死循环中作为原子谓词读取
        stop_requested_.store(true);

        // 2. 状态机跃迁：使用 CAS 循环将 NodeState 安全地推入 kStopRequested
        NodeState current = state_.load();

        // 幂等性拦截：如果已经是停止中或已停止的状态，直接跳出循环，放弃修改
        while (current != NodeState::kStopRequested && current != NodeState::kFinished &&
               current != NodeState::kJoined &&
               // 状态比较与交换：若当前状态仍为 current，则更新为 kStopRequested
               !state_.compare_exchange_strong(current, NodeState::kStopRequested)) {
        }
    }

    /**
     * @brief 触发派生类专属的阻塞 I/O 中断回调
     * @details 此钩子函数可能与后台 Tick() 发生并发争用，实现方仅限于设置原子标志、调用 shutdown() 关闭
     *          Socket 等操作；严禁在此释放 Tick 可能正在持有的媒体上下文（如 FFmpeg/ALSA 句柄）
     */
    void InterruptBlockingIo() noexcept { OnInterruptBlockingIo(); }

    /**
     * @brief 阻塞等待底层工作线程退出并回收内核资源
     * @return 资源回收状态码：若检测到尝试从当前工作线程内部发起 Join 操作，将返回
     * kCalledFromWorkerThread 以避免自身死锁
     */
    NodeJoinResult Join() noexcept {
        if (state_.load() == NodeState::kJoined) {
            return NodeJoinResult::kAlreadyJoined;
        }

        if (!thread_.joinable()) {
            state_.store(NodeState::kJoined);
            return NodeJoinResult::kNoThread;
        }

        // 比较 Node 内部线程 ID 和 当前正在执行的线程 ID
        if (thread_.get_id() == std::this_thread::get_id()) {
            return NodeJoinResult::kCalledFromWorkerThread;
        }

        try {
            thread_.join();
        } catch (const std::system_error&) {
            return NodeJoinResult::kJoinFailed;
        }

        state_.store(NodeState::kJoined);
        return NodeJoinResult::kJoined;
    }

public:
    /**
     * @brief 获取异步停止信令的状态
     * @return true 标识 Pipeline 已经启动回收流程，工作线程应尽快结束当前 Tick 周期
     */
    bool IsStopedRequested() const noexcept { return stop_requested_.load(); }

    /**
     * @brief 获取内部状态机的当前快照
     */
    NodeState GetState() noexcept { return state_.load(); }

    /**
     * @brief 获取节点退出的语义原因，未退出时返回 kNotFinished
     */
    NodeFinishReason GetFinishedReason() noexcept { return finished_reason_.load(); }

    /**
     * @brief 获取节点的名称
     */
    const std::string& GetName() { return name_; }

    /**
     * @brief 提取在工作线程中捕获的未处理 C++ 异常对象
     * @return 若无异常抛出， 返回 nullptr， 提取钱确保 Node 处于 kJoined 状态
     */
    std::exception_ptr GetUnhandledException() const noexcept {
        // 必须在 Join() 返回后，才能读取此变量
        assert(state_.load() == NodeState::kJoined && "Read worker exception only after Pipeline::Stop/Join");

        return unhandled_exception_;
    }

protected:
    /**
     * @brief 数据平面核心多态接口：执行单次数据处理负载
     * @return 指示线程循环应当继续或是以何种原因终止
     */
    virtual TickResult Tick() = 0;

    /**
     * @brief 相应外部中断的钩子
     * @details 默认缺省实现为空，派生类覆盖此方法时必须遵循无锁化，幂等且无内存释放操作的并发约束
     */
    virtual void OnInterruptBlockingIo() noexcept {}

    /**
     * @brief 判断当前生命周期阶段是否允许拓扑变更，所有的“连线”操作，必须在节点处于 kCreated
     * @return 仅在 kCreated 状态（启动前）返回 true
     */
    bool CanConfigure() const noexcept { return state_.load() == NodeState::kCreated; }

    /**
     * @brief 验证数据帧携带的 epoch 与全局控制总线是否一致
     * @param itme_epoch 数据流经的 DTO 所记录的世代戳
     * @return true 标识该数据属于已经废弃的 Seek 世代戳，应当执行无延迟的丢弃处理
     */
    bool ShouldDropStaleItem(std::uint64_t item_epoch) const noexcept { return item_epoch < sync_ctx_->GetEpoch(); }

    /**
     * @brief 获取控制总线 sync_ctx_ 的当前 Seek 世代戳，用于为新产出的数据项打戳
     */
    std::uint64_t GetCurrentEpoch() const noexcept { return sync_ctx_->GetEpoch(); }

    /// 由 Pipeline 统一分配并注入的控制总线，用于全局状态同步
    std::shared_ptr<SyncContext> sync_ctx_;

private:
    /**
     * @brief 工作线程顶层控制流
     * @details 在死循环内部实现了基于快速路径判决的协作式休眠，并包含全局 try-catch 块以捕获 C++ 异常
     */
    void ThreadMain() noexcept {
        NodeFinishReason reason = NodeFinishReason::kNotFinished;

        try {
            while (true) {
                // 步骤 1：处理挂起请求，并将当前节点的 Stop 标志作为唤醒条件注入
                sync_ctx_->WaitWhilePaused([this]() { return IsStopedRequested(); });

                // 步骤 2：醒来后，立即检查是否是因为收到 Stop 标志而被迫醒来的
                if (IsStopedRequested()) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                }

                // 3. 开始执行 Tick
                const TickResult kResult = Tick();
                if (kResult == TickResult::kContinue) {
                    continue;
                }

                if (kResult == TickResult::kEndOfStream) {
                    reason = NodeFinishReason::kEndOfStream;
                    break;
                } else if (kResult == TickResult::kStopped) {
                    reason = NodeFinishReason::kStopRequested;
                    break;
                } else {
                    reason = NodeFinishReason::kError;
                    break;
                }
            }
        } catch (...) {
            // 异常被顶层的 catch(...) 拦截然后使用 std::current_exception() 获取这个异常的指针
            // 并存入到 unhandled_exception_ 变量中
            unhandled_exception_ = std::current_exception();
            reason = NodeFinishReason::kUnhandledException;
        }

        finished_reason_.store(reason);
        state_.store(NodeState::kFinished);
    }

    /// 节点固定标识符。
    std::string name_;

    /// OS 级线程句柄。
    std::thread thread_;

    /// 原子保护的生命周期状态标识
    std::atomic<NodeState> state_{NodeState::kCreated};

    /// 跨线程内存可见的停止请求信令
    std::atomic<bool> stop_requested_{false};

    /// 原子保护的退出原因状态码
    std::atomic<NodeFinishReason> finished_reason_{NodeFinishReason::kNotFinished};

    /// 存放跨线程捕获异常的智能指针包装器
    std::exception_ptr unhandled_exception_;
};

/**
 * @struct NoPort
 * @brief 模板元编程占位符：明确标识缺失的数据端口以触发模板偏特化
 */
struct NoPort final {};

/**
 * @class INode
 * @brief 强类型数据契约节点， 输入 InT，输出 OutT，持有类型化的输入/输出 Link
 * @tparam InT  输入数据项类型（如 std::shared_ptr<Packet>）
 * @tparam OutT 输出数据项类型（如 std::shared_ptr<VideoFrame>）
 * @details 基类 INodeBase 管线程生命周期，本模板叠加 "数据怎么进、怎么出"：
 *          - 持有强类型 Link（input_ / output_）
 *          - 提供连接接口（ConnectInput / ConnectOutput），供 Pipeline 组图时调用
 *          - 提供一进一出拓扑的默认 AbortLinks（基于 input_ / output_）
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
 *   这些节点自行持有所需 Link，覆写 AbortLinks 处理自持管道。
 */
template <typename InT, typename OutT>
class INode : public INodeBase {
public:
    using InputLink = Link<InT>; ///< 输入管道类型别名
    using OutputLink = Link<OutT>; ///< 输出管道类型别名

    /**
     * @brief 构造函数
     * @param name 节点名
     * @param sync 全局控制块
     */
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

    /**
     * @brief 接入上游：把上游的输出 Link 作为本节点的输入
     */
    bool ConnectInput(std::shared_ptr<InputLink> input) {
        if (!CanConfigure() || !input) {
            return false;
        }

        input_ = std::move(input);
        return true;
    }

    /**
     * @brief 接入下游：把下游的输出 Link 作为本节点的输出
     */
    bool ConnectOutput(std::shared_ptr<OutputLink> output) {
        if (!CanConfigure() || !output) {
            return false;
        }

        output_ = std::move(output);
        return true;
    }

protected:
    /*
     * @brief 封装底层的阻塞式接收调用
     */
    std::optional<InT> ReceiveInput() { return input_ ? input_->Receive() : std::nullopt; }

    /**
     * @brief 封装底层的阻塞式发送调用
     */
    bool SendOutput(OutT item) { return output_ && output_->Send(std::move(item)); }

    /**
     * @brief 触发输出管道的温和关闭语义（EOF 排空机制）
     */
    void CloseOutput() {
        if (output_) {
            output_->Close();
        }
    }

private:
    /// 主输入管道
    std::shared_ptr<InputLink> input_;

    /// 主输出管道
    std::shared_ptr<OutputLink> output_;
};

/**
 * @class INode<NoPort, OutT>
 * @brief 模板偏特化：零进一出形态（Source Node/Producer），剥离 Input 逻辑避免实例化非法类型
 */
template <typename OutT>
class INode<NoPort, OutT> : public INodeBase {
public:
    using OutputLink = Link<OutT>;

    /**
     * @brief 构造函数
     * @param name 节点名
     * @param sync 全局控制块
     */
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

    /**
     * @brief 把下游的输出 Link 作为本节点的输出
     */
    bool ConnectOutput(std::shared_ptr<OutputLink> output) {
        if (!CanConfigure() || !output) {
            return false;
        }

        output_ = std::move(output);
        return true;
    }

protected:
    bool SendOutput(OutT item) { return output_ && output_->Send(std::move(item)); }

    /**
     * @brief 关闭输出
     */
    void CloseOutput() {
        if (output_) {
            output_->Close();
        }
    }

private:
    // 输出管道
    std::shared_ptr<OutputLink> output_;
};

/**
 * @class INode<InT, NoPort>
 * @brief 模板偏特化：一进零出形态（Sink Node/Consumer），剥离 Output 逻辑避免实例化非法类型
 */
template <typename InT>
class INode<InT, NoPort> : public INodeBase {
public:
    using InputLink = Link<InT>;

    /**
     * @brief 构造函数，构造一个一进零出形态的 Node
     * @param name 节点名称
     * @param sync 全局同步标志
     */
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

    /**
     * @brief 绑定输入节点
     */
    bool ConnectInput(std::shared_ptr<InputLink> input) {
        if (!CanConfigure() || !input) {
            return false;
        }

        input_ = std::move(input);
        return true;
    }

protected:
    std::optional<InT> ReceiveInput() { return input_ ? input_->Receive() : std::nullopt; }

private:
    std::shared_ptr<InputLink> input_;
};

/**
 * @class INode<NoPort, NoPort>
 * @brief 模板偏特化：独立后台控制节点（无任何数据端口依赖）。
 */
template <>
class INode<NoPort, NoPort> : public INodeBase {
public:
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}
};

} // namespace mediacore

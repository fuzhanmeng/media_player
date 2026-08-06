/**
 * @file pipeline.h
 *
 * @brief
 * DAG 媒体处理图的生命周期管理器
 *
 * Pipeline 负责：
 *
 * 1. 在 Building 阶段创建并登记全部 Node 和 Link
 * 2. 按 Node 的创建顺序保存 Node；调用者必须按 Source -> Sink 的拓扑顺序创建
 * 3. 按创建顺序的反方向启动 Node，保证消费者先于生产者启动
 * 4. Stop 时先让全图都看见停止请求，再解除所有阻塞，最后 Join 全部线程
 * 5. 只有全部线程 Join 完成后，才允许析构 Node、Link 和 SyncContext
 *
 * Pipeline 不负责具体的数据处理
 *
 * 例如：
 *
 * Demuxer 读取文件
 * Decoder 解码
 * Renderer 显示
 *
 * 这些属于 Node 自己的职责
 *
 * Pipeline 只负责让这些 Node 正确连接并运行
 *
 * CreateLink、EmplaceNode、Start、Pause、Resume、Stop 和析构必须由同一个控制线程串行调用
 * PipelineState 使用普通枚举是有意的：状态会和 nodes_、links_ 及停止过程一起变化，
 * 仅把枚举改成 atomic 不能让这些复合操作支持并发调用
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/inode.h"
#include "core/link.h"
#include "core/sync_context.h"

namespace mediacore {

/** @brief Pipeline 的一次性生命周期状态 */
enum class PipelineState : std::uint8_t {
    kBuilding, ///< 可以创建、连接 Node 和 Link
    kStarting, ///< 正在按反向登记顺序启动 Node
    kRunning, ///< 全部 Node 已启动，拓扑不可再修改
    kStopping, ///< 正在停止，或仍有线程尚未成功 Join
    kStopped, ///< 全部 Node 已 Join，Pipeline 不可重新启动
};

/** @brief Pipeline 启动结果 */
enum class PipelineStartResult : std::uint8_t {
    kStarted, ///< 全部 Node 启动成功
    kInvalidState, ///< Pipeline 不处于 Building 状态
    kNoNodes, ///< 尚未创建任何 Node，Pipeline 保持 Building
    kNodeStartFailed, ///< 一个 Node 启动失败，Stop 回滚成功
    kRollbackFailed, ///< 一个 Node 启动失败，并且至少一个回滚步骤失败
};

/**
 * @class Pipeline
 * @brief 拥有 DAG 对象并统一管理全部 Node 工作线程
 * @details
 * Node 使用 unique_ptr 保存，生命周期由 Pipeline 独占；Link 使用 shared_ptr，由 Pipeline 和相邻 Node
 * 共同持有不同类型的 Link<T> 通过 ILinkControl 保存在同一个 vector 中
 * 所有控制接口只能由一个控制线程按顺序调用，工作线程不直接修改 Pipeline 的容器或生命周期状态
 */
class Pipeline final {
public:
    /**
     * @brief 创建使用标准 std::thread 的空 Pipeline
     * @details 初始状态为 kBuilding，并创建本图全部 Node 共用的 SyncContext
     */
    Pipeline() : Pipeline(CreateDefaultThreadFactory()) {}

    /**
     * @brief 创建使用指定线程工厂的空 Pipeline
     * @param thread_factory 为每个 Node 创建工作线程的函数
     * @throws std::invalid_argument thread_factory 为空时抛出
     * @details 自定义工厂可用于统一设置线程名称、优先级或调度参数，也可确定性验证启动失败回滚
     */
    explicit Pipeline(NodeThreadFactory thread_factory)
        : sync_ctx_(std::make_shared<SyncContext>()), thread_factory_(std::move(thread_factory)) {
        if (!thread_factory_) {
            throw std::invalid_argument("Pipeline requires a non-empty NodeThreadFactory");
        }
    }

    /**
     * @brief 停止并 Join 全部 Node 后销毁 Pipeline
     */
    ~Pipeline() {
        const bool stop_succeeded = Stop();
        (void)stop_succeeded;

        // Stop 返回 false 也可能只是 Abort 清缓存失败；只有状态不是 kStopped 才表示仍有线程未 Join
        if (state_ != PipelineState::kStopped) {
            std::terminate();
        }
    }

    // 禁止拷贝构造/赋值 移动构造/赋值
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    /**
     * @brief 创建一个容量固定的 Link<T>，并保存到 links_
     * @tparam T Link 中传输的数据类型，例如 Packet / VideoFrame / AudioFrame
     * @param capacity Link 内部 ThreadSafeQueue 的最大元素数量，必须大于 0
     * @return Pipeline 与相邻 Node 共同持有的 shared_ptr<Link<T>>
     * @throws std::logic_error 当前不处于 Building 状态时抛出，防止运行中修改拓扑
     * @throws std::invalid_argument capacity 为 0 时由 ThreadSafeQueue 构造函数抛出
     *
     * @par 模板参数和函数参数
     * 调用 `pipeline.CreateLink<Packet>(24)` 时，T 是编译期类型参数 Packet，capacity 是运行时参数 24，
     * 返回类型是 `std::shared_ptr<Link<Packet>>`
     *
     * @par LinkCreationToken
     * `LinkCreationToken{}` 构造一个无名临时令牌对象，不是函数，它不保存数据，只用于通过 Link 的构造权限检查
     */
    template <typename T>
    std::shared_ptr<Link<T>> CreateLink(std::size_t capacity) {
        // Link 的元素必须是普通对象类型，不能是引用、数组或 cv 限定类型
        static_assert(std::is_object<T>::value, "Link<T> requires a non-reference object type");

        static_assert(!std::is_array<T>::value, "Link<T> does not support array element types");

        static_assert(!std::is_const<T>::value && !std::is_volatile<T>::value,
                      "Link<T> requires an unqualified mutable value type");

        static_assert(!std::is_same<T, NoPort>::value,
                      "NoPort marks a missing Node port and cannot be transported by a Link");

        // 图启动后不能再增加 Link，否则 Pipeline 的停止登记和启动顺序会失效
        RequireBuilding("Link can only be created while Pipeline is building");

        // make_shared 创建 Link 对象和 shared_ptr 控制块；link 是管理该对象的智能指针
        auto link = std::make_shared<Link<T>>(capacity, LinkCreationToken{});

        links_.emplace_back(link);
        return link;
    }

    /**
     * @brief 创建一个具体 Node，并追加到 nodes_
     * @tparam NodeT 要创建的具体 Node 类型，必须公开继承 INodeBase
     * @tparam Args NodeT 构造函数参数的类型列表，不包含 SyncContext
     * @param args 传给 NodeT 构造函数的参数，不包含 SyncContext
     * @return 新 Node 的引用；Node 所有权由 Pipeline 的 nodes_ 独占
     * @throws std::logic_error 当前不处于 Building 状态时抛出
     * @note 调用者必须按 Source -> Sink 的拓扑顺序调用本函数，Pipeline 只保存调用顺序，不分析 Link
     *       连接关系，也不计算拓扑排序；Start() 只会把登记顺序反过来启动
     *
     * @par 可变参数模板展开示例
     * 调用 `pipeline.EmplaceNode<FiniteSourceNode>(200)` 时：
     * - `NodeT` 是编译期类型参数，值为 FiniteSourceNode；
     * - `Args...` 是编译期类型包，此处大致为 `[int]`；
     * - `args...` 是运行时实参包，此处为 `[200]`；
     * - make_unique 最终调用 FiniteSourceNode(sync_context_, 200)
     *
     * 调用 `pipeline.EmplaceNode<CountingSinkNode>()` 时 Args/args 都是空包，最终调用
     * CountingSinkNode(sync_context_)，std::forward<Args>(args)... 逐项展开，并保留每个实参
     * 原来的左值、右值和移动语义
     *
     * @par 返回引用为什么有效
     * Node 对象本身由 make_unique 分配在稳定的动态存储中，nodes_ 扩容时移动的是 unique_ptr，
     * 不会移动它指向的 Node，所以返回引用在 Pipeline 析构前保持有效
     * 调用方只借用该引用进行 Connect 或查询，不拥有 Node，也不得让引用越过图的生命周期
     */
    template <typename NodeT, typename... Args>
    NodeT& EmplaceNode(Args&&... args) {
        // 编译期检查：NodeT 必须继承 INodeBase，且必须是可实例化的具体类型
        static_assert(std::is_base_of<INodeBase, NodeT>::value, "NodeT must derive from INodeBase");

        static_assert(std::is_convertible<NodeT*, INodeBase*>::value,
                      "NodeT must publicly and unambiguously derive from INodeBase");

        static_assert(!std::is_abstract<NodeT>::value, "NodeT must be a concrete, non-abstract Node type");

        // 构造函数的第一个参数由 Pipeline 注入，后面的参数来自调用者
        static_assert(std::is_constructible<NodeT, std::shared_ptr<SyncContext>, Args...>::value,
                      "NodeT must be constructible from shared_ptr<SyncContext> followed by Args...");

        RequireBuilding("Node can only be created while Pipeline is building");

        // Pipeline 自动注入共享 SyncContext；调用者只传具体 Node 自己需要的参数
        auto node = std::make_unique<NodeT>(sync_ctx_, std::forward<Args>(args)...);

        // nodes_ 接管 unique_ptr；返回值只是对同一个 Node 对象的借用引用
        NodeT& ref = *node;
        nodes_.emplace_back(std::move(node));
        return ref;
    }

    /**
     * @brief 按 nodes_ 的反向登记顺序启动全部 Node
     * @return 明确的启动结果；任一 Node 失败时立即执行完整 Stop 回滚
     * @details
     * 调用者按 EmplaceNode() 的约定以 Source 到 Sink 的顺序登记 Node 后，反向遍历会先启动消费者
     * 任一 Node 启动失败时，Stop() 会停止并 Join 已创建的线程，Pipeline 启动后不能再次调用 Start()
     */
    PipelineStartResult Start() {
        // 只有 Building 状态可以启动
        if (state_ != PipelineState::kBuilding) {
            return PipelineStartResult::kInvalidState;
        }

        // 空图不启动，也不改变状态，调用方仍可继续组图
        if (nodes_.empty()) {
            return PipelineStartResult::kNoNodes;
        }

        state_ = PipelineState::kStarting;

        // 反向启动：登记顺序是 Source -> Sink，因此这里先启动消费者
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
            if ((*it)->Start(thread_factory_) != NodeStartResult::kStarted) {
                // 任何一个 Node 创建线程失败，都回滚已经启动的 Node
                return Stop() ? PipelineStartResult::kNodeStartFailed : PipelineStartResult::kRollbackFailed;
            }
        }
        state_ = PipelineState::kRunning;
        return PipelineStartResult::kStarted;
    }

    /**
     * @brief 请求全图停止，解除所有已知阻塞并 Join 全部 Node
     * @return 所有停止步骤都成功时返回 true，否则返回 false
     * @details
     * Stop() 按以下顺序执行：
     *
     * 1. 设置每个 Node 的停止标志；
     * 2. Abort 所有 Link，唤醒阻塞的 Send() 和 Receive()；
     * 3. 请求每个 Node 中断 Link 之外的阻塞 I/O；
     * 4. 唤醒等待暂停条件变量的 Node；
     * 5. Join 所有 Node 工作线程
     * @note 析构函数会调用本函数，因此本函数不抛出异常，可处理的异常会转换为 false，停止流程仍会继续
     */
    bool Stop() noexcept {
        // 已经完成停止时，重复调用返回第一次停止结果
        if (state_ == PipelineState::kStopped) {
            return stop_succeeded_;
        }

        // 只允许一次停止流程；控制接口本身不支持并发调用
        if (stop_in_progress_) {
            return false;
        }

        stop_in_progress_ = true;
        state_ = PipelineState::kStopping;

        // 1. 先给所有 Node 设置停止标志，包括尚未创建线程的 Node
        for (const auto& node : nodes_) {
            node->SignalStop();
        }

        // 2. 解除 Link 阻塞；Abort 让 Send() 和 Receive() 立即返回失败
        bool success = true;
        for (const auto& link : links_) {
            try {
                link->Abort();
            } catch (...) {
                success = false;
            }
        }

        // 3. 中断 Link 之外的阻塞 I/O，例如 av_read_frame() 或 socket recv()
        for (auto& node : nodes_) {
            node->InterruptBlockingIo();
        }

        // 4. 唤醒暂停条件变量上的 Node；它们随后读取自己的停止标志并退出 ThreadMain()
        try {
            sync_ctx_->WakeAllPausedThreads();
        } catch (...) {
            success = false;
        }

        // 5. 等待并回收全部工作线程
        bool all_nodes_joined = true;
        for (const auto& node : nodes_) {
            const NodeJoinResult result = node->Join();

            if (result == NodeJoinResult::kCannotJoinSelf || result == NodeJoinResult::kJoinFailed) {
                success = false;
                all_nodes_joined = false;
            }
        }

        stop_in_progress_ = false;

        // 只要有一个线程没有 Join，就不能进入 kStopped，也不能析构 Pipeline
        if (!all_nodes_joined) {
            return false;
        }

        // 到这里时全部线程都已退出并回收，Node 和 Link 可以安全析构
        state_ = PipelineState::kStopped;
        stop_succeeded_ = success;
        return stop_succeeded_;
    }

    /**
     * @brief 请求所有 Node 在后续工作循环中暂停
     * @return Pipeline 正在运行时返回 true，否则返回 false
     * @details
     * 本函数只提交 Paused 状态，不等待所有 Node 回报 已经暂停，正在执行或阻塞在 Tick() 中的
     * Node 不会被强行中断；它会在 Tick() 返回并进入下一轮 WaitWhilePaused() 时暂停
     */
    bool Pause() {
        if (state_ != PipelineState::kRunning) {
            return false;
        }

        sync_ctx_->SetPaused();
        return true;
    }

    /**
     * @brief 恢复运行并唤醒所有暂停 Node
     * @return Pipeline 状态为 kRunning 时返回 true，否则返回 false
     */
    bool Resume() {
        if (state_ != PipelineState::kRunning) {
            return false;
        }

        sync_ctx_->SetRunning();
        return true;
    }

    /**
     * @brief 返回所有 Node 共用的 SyncContext 的只读视图
     * @return 指向 const SyncContext 的 std::shared_ptr，外部不能调用 Pipeline 专属的写接口
     */
    std::shared_ptr<const SyncContext> GetSyncContext() const { return sync_ctx_; }

    /**
     * @brief 返回 Pipeline 当前状态
     * @return 由控制线程维护的 PipelineState
     * @note 只能从约定的 Pipeline 控制线程调用；本函数不是跨线程状态观察接口
     */
    PipelineState GetState() const { return state_; }

private:
    /**
     * @brief 创建默认 Node 线程工厂
     * @return 忽略 Node 名称并直接使用 std::thread 执行入口函数的工厂
     * @details 工厂收到的 entry 就是 Node 的 ThreadMain() 回调；std::thread(entry) 创建并启动一个
     *          新线程，Pipeline 随后保存返回的线程对象并在 Stop() 中 Join
     */
    static NodeThreadFactory CreateDefaultThreadFactory() {
        return [](const std::string&, NodeThreadEntry entry) { return std::thread(std::move(entry)); };
    }

    /**
     * @brief 检查当前是否允许添加 Node 或 Link
     * @param message 状态不正确时使用的异常文本
     * @throws std::logic_error Pipeline 已离开 Building 状态时抛出
     */
    void RequireBuilding(const char* message) const {
        if (state_ != PipelineState::kBuilding) {
            throw std::logic_error(message);
        }
    }

    /// 本 Pipeline 的共享运行状态；Pipeline 写入，Node 持有 shared_ptr 并读取
    std::shared_ptr<SyncContext> sync_ctx_;

    /// 按 Node 启动顺序同步调用的线程创建函数
    NodeThreadFactory thread_factory_;

    /// 按 Source 到 Sink 的登记顺序独占全部 Node
    std::vector<std::unique_ptr<INodeBase>> nodes_;

    /// 保存全部 Link 的控制接口，Stop 时不需要知道数据类型
    std::vector<std::shared_ptr<ILinkControl>> links_;

    /// 只由单一 Pipeline 控制线程访问的生命周期状态
    PipelineState state_{PipelineState::kBuilding};

    /// 防止停止钩子意外同步重入 Stop()
    bool stop_in_progress_{false};

    /// 保存首次完成停止时的结果，重复 Stop() 返回同一结果
    bool stop_succeeded_{false};
};

} // namespace mediacore

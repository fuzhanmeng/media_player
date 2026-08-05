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
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/inode.h"
#include "core/link.h"
#include "core/sync_context.h"

namespace mediacore {

/**
 * @enum PipelineState
 * @brief Pipeline 当前所处的执行阶段
 */
enum class PipelineState : std::uint8_t {
    kBuilding, ///< 可以创建 Node/Link 和连接端口；尚未启动任何工作线程
    kStarting, ///< 正在按 nodes_ 的反向顺序启动 Node
    kRunning, ///< Node 已启动；拓扑冻结，只允许 Pause/Resume/Stop
    kStopping, ///< 正在停止，或上一次 Join 失败而仍有线程需要再次回收
    kStopped, ///< 已完成回收；Pipeline 和内部对象均不可重新启动
};

/**
 * @enum PipelineStartResult
 * @brief Pipeline 启动结果
 */
enum class PipelineStartResult : std::uint8_t {
    kStarted, ///< 全部 Node 启动成功
    kInvalidState, ///< Pipeline 已经启动、正在停止或已经停止
    kNoNodes, ///< 图为空；保持 Building，调用者仍可继续组图
    kNodeStartFailed, ///< 某个 Node 启动失败，并且 Stop 回滚成功
    kRollbackFailed, ///< 某个 Node 启动失败，且至少一个已启动线程未能完成回收
};

/**
 * @class Pipeline
 * @brief 拥有 Node，并保存停止时需要访问的全部 Link
 * @details Node 由 unique_ptr 保存，只能由 Pipeline 销毁，Link 使用 shared_ptr，因为 Pipeline
 * 和连接该 Link 的 Node 都需要持有它
 * links_ 保存 shared_ptr<ILinkControl>，因此不同数据类型的 Link<T> 可以放在同一个
 * vector 中。Stop() 只调用 ILinkControl::Abort()，不需要知道 Link 中的数据类型
 * @note CreateLink、EmplaceNode、Start、Pause、Resume、Stop 和析构必须由同一个控制线程串行调用
 *       Pipeline 不支持这些操作彼此并发执行
 */
class Pipeline final {
public:
    /**
     * @brief 创建一个空 Pipeline
     * @details 初始状态为 kBuilding，并创建所有 Node 共用的 SyncContext
     */
    Pipeline() : sync_ctx_(std::make_shared<SyncContext>()) {}

    /**
     * @brief 停止并 Join 所有 Node 后再析构成员
     */
    ~Pipeline() {
        const bool kStopped = Stop();
        (void)kStopped;
        assert(state_ == PipelineState::kStopped &&
               "Pipeline cannot be destroyed while a Node thread remains unjoined");
    }

    // 禁止拷贝构造/赋值 移动构造/赋值
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    /**
     * @brief 创建一个容量固定的 Link<T>，并保存到 links_
     * @tparam T Link 中传输的数据类型，例如 Packet、VideoFrame 或 AudioFrame
     * @param capacity Link 内部 ThreadSafeQueue 的最大元素数量，必须大于 0
     * @return 新 Link 的 std::shared_ptr， Pipeline 和 Node 共同持有
     * @throws std::logic_error 当前不处于 Building 状态时抛出，防止运行中修改拓扑
     * @throws std::invalid_argument capacity 为 0 时由 ThreadSafeQueue 构造函数抛出
     *
     * @par 模板参数和函数参数
     * 调用 `pipeline.CreateLink<Packet>(24)` 时，T 是编译期类型参数 Packet，capacity 是运行时参数 24，
     * 返回类型是 `std::shared_ptr<Link<Packet>>`
     *
     * @par LinkCreationToken
     * LinkCreationToken{} 构造的一个无名临时通行证对象，不是函数
     */
    template <typename T>
    std::shared_ptr<Link<T>> CreateLink(std::size_t capacity) {
        // T 必须是一个“对象类型
        static_assert(std::is_object<T>::value, "Link<T> requires a non-reference object type");

        // T 不能是原生的数组类型
        static_assert(!std::is_array<T>::value, "Link<T> does not support array element types");

        // T 既不能带有 const，也不能带有 volatile
        static_assert(!std::is_const<T>::value && !std::is_volatile<T>::value,
                      "Link<T> requires an unqualified mutable value type");

        // T 不能和 NoPort 是同一个类型
        static_assert(!std::is_same<T, NoPort>::value,
                      "NoPort marks a missing Node port and cannot be transported by a Link");

        RequireBuilding("Link can only be created while Pipeline is building");

        // 构造 Link<T> 对象，并返回管理该对象的 shared_ptr
        auto link = std::make_shared<Link<T>>(capacity, LinkCreationToken{});

        links_.emplace_back(link);
        return link;
    }

    /**
     * @brief 创建一个 Node，并追加到 nodes_
     * @tparam NodeT 要创建的具体 Node 类型，必须公开继承 INodeBase
     * @tparam Args NodeT 构造函数参数的类型列表，不包含 SyncContext
     * @param args 传给 NodeT 构造函数的参数，不包含 SyncContext
     * @return 新 Node 的引用；Node 所有权由 Pipeline 的 nodes_ 独占
     * @throws std::logic_error 当前不处于 Building 状态时抛出
     * @note 调用者必须按 Source -> Sink 的拓扑顺序调用本函数。Pipeline 只保存调用顺序，
     *       不分析 Link 连接关系，也不会自动计算拓扑顺序
     *
     * @par 可变参数模板展开示例
     * 调用 `pipeline.EmplaceNode<FiniteSourceNode>(200)` 时：
     * - `NodeT` 是编译期类型参数，值为 FiniteSourceNode；
     * - `Args...` 是编译期类型包，此处大致为 `[int]`；
     * - `args...` 是运行时实参包，此处为 `[200]`；
     * - make_unique 最终调用 FiniteSourceNode(sync_context_, 200)
     *
     * 调用 `pipeline.EmplaceNode<CountingSinkNode>()` 时 Args/args 都是空包，最终调用
     * `CountingSinkNode(sync_context_)`。`std::forward<Args>(args)...` 逐项展开并保留
     * 每个实参原来的左值、右值和移动语义
     *
     * @par 返回引用为什么有效
     * Node 对象本身由 make_unique 分配在稳定的动态存储中nodes_ 扩容时移动的是
     * unique_ptr，不会移动它指向的 Node，所以返回引用在 Pipeline 析构前保持有效
     * 调用方只借用该引用进行 Connect 或查询，不拥有 Node，也不得让引用越过图的生命期

     */
    template <typename NodeT, typename... Args>
    NodeT& EmplaceNode(Args&&... args) {
        // 编译期断言，检查 NodeT 是否继承自 INodeBase
        static_assert(std::is_base_of<INodeBase, NodeT>::value, "NodeT must derive from INodeBase");

        // 编译期断言，检查能不能把一个指向 NodeT 的指针，隐式转换成指向 INodeBase 的指针
        static_assert(std::is_convertible<NodeT*, INodeBase*>::value,
                      "NodeT must publicly and unambiguously derive from INodeBase");

        // 编译期断言，检查 NodeT 是不是一个抽象类
        static_assert(!std::is_abstract<NodeT>::value, "NodeT must be a concrete, non-abstract Node type");

        // 编译期断言，检查 NodeT 这个类，能不能用 <std::shared_ptr<SyncContext>,  Args...> 作为参数来调用构造函数
        static_assert(std::is_constructible<NodeT, std::shared_ptr<SyncContext>, Args...>::value,
                      "NodeT must be constructible from shared_ptr<SyncContext> followed by Args...");

        RequireBuilding("Node can only be created while Pipeline is building");

        // Pipeline 自动注入共享 SyncContext；调用者只传具体 Node 自己需要的参数
        auto node = std::make_unique<NodeT>(sync_ctx_, std::forward<Args>(args)...);

        // nodes_ 接管 unique_ptr，返回值只是对同一个 Node 对象的引用
        NodeT& ref = *node;
        nodes_.emplace_back(std::move(node));
        return ref;
    }

    /**
     * @brief 按 nodes_ 的反向登记顺序启动全部 Node
     * @return 明确的启动结果；任一 Node 失败时立即执行完整 Stop 回滚
     * @details
     * 调用者按 EmplaceNode() 的约定以 Source 到 Sink 的顺序登记 Node 后，反向遍历会先启动消费者。
     * 任一 Node 启动失败时，Stop() 会停止并 Join 已启动的 Node。Pipeline 启动后不能再次调用 Start()
     */
    PipelineStartResult Start() {
        if (state_ != PipelineState::kBuilding) {
            return PipelineStartResult::kInvalidState;
        }

        if (nodes_.empty()) {
            return PipelineStartResult::kNoNodes;
        }

        state_ = PipelineState::kStarting;

        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
            if ((*it)->Start() != NodeStartResult::kStarted) {
                return Stop() ? PipelineStartResult::kNodeStartFailed : PipelineStartResult::kRollbackFailed;
            }
        }
        state_ = PipelineState::kRunning;
        return PipelineStartResult::kStarted;
    }

    /**
     * @brief 停止全部 Node，并等待它们的工作线程退出
     * @return 所有停止步骤都成功时返回 true，否则返回 false
     * @details
     * Stop() 按以下顺序执行：
     *
     * 1. 设置每个 Node 的停止标志；
     * 2. Abort 所有 Link，唤醒阻塞的 Send() 和 Receive()；
     * 3. 请求每个 Node 中断 Link 之外的阻塞 I/O；
     * 4. 唤醒等待暂停条件变量的 Node；
     * 5. Join 所有 Node 工作线程
     * @note 析构函数会调用本函数，因此本函数不抛出异常。可处理的异常会转换为false 返回值，同时继续执行
     * 剩余停止步骤
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

        // 1. 通知所有 Node 停止
        for (const auto& node : nodes_) {
            node->SignalStop();
        }

        // 2. 解除 Link 阻塞，Abort 会让阻塞在 Link::Send() 或 Link::Receive() 的线程返回
        bool success = true;
        for (const auto& link : links_) {
            try {
                link->Abort();
            } catch (...) {
                success = false;
            }
        }

        // 3. 打断外部阻塞：如av_read_frame() 或 socket recv()
        for (auto& node : nodes_) {
            node->InterruptBlockingIo();
        }

        // 4. 让暂停中的线程返回 ThreadMain() 并检查停止标志
        try {
            sync_ctx_->WakeAllPausedThreads();
        } catch (...) {
            success = false;
        }

        // 5. 等待线程退出
        bool all_nodes_joined = true;
        for (const auto& node : nodes_) {
            const NodeJoinResult kResult = node->Join();

            if (kResult == NodeJoinResult::kCalledFromWorkerThread || kResult == NodeJoinResult::kJoinFailed) {
                success = false;
                all_nodes_joined = false;
            }
        }

        stop_in_progress_ = false;

        // 6. 只要有一个线程还没有 Join，就不能把状态标记为 kStopped
        if (!all_nodes_joined) {
            return false;
        }

        // 7. 走到这里，全部线程都已安全退出，销毁 Pipeline 不会再触发 std::thread 的 terminate
        state_ = PipelineState::kStopped;
        stop_succeeded_ = success;
        return stop_succeeded_;
    }

    /**
     * @brief 请求所有 Node 在下一次 Tick() 前暂停
     * @return 当前状态为 kRunning 时返回 true，否则返回 false
     * @note 本函数只提交暂停状态，不等待所有 Node 都进入暂停等待
     */
    bool Pause() {
        if (state_ != PipelineState::kRunning) {
            return false;
        }

        sync_ctx_->SetPaused();
        return true;
    }

    /**
     * @brief 将 SyncContext 设置为 Running， 并唤醒暂停中的 Node
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
     * @brief 返回所有 Node 共用的 SyncContext，只允许调用只读接口
     * @return 指向 const SyncContext 的 std::shared_ptr
     */
    std::shared_ptr<const SyncContext> GetSyncContext() const { return sync_ctx_; }

    /**
     * @brief 返回 Pipeline 当前状态
     * @return PipelineState
     * @note 只能从 Pipeline 的控制线程调用
     */
    PipelineState GetState() const { return state_; }

private:
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

    std::shared_ptr<SyncContext> sync_ctx_; ///< 所有 Node 共享的控制对象

    std::vector<std::unique_ptr<INodeBase>> nodes_; ///< 保存所有的 Nodes

    std::vector<std::shared_ptr<ILinkControl>> links_; ///< 保存类型擦除后的全部 Link

    PipelineState state_{PipelineState::kBuilding}; ///< 仅由 Pipeline 控制线程访问

    /// 防止 Stop 在执行 OnInterruptBlockingIo 等用户钩子期间发生同步重入
    bool stop_in_progress_{false};

    /// 缓存已经完成的停止结果，使重复 Stop 不会把首次失败错误地改报为成功
    bool stop_succeeded_{false};
};

} // namespace mediacore

/**
 * @file inode.h
 * @brief DAG 两层抽象，非模板生命周期基类 INodeBase + 强类型数据契约模板 INode<InT,OutT>
 * @details
 * 一个节点实际上有两部分职责：
 *
 * ① 管理自己的工作线程（启动、停止、等待退出）——与数据类型无关
 * ② 处理数据（例如 Packet -> VideoFrame）——强类型
 *
 * 这两部分职责互不影响，因此拆成两个类：
 *
 * INodeBase
 *     ├── 管线程（Start / RequestStop / WaitForStop）
 *     ├── 持有共享的 SyncContext（读全局运行态：运行/暂停/停止）
 *     └── 与数据类型无关 —— 故 Pipeline 可用 vector<unique_ptr<INodeBase>> 异构统一保管
 *
 * INode<InT, OutT>
 *     ├── 管数据类型（输入 InT，输出 OutT，持有类型化 Link）
 *     └── 【D9 方案 A】提供标准三态循环 ThreadMain()，子类只需实现 Tick()
 *
 * 这样 Pipeline 就可以把所有节点都当作 INodeBase 保存，
 * 统一启动和停止；而每个节点仍然保留自己的数据类型，
 */
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility> // std::move

#include "mediacore/core/link.h"
#include "mediacore/core/sync_context.h"

namespace mediacore {
/**
 * @class INodeBase
 * @brief 与数据类型无关的生命周期基类：管线程（Start / RequestStop / WaitForStop）
 *  并持有共享 SyncContext 供派生类读取全局运行态
 */
class INodeBase {
public:
    /**
     * @brief 构造函数
     * @param name 节点名
     * @param sync 全局控制块， 由 Pipeline 创建，全局共享
     */
    INodeBase(std::string name, std::shared_ptr<SyncContext> sync)
        : name_(std::move(name)), sync_ctx_(std::move(sync)) {}

    /**
     * @brief 析构函数
     */
    virtual ~INodeBase() {
        RequestStop();
        WaitForStop();
    }

    // 节点持有线程和原子状态，不支持拷贝移动
    INodeBase(const INodeBase&) = delete;
    INodeBase& operator=(const INodeBase&) = delete;

    INodeBase(INodeBase&&) = delete;
    INodeBase& operator=(INodeBase&&) = delete;

    /**
     * @brief 启动工作线程
     * @details 先在 Start 线程同步执行 BeforeThreadStart 准备资源
     */
    void Start() {
        // 如果已经启动了，那么直接返回
        if (running_.exchange(true)) {
            return;
        }

        stopRequested_.store(false);

        // 1. 线程启动前准备，主线程执行：打开文件、分配缓冲等
        BeforeThreadStart();

        // 2. 拉起后台工作线程，执行标准循环
        thread_ = std::thread([this]() { ThreadMain(); });
    }

    /**
     * @brief 异步请求停止（不阻塞）：置停止标志并调 OnStopRequested 踹醒可能阻塞的线程
     */
    void RequestStop() {
        stopRequested_.store(true);
        OnStopRequested();
    }

    /**
     * @brief 同步等待线程退出 join
     */
    void WaitForStop() {
        if (thread_.joinable()) {
            thread_.join();
        }

        running_.store(false);
    }

    /**
     * @brief 是否已经收到停止请求，派生类主循环应该在每轮迭代检查它以便及时退出
     */
    bool IsStopRequested() const { return stopRequested_.load(); }

protected:
    /**
     * @brief 工作线程主体：节点的核心循环,由派生类实现
     */
    virtual void ThreadMain() = 0;

    /**
     * @brief 线程启动前由 Start 调用，做好启动准备，由派生类实现
     */
    virtual void BeforeThreadStart() {}

    /**
     * @brief 收到停止请求时执行，用于唤醒阻塞、通知退出等收尾工作
     */
    virtual void OnStopRequested() {}

    /// 全局状态标志
    std::shared_ptr<SyncContext> sync_ctx_;

private:
    /// 节点名
    std::string name_;

    /// 工作线程句柄：WaitForStop 负责join
    std::thread thread_;

    /// 是否已经启动
    std::atomic<bool> running_{false};

    /// 停止请求标志： RequestStop
    std::atomic<bool> stopRequested_{false};
};

/**
 * @class INode
 * @brief 强类型数据契约节点， 输入 InT，输出 OutT，持有类型化的输入/输出 Link
 * @tparam InT  输入数据项类型（如 std::shared_ptr<Packet>）
 * @tparam OutTT 输出数据项类型（如 std::shared_ptr<VideoFrame>）
 * @details 基类 INodeBase 管线程生命周期，本模板叠加 "数据怎么进、怎么出"：
 *          - ConnectInput / ConnectOutTput 在 Pipeline 组装期把上下游 Link 进行连接
 * @note 本模板固定为 "一条输入、一条输出"， 多路由节点 Demuxer（一进双出）、Tee 多播、Muxer 多进
         作为派生类 自行持有 所需 Link
 */
template <typename InT, typename OutT>
class INode : public INodeBase {
public:
    using InputLink = Link<InT>; ///< 输入管道类型别名
    using OutTputLink = Link<OutT>; ///< 输出管道类型别名

    /**
     * @brief 构造函数
     */
    explicit INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

    /**
     * @brief 接入上游：把上游的输入 Link 作为本节点的输入
     */
    void ConnectInput(std::shared_ptr<InputLink> input) { input_ = std::move(input); }

    /**
     * @brief 接入下游：把下游的输出 Link 作为本节点的输出
     */
    void ConnectOutTput(std::shared_ptr<OutTputLink> OutTput) { OutTput_ = std::move(OutTput); }

protected:
    /**
     * @details 每轮：① 先判停止（节点级标志，最高优先级）；② 若暂停则睡入 CV 省 CPU，
     *          注入停止判据让谓词能"看到"停止；醒来回顶复查；③ 运行态调 Tick() 干一次活，
     *          Tick 返回 false（上游 EOF/Abort）则收尾退出。
     */
    void ThreadMain() final {
        while (true) {
            // 1. 先判断本节点是否停止态
            if (IsStopRequested()) {
                break;
            }

            // 2. 暂停态：睡入 SyncContext 的 CV， 不空转 CPU
            if (sync_ctx_->GetState() == PipelineState::kPaused) {
                sync_ctx_->WaitWhilePaused([this]() { return IsStopRequested(); });
                continue;
            }

            // 3. 运行态： 执行 Tick 干活，if 返回 false 表示上游 EOF/Abort，退出循环
            if (!Tick()) {
                break;
            }
        }
    }

    /**
     * @brief 唤醒可能因 Receive() 阻塞在输入管道上的工作线程
     */
    void OnStopRequested() override {
        if (input_ != nullptr) {
            input_->Abort();
        }
    }

    /**
     * @brief 处理单个数据项：各派生类自己去实现
     * @return true 继续循环， false 表示上游结束
     * @note
     */
    virtual bool Tick() = 0;

    /// 主输入管道
    std::shared_ptr<InputLink> input_;

    /// 主输出管道
    std::shared_ptr<OutTputLink> OutTput_;
};
} // namespace mediacore
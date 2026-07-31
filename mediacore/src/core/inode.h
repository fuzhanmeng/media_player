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
 *     ├── 管线程（Start / RequestStop / WaitForStop / ThreadMain 三态循环）
 *     ├── 持有共享的 SyncContext（读全局运行态：运行/暂停/停止）
 *     └── 与数据类型无关 —— 故 Pipeline 可用 vector<unique_ptr<INodeBase>> 异构统一保管
 *
 * INode<InT, OutT>
 *     ├── 管数据类型（输入 InT，输出 OutT，持有类型化 Link）
 *     └── 提供一进一出拓扑的默认 AbortLinks（基于 input_ / output_）
 *
 * 这样 Pipeline 就可以把所有节点类型（一进一出、一进双出、双进一出、无输入源节点、无输出汇节点）
 * 统一放在同一个 vector<unique_ptr<INodeBase>> 里管理，异构而类型安全。
 *
 * @par 多路节点（一进双出 / 双进一出 / 多播）怎么办
 *   它们继承 INodeBase，自行持有所需的多条 Link，覆写 AbortLinks 处理自持管道。
 *   示例：
 *     class FfmpegDemuxerNode : public INodeBase {
 *         std::shared_ptr<Link<Packet>> videoOutput_;
 *         std::shared_ptr<Link<Packet>> audioOutput_;
 *         void AbortLinks() override {
 *             videoOutput_->Abort();
 *             audioOutput_->Abort();
 *         }
 *     };
 *
 * @par 为什么 ThreadMain 在 INodeBase 而非 INode
 *   三态循环（判停止 / 睡暂停 / 调 Tick）完全不依赖数据类型，只用到 sync_ctx_ / IsStopRequested() / Tick()，
 *   这三者都在 INodeBase 手里。若放在 INode，多路节点继承 INodeBase 时必须自己重写一遍循环 ——
 *   而这是方案甲的核心逻辑，抄四遍（Demuxer/Tee/Muxer/AVSync）等于把"正确性靠开发者记忆"的卡点 C
 *   问题在更高层级重犯一次。
 *
 * @par 派生类析构必须显式停线程
 *   C++ 析构顺序是派生→基类。若派生类持有 Link 等资源，而工作线程仍在跑（访问这些资源），
 *   派生类析构时资源先失效 → 线程悬垂访问 → 段错误。故**每个具体节点的析构函数首行必须**：
 *       RequestStop();
 *       WaitForStop();
 *   基类 INodeBase 析构仍保留兜底逻辑（防有人忘写），但它不能替代派生类的显式停止。
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef> // std::size_t
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility> // std::move

// 私有头：路径相对 mediacore/src（PRIVATE include 目录），不对外暴露
#include "core/link.h"
#include "core/sync_context.h"

namespace mediacore {
/**
 * @class INodeBase
 * @brief 所有 DAG 节点的基类，管线程生命周期与停止信号，与数据类型无关
 * @details 职责：
 *          - Start / RequestStop(两阶段停止) / WaitForStop
 *          - ThreadMain 标准三态循环（停止/暂停/运行，声明为 final）
 *          - 持有 SyncContext（读全局运行态 state / epoch）
 *          - 纯虚 Tick() 与 AbortLinks()，派生类各自实现
 */
class INodeBase {
public:
    /**
     * @brief 构造函数
     * @param name 节点名，调试日志用
     * @param sync 全局控制块， 由 Pipeline 创建，全局共享
     */
    INodeBase(std::string name, std::shared_ptr<SyncContext> sync)
        : sync_ctx_(std::move(sync)), name_(std::move(name)) {}

    /**
     * @brief 析构函数
     * @details 兜底：若派生类析构未显式停线程，基类再停一次（幂等，安全）。
     *          但这**不能替代**派生类显式停止 —— 派生类持有的资源会在基类析构前失效，
     *          若线程仍在跑则悬垂访问。正确做法见类注释顶部。
     */
    virtual ~INodeBase() {
        RequestStop();
        WaitForStop();
    }

    // 禁止拷贝和移动，节点持有线程与同步原语，语义上不可赋值
    INodeBase(const INodeBase&) = delete;
    INodeBase& operator=(const INodeBase&) = delete;
    INodeBase(INodeBase&&) = delete;
    INodeBase& operator=(INodeBase&&) = delete;

    /**
     * @brief 启动工作线程（非阻塞）
     * @details running_ 用 CAS 防重入。BeforeThreadStart 在持锁期执行，用于节点的同步准备
     *          （例如打开文件、探测流参数）。线程创建成功后立即返回，不等待线程启动完成。
     */
    void Start() {
        // CAS 防重入：只有首次调用能把 false 换成 true
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return; // 已启动，幂等返回
        }

        BeforeThreadStart(); // 派生类的同步准备（如 Demuxer 探测流参数）

        thread_ = std::thread(&INodeBase::ThreadMain, this);
    }

    /**
     * @brief 阶段一：异步请求停止（非阻塞）
     * @details 置停止标志并调 OnStopRequested 踹醒可能阻塞的线程，立即返回。
     *          幂等，可重复调用。
     */
    void RequestStop() {
        stopRequested_.store(true);
        OnStopRequested();
    }

    /**
     * @brief 阶段二：阻塞等待线程退出
     * @details join 工作线程。必须在 RequestStop 之后调用（否则线程永不退出）。
     *          幂等，可重复调用（join 已退出的线程是空操作）。
     */
    void WaitForStop() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /**
     * @brief 是否已经收到停止请求，派生类主循环应该在每轮迭代检查它以便及时退出
     */
    bool IsStopRequested() const { return stopRequested_.load(); }

protected:
    /**
     * @brief 标准三态循环（停止/暂停/运行），所有节点共享，声明为 final 禁止覆写
     * @details 职责分工：
     *          - ThreadMain（本函数）：调度骨架，判停止/睡暂停/调 Tick，与数据类型无关
     *          - Tick()（纯虚）：单次迭代逻辑，由派生类实现，与具体业务相关
     *          此循环是方案甲的核心，卡点 A（暂停 CV）的正确处理全在这三行里。
     *          若让每个多路节点自己重写，等于把"正确性靠开发者记忆"的卡点 C 问题在更高层级重犯。
     */
    void ThreadMain() {
        while (true) {
            // 停止：优先级最高，立即退出
            if (IsStopRequested()) {
                break;
            }

            // 暂停：睡在 pauseCv_ 上，注入停止判据让谓词能"看到"停止信号
            if (sync_ctx_->GetState() == PipelineState::kPaused) {
                sync_ctx_->WaitWhilePaused([this]() { return IsStopRequested(); });
                continue; // 被唤醒后回到循环顶重新判停止
            }

            // 运行：调派生类的 Tick 干活，返回 false 表示自然结束（EOF / Abort）
            if (!Tick()) {
                break;
            }
        }
    }

    /**
     * @brief 线程启动前由 Start 调用，做好启动准备，由派生类实现
     * @details 在持锁期执行，用于同步准备（如 Demuxer 的 Open(url) 探测流参数）。
     *          默认空实现，有需要的节点覆写。
     */
    virtual void BeforeThreadStart() {}

    /**
     * @brief 收到停止请求时执行，用于唤醒阻塞、通知退出等收尾工作
     * @details RequestStop 会调此钩子。基类已调 AbortLinks，派生类若有额外清理
     *          （如关闭文件句柄、释放 FFmpeg 上下文）可覆写并调基类实现。
     */
    virtual void OnStopRequested() { AbortLinks(); }

    /**
     * @brief 处理单个数据项或一次迭代：各派生类自己实现
     * @return true 继续循环， false 表示自然结束（EOF / 管道 Abort / 错误）
     * @note 由 ThreadMain 在运行态反复调用。节点的全部业务逻辑在此。
     */
    virtual bool Tick() = 0;

    /**
     * @brief 强制终止本节点持有的全部输入与输出管道，唤醒阻塞在其上的等待者
     * @details 停止时必须断开所有管道（输入的等收、输出的等发），否则线程无法从
     *          Receive / Send 返回、回不到循环顶去检查 IsStopRequested() → join 挂死。
     *
     *          声明为纯虚：
     *          - 一进一出节点（继承 INode<InT,OutT>）：使用模板提供的默认实现（基于 input_ / output_）
     *          - 多路节点（直接继承 INodeBase）：自行实现，断开自持的多条 Link
     *
     *          编译器强制派生类实现此函数 → "可能忘"变成"不可能忘" → 从运行时死锁风险
     *          变为编译期约束。这正是卡点 C 修复的教训在更高抽象层的应用。
     *
     * @note 幂等，可重复调用（底层 Abort 自身幂等）；Link 为空指针时安全跳过。
     */
    virtual void AbortLinks(){}

    /// 全局状态标志：atomic state + epoch + 暂停 CV
    std::shared_ptr<SyncContext> sync_ctx_;

private:
    /// 节点名：调试日志用
    std::string name_;

    /// 工作线程句柄：WaitForStop 负责 join
    std::thread thread_;

    /// 是否已经启动：防重入
    std::atomic<bool> running_{false};

    /// 停止请求标志： RequestStop 置位
    std::atomic<bool> stopRequested_{false};
};

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
    using InputLink = Link<InT>;   ///< 输入管道类型别名
    using OutputLink = Link<OutT>; ///< 输出管道类型别名

    /**
     * @brief 构造函数
     * @param name 节点名
     * @param sync 全局控制块
     */
    INode(std::string name, std::shared_ptr<SyncContext> sync) : INodeBase(std::move(name), std::move(sync)) {}

    // ================= 连接接口（Pipeline 组图时调用）=================

    /**
     * @brief 接入上游：把上游的输出 Link 作为本节点的输入
     */
    void ConnectInput(std::shared_ptr<InputLink> input) { input_ = std::move(input); }

    /**
     * @brief 接入下游：把下游的输出 Link 作为本节点的输出
     */
    void ConnectOutput(std::shared_ptr<OutputLink> output) { output_ = std::move(output); }

protected:
    /**
     * @brief 一进一出节点的默认实现：断开 input_ 与 output_
     * @details 覆盖基类纯虚函数。多路节点（直接继承 INodeBase）需自行实现。
     */
    void AbortLinks() override {
        if (input_) {
            input_->Abort();
        }

        if (output_) {
            output_->Abort();
        }
    }

    /// 主输入管道
    std::shared_ptr<InputLink> input_;

    /// 主输出管道
    std::shared_ptr<OutputLink> output_;
};
} // namespace mediacore

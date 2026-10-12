/**
 * @file all_tests.cpp
 * @brief 项目测试入口，目前覆盖 DAG 生命周期和阻塞解除验证。
 *
 * 这些测试只实例化 Queue、Link、INode 和 Pipeline，不创建 FFmpeg 或 Qt 对象。
 * 每个用例都使用小型假 Node，通过条件变量等待确定的事件；测试本身不依赖
 * 固定时长的 sleep 来判断线程是否已经运行。
 */

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "core/pipeline.h"
#include "core/stream_item.h"

namespace {

using namespace std::chrono_literals;
using mediacore::EndOfStream;
using mediacore::INode;
using mediacore::NodeFinishReason;
using mediacore::NoPort;
using mediacore::Pipeline;
using mediacore::PipelineEventCallbacks;
using mediacore::PipelineStartResult;
using mediacore::PipelineState;
using mediacore::TickResult;
using IntegerStreamItem = mediacore::StreamItem<int>;

/** @brief 测试断言失败时抛出的异常。 */
class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

/**
 * @brief 一个可重复使用的测试事件。
 * @details
 * Node 在改变可观察状态后调用 Notify()。等待方使用条件变量谓词，因此事件在
 * 等待前发生时也不会丢失，且不会因虚假唤醒而提前返回。
 */
class TestEvent final {
public:
    TestEvent() = default;

    TestEvent(const TestEvent&) = delete;
    TestEvent& operator=(const TestEvent&) = delete;

    /** @brief 发布一次状态变化并唤醒等待方。 */
    void Notify() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++sequence_;
        }
        condition_.notify_all();
    }

    /**
     * @brief 在截止时间前等待谓词成立。
     * @tparam Predicate 只读取原子或调用方自己保护的数据的谓词
     * @param predicate 要等待的条件
     * @param timeout 最大等待时间
     * @return 谓词成立返回 true，超时返回 false
     */
    template <typename Predicate>
    bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, std::move(predicate));
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::uint64_t sequence_{0};
};

/**
 * @brief 在事件通知可能早于最终状态写入时，按小段条件变量等待检查状态。
 * @details
 * ThreadMain 会先调用派生 Node 的 Tick()，再写入 kFinished/退出原因；因此测试不能
 * 假设 Tick() 内的通知一定发生在最终状态写入之后。分段等待避免使用忙等，同时给
 * 没有后续通知的状态变化留下再次检查机会。
 */
template <typename Predicate>
bool WaitUntil(TestEvent& event, Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto slice = remaining < 10ms ? remaining : 10ms;
        event.WaitFor(predicate, slice);
    }
    return true;
}

/** @brief 假 Node 共用的可观察状态。 */
struct Probe final {
    TestEvent event_;
    std::atomic<int> ticks_{0};
    std::atomic<int> received_{0};
    std::atomic<int> last_value_{0};
    std::atomic<bool> eof_{false};
    std::atomic<bool> first_send_done_{false};
    std::atomic<bool> second_send_entered_{false};
    std::atomic<bool> io_entered_{false};
    std::atomic<bool> interrupted_{false};

    std::mutex gate_mutex_;
    std::condition_variable gate_condition_;
    int released_ticks_{0};

    /** @brief 允许指定数量的 GateNode Tick() 返回。 */
    void ReleaseTick() {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            ++released_ticks_;
        }
        gate_condition_.notify_all();
    }

    /** @brief 允许 GateNode 当前及后续 Tick() 返回，用于 Stop 清理。 */
    void ReleaseAllTicks() {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            released_ticks_ = 1'000'000;
        }
        gate_condition_.notify_all();
    }
};

/** @brief 用异常终止当前测试，避免 assert 在 Release 构建中被移除。 */
void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

/**
 * @brief 依次发送一个整数和 EOS，之后等待 Stop；EOS 不结束工作线程。
 */
class EofSource final : public INode<NoPort, IntegerStreamItem> {
public:
    EofSource(std::shared_ptr<mediacore::SyncContext> sync,
              PipelineEventCallbacks event_callbacks,
              std::shared_ptr<Probe> probe)
        : INode("eof_source", std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        const int tick_index = probe_->ticks_.fetch_add(1) + 1;
        probe_->event_.Notify();

        if (tick_index == 1) {
            const bool sent = SendOutput(7);
            probe_->first_send_done_.store(sent);
            probe_->event_.Notify();
            return sent ? TickResult::kContinue : TickResult::kStopped;
        }

        if (tick_index == 2) {
            return SendOutput(EndOfStream{GetCurrentEpoch()}) ? TickResult::kContinue : TickResult::kStopped;
        }

        std::unique_lock<std::mutex> lock(probe_->gate_mutex_);
        probe_->gate_condition_.wait(lock, [this]() { return IsStopRequested(); });
        return TickResult::kStopped;
    }

    void OnInterruptBlockingIo() noexcept override { probe_->ReleaseAllTicks(); }

private:
    std::shared_ptr<Probe> probe_;
};

/** @brief 对整数加一后发送；StreamReset 和 EOS 原样转发，继续等待下一条输入。 */
class IncrementTransform final : public INode<IntegerStreamItem, IntegerStreamItem> {
public:
    IncrementTransform(std::shared_ptr<mediacore::SyncContext> sync,
                       PipelineEventCallbacks event_callbacks,
                       std::shared_ptr<Probe> probe)
        : INode("increment_transform", std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        probe_->ticks_.fetch_add(1);
        probe_->event_.Notify();

        auto item = ReceiveInput();
        if (!item.has_value()) {
            return TickResult::kStopped;
        }

        const bool is_data = std::holds_alternative<int>(*item);
        if (is_data) {
            ++std::get<int>(*item);
        }
        if (!SendOutput(std::move(*item))) {
            return TickResult::kStopped;
        }
        if (is_data) {
            probe_->received_.fetch_add(1);
        }
        probe_->event_.Notify();
        return TickResult::kContinue;
    }

private:
    std::shared_ptr<Probe> probe_;
};

/** @brief 顺序处理数据和 EOS，收到 EOS 时上报 MediaEnded，随后继续等待输入。 */
class DrainSink final : public INode<IntegerStreamItem, NoPort> {
public:
    DrainSink(std::shared_ptr<mediacore::SyncContext> sync,
              PipelineEventCallbacks event_callbacks,
              std::shared_ptr<Probe> probe)
        : INode("drain_sink", std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        probe_->ticks_.fetch_add(1);
        probe_->event_.Notify();

        auto item = ReceiveInput();
        if (!item.has_value()) {
            return TickResult::kStopped;
        }

        if (const auto* value = std::get_if<int>(&*item)) {
            probe_->last_value_.store(*value);
            probe_->received_.fetch_add(1);
        } else if (const auto* eos = std::get_if<EndOfStream>(&*item)) {
            probe_->eof_.store(true);
            ReportMediaEnded(eos->epoch_);
        }

        probe_->event_.Notify();
        return TickResult::kContinue;
    }

private:
    std::shared_ptr<Probe> probe_;
};

/** @brief 在空 Link 上阻塞 Receive()，用于验证 Pipeline::Stop() 的 Abort。 */
class BlockingReceiveSink final : public INode<int, NoPort> {
public:
    BlockingReceiveSink(std::shared_ptr<mediacore::SyncContext> sync,
                        PipelineEventCallbacks event_callbacks,
                        std::shared_ptr<Probe> probe)
        : INode("blocking_receive_sink", std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        probe_->io_entered_.store(true);
        probe_->event_.Notify();
        const auto item = ReceiveInput();
        return item.has_value() ? TickResult::kContinue : TickResult::kStopped;
    }

private:
    std::shared_ptr<Probe> probe_;
};

/** @brief 填满 Link 后在第二次 Send() 上阻塞的 Source。 */
class BlockingSendSource final : public INode<NoPort, int> {
public:
    BlockingSendSource(std::shared_ptr<mediacore::SyncContext> sync,
                       PipelineEventCallbacks event_callbacks,
                       std::shared_ptr<Probe> probe)
        : INode("blocking_send_source", std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        const int tick_index = probe_->ticks_.fetch_add(1) + 1;
        probe_->event_.Notify();

        if (tick_index == 1) {
            const bool sent = SendOutput(1);
            probe_->first_send_done_.store(sent);
            probe_->event_.Notify();
            return sent ? TickResult::kContinue : TickResult::kStopped;
        }

        probe_->second_send_entered_.store(true);
        probe_->event_.Notify();
        return SendOutput(2) ? TickResult::kContinue : TickResult::kStopped;
    }

private:
    std::shared_ptr<Probe> probe_;
};

/**
 * @brief Tick() 在外部条件下返回的无端口 Node。
 * @details
 * 第 n 次 Tick() 只有在测试方调用 ReleaseTick() 后才返回。这样测试可以先提交
 * Pause，再让 Tick() 到达工作循环边界，确定验证暂停等待而不是验证一个时间窗口。
 */
class GateNode final : public INode<NoPort, NoPort> {
public:
    GateNode(std::shared_ptr<mediacore::SyncContext> sync,
             PipelineEventCallbacks event_callbacks,
             std::shared_ptr<Probe> probe)
        : INode("gate_node", std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        const int tick_index = probe_->ticks_.fetch_add(1) + 1;
        probe_->event_.Notify();

        std::unique_lock<std::mutex> lock(probe_->gate_mutex_);
        probe_->gate_condition_.wait(
            lock, [this, tick_index]() { return probe_->released_ticks_ >= tick_index || IsStopRequested(); });
        return IsStopRequested() ? TickResult::kStopped : TickResult::kContinue;
    }

    void OnInterruptBlockingIo() noexcept override { probe_->ReleaseAllTicks(); }

private:
    std::shared_ptr<Probe> probe_;
};

/** @brief 在 Tick() 中等待 OnInterruptBlockingIo() 的模拟外部 I/O Node。 */
class InterruptibleIoNode final : public INode<NoPort, NoPort> {
public:
    InterruptibleIoNode(std::shared_ptr<mediacore::SyncContext> sync,
                        PipelineEventCallbacks event_callbacks,
                        std::shared_ptr<Probe> probe)
        : INode("interruptible_io", std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        probe_->io_entered_.store(true);
        probe_->event_.Notify();

        std::unique_lock<std::mutex> lock(probe_->gate_mutex_);
        probe_->gate_condition_.wait(lock, [this]() { return probe_->interrupted_.load() || IsStopRequested(); });
        return TickResult::kStopped;
    }

    void OnInterruptBlockingIo() noexcept override {
        {
            std::lock_guard<std::mutex> lock(probe_->gate_mutex_);
            probe_->interrupted_.store(true);
        }
        probe_->event_.Notify();
        probe_->gate_condition_.notify_all();
    }

private:
    std::shared_ptr<Probe> probe_;
};

/** @brief 在第一次 Tick() 抛出异常的 Node。 */
class ThrowingNode final : public INode<NoPort, NoPort> {
public:
    ThrowingNode(std::shared_ptr<mediacore::SyncContext> sync,
                 PipelineEventCallbacks event_callbacks,
                 std::shared_ptr<Probe> probe)
        : INode("throwing_node", std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        probe_->ticks_.fetch_add(1);
        probe_->event_.Notify();
        throw std::runtime_error("all tests exception");
    }

private:
    std::shared_ptr<Probe> probe_;
};

/** @brief 用于启动顺序和部分启动失败测试的可中断无端口 Node。 */
class NamedBlockingNode final : public INode<NoPort, NoPort> {
public:
    NamedBlockingNode(std::shared_ptr<mediacore::SyncContext> sync,
                      PipelineEventCallbacks event_callbacks,
                      std::string name,
                      std::shared_ptr<Probe> probe)
        : INode(std::move(name), std::move(sync), std::move(event_callbacks)), probe_(std::move(probe)) {}

protected:
    TickResult Tick() override {
        probe_->ticks_.fetch_add(1);
        probe_->event_.Notify();
        std::unique_lock<std::mutex> lock(probe_->gate_mutex_);
        probe_->gate_condition_.wait(lock, [this]() { return probe_->interrupted_.load() || IsStopRequested(); });
        return TickResult::kStopped;
    }

    void OnInterruptBlockingIo() noexcept override {
        {
            std::lock_guard<std::mutex> lock(probe_->gate_mutex_);
            probe_->interrupted_.store(true);
        }
        probe_->event_.Notify();
        probe_->gate_condition_.notify_all();
    }

private:
    std::shared_ptr<Probe> probe_;
};

/** @brief 验证 EOS 在数据之后传到 Sink 并上报结束，所有线程继续存活直到 Stop。 */
void TestEosPropagationAndDrain() {
    auto source_probe = std::make_shared<Probe>();
    auto transform_probe = std::make_shared<Probe>();
    auto sink_probe = std::make_shared<Probe>();
    std::atomic<int> ended_count{0};
    std::atomic<std::uint64_t> ended_epoch{0};
    std::atomic<bool> ended_from_sink{false};
    std::atomic<int> received_at_eos{0};
    std::atomic<int> error_count{0};
    PipelineEventCallbacks callbacks;
    callbacks.on_media_ended_ = [&](mediacore::MediaEnded event) {
        ended_epoch.store(event.epoch_);
        ended_from_sink.store(event.node_name_ == "drain_sink");
        received_at_eos.store(sink_probe->received_.load());
        ended_count.fetch_add(1);
        sink_probe->event_.Notify();
    };
    callbacks.on_node_error_ = [&](mediacore::NodeError) {
        error_count.fetch_add(1);
        sink_probe->event_.Notify();
    };
    Pipeline pipeline(std::move(callbacks));
    auto source_link = pipeline.CreateLink<IntegerStreamItem>(2);
    auto sink_link = pipeline.CreateLink<IntegerStreamItem>(2);
    auto& source = pipeline.EmplaceNode<EofSource>(source_probe);
    auto& transform = pipeline.EmplaceNode<IncrementTransform>(transform_probe);
    auto& sink = pipeline.EmplaceNode<DrainSink>(sink_probe);

    Require(source.ConnectOutput(source_link), "source output connection failed");
    Require(transform.ConnectInput(source_link), "transform input connection failed");
    Require(transform.ConnectOutput(sink_link), "transform output connection failed");
    Require(sink.ConnectInput(sink_link), "sink input connection failed");
    Require(pipeline.Start() == PipelineStartResult::kStarted, "EOS graph failed to start");
    Require(pipeline.Resume(), "EOS graph failed to resume");
    Require(sink_probe->event_.WaitFor([&]() { return ended_count.load() != 0; }, 1s),
            "sink did not report MediaEnded");
    Require(sink_probe->eof_.load(), "sink did not observe EOS");
    Require(received_at_eos.load() == 1, "EOS was processed before the queued data");
    Require(ended_epoch.load() == pipeline.GetCurrentEpoch(), "MediaEnded has the wrong epoch");
    Require(ended_from_sink.load(), "MediaEnded was not reported by the terminal sink");
    Require(transform_probe->received_.load() == 1, "transform did not process the queued element");
    Require(sink_probe->received_.load() == 1, "sink did not drain the queued element");
    Require(sink_probe->last_value_.load() == 8, "sink did not receive the transformed value");
    const std::array<const mediacore::INodeBase*, 3> nodes = {&source, &transform, &sink};
    for (const auto* node : nodes) {
        Require(node->GetState() == mediacore::NodeState::kRunning, "EOS ended a Node thread");
        Require(node->GetFinishReason() == NodeFinishReason::kNotFinished, "EOS recorded a thread finish reason");
    }
    Require(pipeline.Stop(), "EOS graph Stop failed");
    Require(pipeline.GetState() == PipelineState::kStopped, "EOS graph is not stopped");
    Require(ended_count.load() == 1, "EOS was not reported exactly once");
    Require(error_count.load() == 0, "EOS graph reported a Node error");
    for (const auto* node : nodes) {
        Require(node->GetState() == mediacore::NodeState::kJoined, "EOS graph left a Node unjoined");
        Require(node->GetFinishReason() == NodeFinishReason::kStopRequested, "Node did not finish due to Stop");
    }
}

/** @brief 验证空队列上的 Receive() 会被 Stop/Abort 唤醒。 */
void TestStopWhileReceiving() {
    Pipeline pipeline;
    auto link = pipeline.CreateLink<int>(1);
    auto probe = std::make_shared<Probe>();
    auto& sink = pipeline.EmplaceNode<BlockingReceiveSink>(probe);
    Require(sink.ConnectInput(link), "receive sink input connection failed");
    Require(pipeline.Start() == PipelineStartResult::kStarted, "receive graph failed to start");
    Require(pipeline.Resume(), "receive graph failed to resume");
    Require(probe->event_.WaitFor([&]() { return probe->io_entered_.load(); }, 1s), "sink did not enter Receive()");
    Require(pipeline.Stop(), "Stop while receiving failed");
    Require(!link->Send(1) && !link->Receive().has_value(), "input Link remained usable after Stop");
    Require(sink.GetState() == mediacore::NodeState::kJoined, "receive sink was not joined");
}

/** @brief 验证满队列上的 Send() 会被 Stop/Abort 唤醒。 */
void TestStopWhileSending() {
    Pipeline pipeline;
    auto link = pipeline.CreateLink<int>(1);
    auto probe = std::make_shared<Probe>();
    auto& source = pipeline.EmplaceNode<BlockingSendSource>(probe);
    Require(source.ConnectOutput(link), "send source output connection failed");
    Require(pipeline.Start() == PipelineStartResult::kStarted, "send graph failed to start");
    Require(pipeline.Resume(), "send graph failed to resume");
    Require(probe->event_.WaitFor([&]() { return probe->second_send_entered_.load(); }, 1s),
            "source did not reach the blocking Send()");
    Require(pipeline.Stop(), "Stop while sending failed");
    Require(!link->Send(1) && !link->Receive().has_value(), "output Link remained usable after Stop");
    Require(source.GetState() == mediacore::NodeState::kJoined, "send source was not joined");
}

/** @brief 验证 Node 在 Tick 返回后进入暂停等待时，Stop 可以将其唤醒。 */
void TestStopWhilePaused() {
    Pipeline pipeline;
    auto probe = std::make_shared<Probe>();
    auto& node = pipeline.EmplaceNode<GateNode>(probe);
    Require(pipeline.Start() == PipelineStartResult::kStarted, "paused graph failed to start");
    Require(pipeline.Resume(), "paused graph failed to resume");
    Require(probe->event_.WaitFor([&]() { return probe->ticks_.load() == 1; }, 1s),
            "gate node did not enter its first Tick()");
    Require(pipeline.Pause(), "Pause failed while running");
    probe->ReleaseTick();
    Require(!probe->event_.WaitFor([&]() { return probe->ticks_.load() >= 2; }, 100ms),
            "Node ran another Tick while paused");
    Require(pipeline.Stop(), "Stop while paused failed");
    Require(node.GetState() == mediacore::NodeState::kJoined, "paused node was not joined");
}

/** @brief 验证 Pause/Resume/Pause 连续操作不会丢失唤醒或重新执行旧 Tick。 */
void TestPauseResumePause() {
    Pipeline pipeline;
    auto probe = std::make_shared<Probe>();
    auto& node = pipeline.EmplaceNode<GateNode>(probe);
    Require(pipeline.Start() == PipelineStartResult::kStarted, "pause/resume graph failed to start");
    Require(pipeline.Resume(), "pause/resume graph failed to resume initially");

    Require(probe->event_.WaitFor([&]() { return probe->ticks_.load() == 1; }, 1s), "first Tick did not start");
    Require(pipeline.Pause(), "first Pause failed");
    probe->ReleaseTick();
    Require(!probe->event_.WaitFor([&]() { return probe->ticks_.load() >= 2; }, 100ms),
            "node crossed the first pause boundary");

    Require(pipeline.Resume(), "Resume failed");
    Require(probe->event_.WaitFor([&]() { return probe->ticks_.load() == 2; }, 1s), "second Tick did not start");
    Require(pipeline.Pause(), "second Pause failed");
    probe->ReleaseTick();
    Require(!probe->event_.WaitFor([&]() { return probe->ticks_.load() >= 3; }, 100ms),
            "node crossed the second pause boundary");

    Require(pipeline.Resume(), "second Resume failed");
    Require(probe->event_.WaitFor([&]() { return probe->ticks_.load() == 3; }, 1s), "third Tick did not start");
    Require(pipeline.Stop(), "Stop after Pause/Resume/Pause failed");
    Require(node.GetState() == mediacore::NodeState::kJoined, "pause/resume node was not joined");
}

/** @brief 验证 Pipeline 会调用 Node 的外部 I/O 中断钩子。 */
void TestInterruptBlockingIo() {
    Pipeline pipeline;
    auto probe = std::make_shared<Probe>();
    auto& node = pipeline.EmplaceNode<InterruptibleIoNode>(probe);
    Require(pipeline.Start() == PipelineStartResult::kStarted, "I/O graph failed to start");
    Require(pipeline.Resume(), "I/O graph failed to resume");
    Require(probe->event_.WaitFor([&]() { return probe->io_entered_.load(); }, 1s),
            "I/O node did not enter its blocking operation");
    Require(pipeline.Stop(), "Stop did not interrupt external I/O");
    Require(probe->interrupted_.load(), "OnInterruptBlockingIo was not called");
    Require(node.GetState() == mediacore::NodeState::kJoined, "I/O node was not joined");
}

/** @brief 验证部分 Node 启动失败时，Pipeline 会回滚已经创建的线程。 */
void TestPartialStartRollback() {
    auto first_probe = std::make_shared<Probe>();
    auto second_probe = std::make_shared<Probe>();
    std::atomic<int> factory_calls{0};
    mediacore::NodeThreadFactory factory = [&](const std::string&, mediacore::NodeThreadEntry entry) {
        const int call_index = factory_calls.fetch_add(1) + 1;
        if (call_index == 2) {
            return std::thread();
        }
        return std::thread(std::move(entry));
    };

    Pipeline pipeline(std::move(factory));
    auto& source = pipeline.EmplaceNode<NamedBlockingNode>("source", first_probe);
    auto& sink = pipeline.EmplaceNode<NamedBlockingNode>("sink", second_probe);
    const PipelineStartResult result = pipeline.Start();
    Require(result == PipelineStartResult::kNodeStartFailed, "partial startup did not report failure");
    Require(factory_calls.load() == 2, "thread factory was not called for both Nodes");
    Require(pipeline.GetState() == PipelineState::kStopped, "partial startup rollback did not stop Pipeline");
    Require(source.GetState() == mediacore::NodeState::kJoined, "started Node was not joined during rollback");
    Require(sink.GetState() == mediacore::NodeState::kJoined, "failed Node was not finalized during rollback");
}

/** @brief 验证重复 Stop() 返回稳定结果，并且不会再次执行停止流程。 */
void TestRepeatedStop() {
    Pipeline pipeline;
    auto probe = std::make_shared<Probe>();
    auto& node = pipeline.EmplaceNode<InterruptibleIoNode>(probe);
    Require(pipeline.Start() == PipelineStartResult::kStarted, "repeat-stop graph failed to start");
    Require(pipeline.Resume(), "repeat-stop graph failed to resume");
    Require(probe->event_.WaitFor([&]() { return probe->io_entered_.load(); }, 1s), "I/O node did not start");
    const bool first_result = pipeline.Stop();
    const bool second_result = pipeline.Stop();
    Require(first_result && second_result, "repeated Stop() did not return the original success result");
    Require(pipeline.GetState() == PipelineState::kStopped, "repeated Stop() changed final state");
    Require(node.GetState() == mediacore::NodeState::kJoined, "repeated Stop() left a joinable Node");
}

/** @brief 验证未处理的 Tick() 异常在线程边界被保存，而不是终止进程。 */
void TestTickException() {
    Pipeline pipeline;
    auto probe = std::make_shared<Probe>();
    auto& node = pipeline.EmplaceNode<ThrowingNode>(probe);
    Require(pipeline.Start() == PipelineStartResult::kStarted, "exception graph failed to start");
    Require(pipeline.Resume(), "exception graph failed to resume");
    Require(WaitUntil(
                probe->event_, [&]() { return node.GetState() == mediacore::NodeState::kFinished; }, 1s),
            "throwing Node did not finish");
    Require(pipeline.Stop(), "Stop after Tick exception failed");
    Require(node.GetFinishReason() == NodeFinishReason::kUnhandledException,
            "Tick exception was not recorded as unhandled");
    Require(static_cast<bool>(node.GetUnhandledException()), "exception pointer was not retained");
}

/** @brief 验证 Stop 后 Node 已 Join，Pipeline 才允许结束其拥有的对象。 */
void TestJoinBeforeDestruction() {
    auto probe = std::make_shared<Probe>();
    auto link_holder = std::shared_ptr<mediacore::Link<int>>();
    {
        Pipeline pipeline;
        link_holder = pipeline.CreateLink<int>(1);
        auto& source = pipeline.EmplaceNode<BlockingSendSource>(probe);
        Require(source.ConnectOutput(link_holder), "join-order source connection failed");
        Require(pipeline.Start() == PipelineStartResult::kStarted, "join-order graph failed to start");
        Require(pipeline.Resume(), "join-order graph failed to resume");
        Require(probe->event_.WaitFor([&]() { return probe->second_send_entered_.load(); }, 1s),
                "join-order source did not block");
        Require(pipeline.Stop(), "join-order Stop failed");
        Require(source.GetState() == mediacore::NodeState::kJoined, "Node was destroyed before Join");
        Require(!link_holder->Send(1) && !link_holder->Receive().has_value(),
                "Link remained usable before Node destruction");
    }
    // Pipeline 已析构，但测试仍持有 shared_ptr；Link 应保持有效且不再被工作线程访问。
    Require(!link_holder->Send(1) && !link_holder->Receive().has_value(),
            "Link did not retain its aborted state after Pipeline destruction");
    link_holder.reset();
}

/** @brief 验证登记顺序为 Source -> Sink 时，线程工厂按 Sink -> Source 被调用。 */
void TestConsumerStartsFirst() {
    std::mutex order_mutex;
    std::vector<std::string> start_order;
    mediacore::NodeThreadFactory factory = [&](const std::string& name, mediacore::NodeThreadEntry entry) {
        {
            std::lock_guard<std::mutex> lock(order_mutex);
            start_order.push_back(name);
        }
        return std::thread(std::move(entry));
    };

    Pipeline pipeline(std::move(factory));
    auto source_probe = std::make_shared<Probe>();
    auto sink_probe = std::make_shared<Probe>();
    auto& source = pipeline.EmplaceNode<NamedBlockingNode>("source", source_probe);
    auto& sink = pipeline.EmplaceNode<NamedBlockingNode>("sink", sink_probe);
    Require(pipeline.Start() == PipelineStartResult::kStarted, "order graph failed to start");
    Require(start_order.size() == 2, "thread factory did not create both Nodes");
    Require(start_order[0] == "sink" && start_order[1] == "source",
            "Pipeline did not start the consumer before the producer");
    Require(pipeline.Stop(), "order graph Stop failed");
    Require(source.GetState() == mediacore::NodeState::kJoined && sink.GetState() == mediacore::NodeState::kJoined,
            "order graph left a Node unjoined");
}

using TestFunction = void (*)();

struct TestCase final {
    const char* name_;
    TestFunction function_;
};

const std::array<TestCase, 11> kTestCases = {{
    {"eos_propagation_and_drain", &TestEosPropagationAndDrain},
    {"stop_while_receiving", &TestStopWhileReceiving},
    {"stop_while_sending", &TestStopWhileSending},
    {"stop_while_paused", &TestStopWhilePaused},
    {"pause_resume_pause", &TestPauseResumePause},
    {"interrupt_blocking_io", &TestInterruptBlockingIo},
    {"partial_start_rollback", &TestPartialStartRollback},
    {"repeated_stop", &TestRepeatedStop},
    {"tick_exception", &TestTickException},
    {"join_before_destruction", &TestJoinBeforeDestruction},
    {"consumer_starts_first", &TestConsumerStartsFirst},
}};

const TestCase* FindTestCase(const std::string& name) {
    for (const TestCase& test_case : kTestCases) {
        if (name == test_case.name_) {
            return &test_case;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    std::string requested_case;
    if (argc == 3 && std::string(argv[1]) == "--case") {
        requested_case = argv[2];
    }

    if (argc != 1 && requested_case.empty()) {
        std::cerr << "usage: all_tests [--case NAME]\n";
        return 2;
    }

    int failures = 0;
    for (const TestCase& test_case : kTestCases) {
        if (!requested_case.empty() && requested_case != test_case.name_) {
            continue;
        }

        try {
            test_case.function_();
            std::cout << "[PASS] " << test_case.name_ << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test_case.name_ << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test_case.name_ << ": unknown exception\n";
        }
    }

    if (!requested_case.empty() && FindTestCase(requested_case) == nullptr) {
        std::cerr << "unknown test case: " << requested_case << '\n';
        return 2;
    }
    return failures == 0 ? 0 : 1;
}

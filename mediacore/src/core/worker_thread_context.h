/**
 * @file worker_thread_context.h
 * @brief 标记当前调用栈是否位于 Pipeline 节点工作线程，供播放器选择安全的线程回收方式。
 * @details 此标记不拥有线程，也不延长节点生命周期。节点线程持有 Scope，用户回调中
 *          销毁 Player 时可以据此避免同步等待控制线程，防止控制线程反向 Join 当前节点。
 */
#pragma once

namespace mediacore {

class WorkerThreadContext final {
public:
    /** @brief 在当前线程标记节点执行范围；嵌套使用时保留外层标记。 */
    class Scope final {
    public:
        Scope() noexcept : was_worker_thread_(is_worker_thread) { is_worker_thread = true; }
        ~Scope() { is_worker_thread = was_worker_thread_; }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;

    private:
        bool was_worker_thread_;
    };

    /** @brief 仅查询调用线程，不读取或访问其他线程的节点对象。 */
    static bool IsWorkerThread() noexcept { return is_worker_thread; }

private:
    inline static thread_local bool is_worker_thread{false};
};

}  // namespace mediacore

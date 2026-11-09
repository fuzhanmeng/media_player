#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>

namespace mediacore::perf {

using Clock = std::chrono::steady_clock;

inline std::int64_t SteadyNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// -1 means unavailable. Thread CPU excludes FFmpeg's separate worker threads;
// process CPU includes all process threads and is sampled only at log boundaries.
inline std::int64_t ThreadCpuNs() noexcept {
#if defined(__linux__)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
        return static_cast<std::int64_t>(value.tv_sec) * 1000000000LL + value.tv_nsec;
#endif
    return -1;
}

inline std::int64_t ProcessCpuNs() noexcept {
#if defined(__linux__)
    timespec value{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0)
        return static_cast<std::int64_t>(value.tv_sec) * 1000000000LL + value.tv_nsec;
#endif
    return -1;
}

// Owner-thread-only accumulator. Merge/read after join or under existing locks.
struct Timing {
    std::uint64_t calls{0};
    std::uint64_t total_ns{0};
    std::uint64_t max_ns{0};
    void Add(std::int64_t ns) noexcept {
        if (ns < 0) return;
        ++calls;
        total_ns += static_cast<std::uint64_t>(ns);
        max_ns = std::max(max_ns, static_cast<std::uint64_t>(ns));
    }
    double TotalMs() const noexcept { return static_cast<double>(total_ns) / 1e6; }
    double AvgMs() const noexcept { return calls ? TotalMs() / static_cast<double>(calls) : 0.0; }
    double MaxMs() const noexcept { return static_cast<double>(max_ns) / 1e6; }
};

// Records an existing operation without waiting for GPU completion.
class ScopedWall final {
public:
    explicit ScopedWall(Timing& total, Timing* interval = nullptr) noexcept
        : total_(total), interval_(interval), start_(SteadyNs()) {}
    ~ScopedWall() {
        const auto ns = SteadyNs() - start_;
        total_.Add(ns);
        if (interval_) interval_->Add(ns);
    }
    ScopedWall(const ScopedWall&) = delete;
    ScopedWall& operator=(const ScopedWall&) = delete;
private:
    Timing& total_;
    Timing* interval_;
    std::int64_t start_;
};

}  // namespace mediacore::perf

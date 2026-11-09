#pragma once

#include <memory>
#include <new>
#include <vector>

// GPU-backed frame resources are safe to release before completion only when no
// GPU command was submitted to read them.
inline constexpr bool IsGpuResourceReleaseSafe(bool gpu_work_submitted, bool completion_confirmed) noexcept {
    return !gpu_work_submitted || completion_confirmed;
}

class DeferredResourceBatch {
public:
    virtual ~DeferredResourceBatch() = default;

    // Return true only after every resource in the batch is safe to destroy.
    virtual bool ReclaimIfSafe() noexcept = 0;
};

class DeferredResourceQueue final {
public:
    bool Defer(std::unique_ptr<DeferredResourceBatch>& batch) noexcept {
        if (batch == nullptr) {
            return true;
        }
        try {
            batches_.push_back(std::move(batch));
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    void Reap() noexcept {
        for (auto it = batches_.begin(); it != batches_.end();) {
            if ((*it)->ReclaimIfSafe()) {
                it = batches_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool Empty() const noexcept {
        return batches_.empty();
    }

    std::size_t Size() const noexcept {
        return batches_.size();
    }

private:
    std::vector<std::unique_ptr<DeferredResourceBatch>> batches_;
};

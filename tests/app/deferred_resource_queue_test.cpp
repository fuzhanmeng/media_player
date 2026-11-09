#include "app/deferred_resource_queue.h"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class FakeBatch final : public DeferredResourceBatch {
public:
    FakeBatch(int& destruction_count, bool gpu_work_submitted, bool completion_confirmed)
        : gpu_work_submitted_(gpu_work_submitted),
          completion_confirmed_(completion_confirmed),
          destruction_count_(destruction_count) {}
    ~FakeBatch() override {
        ++destruction_count_;
    }

    bool ReclaimIfSafe() noexcept override {
        return IsGpuResourceReleaseSafe(gpu_work_submitted_, completion_confirmed_);
    }

    bool gpu_work_submitted_;
    bool completion_confirmed_;

private:
    int& destruction_count_;
};

void KeepsResourcesUntilCompletionIsConfirmed() {
    DeferredResourceQueue queue;
    int destruction_count = 0;
    std::unique_ptr<DeferredResourceBatch> batch = std::make_unique<FakeBatch>(destruction_count, true, false);
    auto* batch_state = static_cast<FakeBatch*>(batch.get());

    Require(queue.Defer(batch), "failed to enqueue deferred resource batch");
    Require(batch == nullptr && queue.Size() == 1, "queue did not take ownership of the resource batch");

    queue.Reap();
    Require(queue.Size() == 1 && destruction_count == 0,
            "resources were released before GPU completion was confirmed");

    batch_state->completion_confirmed_ = true;
    queue.Reap();
    Require(queue.Empty() && destruction_count == 1,
            "resources were not released after GPU completion was confirmed");
}

void ReleasesResourcesWhenNoGpuReadWasSubmitted() {
    DeferredResourceQueue queue;
    int destruction_count = 0;
    std::unique_ptr<DeferredResourceBatch> batch = std::make_unique<FakeBatch>(destruction_count, false, false);

    Require(queue.Defer(batch), "failed to enqueue pre-draw rollback resources");
    queue.Reap();
    Require(queue.Empty() && destruction_count == 1,
            "resources from a pre-draw rollback were retained despite no GPU read being submitted");
}

}  // namespace

int main() {
    KeepsResourcesUntilCompletionIsConfirmed();
    ReleasesResourcesWhenNoGpuReadWasSubmitted();
    std::cout << "deferred_resource_queue_test passed\n";
    return 0;
}

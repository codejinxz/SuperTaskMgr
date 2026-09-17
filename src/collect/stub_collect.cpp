// SKELETON STUB - DELETE THIS FILE when the real collector lands (dev agent D1).
// Minimal CollectService so the GUI shell can link and smoke-run before the real
// collection thread exists: emits empty snapshots on a paced thread, no data.
#include "collect/CollectService.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace stm {

struct CollectService::Impl {
    std::thread worker;
    std::mutex mu;
    std::condition_variable cv;
    bool running = false;
    uint32_t intervalMs = 1000;
    uint64_t ticks = 0;
    double lastTickMs = 0.0;

    void Run(SnapshotStore* store) {
        for (;;) {
            uint32_t wait = intervalMs;
            {
                std::lock_guard<std::mutex> lock(mu);
                if (!running) return;
                wait = intervalMs;
            }
            std::unique_lock<std::mutex> lock(mu);
            if (cv.wait_for(lock, std::chrono::milliseconds(wait),
                            [this] { return !running; })) {
                return;
            }
            lock.unlock();

            const auto t0 = std::chrono::steady_clock::now();
            auto snap = std::make_shared<Snapshot>();
            snap->tickId = ticks + 1;
            snap->tickSec = static_cast<double>(wait) / 1000.0;
            snap->timestamp = static_cast<int64_t>(::time(nullptr));
            store->Set(snap);
            const auto t1 = std::chrono::steady_clock::now();
            lock.lock();
            ++ticks;
            lastTickMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
    }
};

CollectService::CollectService() : impl_(std::make_unique<Impl>()) {}
CollectService::~CollectService() { Stop(); }

bool CollectService::Start(uint32_t intervalMs) {
    SetInterval(intervalMs);
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->running) return true;
    impl_->running = true;
    try {
        impl_->worker = std::thread(&Impl::Run, impl_.get(), &store_);
    } catch (...) {
        impl_->running = false;
        return false;
    }
    return true;
}

void CollectService::Stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!impl_->running) return;
        impl_->running = false;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void CollectService::SetInterval(uint32_t ms) {
    if (ms < 500) ms = 500;
    if (ms > 5000) ms = 5000;
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->intervalMs = ms;
}

uint64_t CollectService::TickCount() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->ticks;
}
double CollectService::LastTickMs() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->lastTickMs;
}
double CollectService::TickP95Ms() const { return impl_->lastTickMs; }
double CollectService::GpuTickP95Ms() const { return 0.0; }
void CollectService::SetGpuEnabled(bool) {}
bool CollectService::GpuEnabled() const { return false; }
void CollectService::SetNetEtwEnabled(bool) {}
bool CollectService::NetEtwEnabled() const { return false; }

}  // namespace stm

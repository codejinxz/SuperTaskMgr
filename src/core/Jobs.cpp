#include "core/Jobs.h"
#include "core/Log.h"
#include <chrono>
#include <windows.h>

namespace stm {

bool JobQueue::Start() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (running_) return true;
        running_ = true;
    }
    try {
        worker_ = std::thread(&JobQueue::Run, this);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        return false;
    }
    return true;
}

uint64_t JobQueue::Submit(std::function<void()> job) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) return 0;
    const uint64_t seq = nextSeq_++;
    jobs_.emplace_back(seq, std::move(job));
    cv_.notify_one();
    return seq;
}

size_t JobQueue::PendingCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return jobs_.size();
}

void JobQueue::Shutdown(uint32_t waitMs) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_ && !worker_.joinable()) return;
        running_ = false;
        while (!jobs_.empty()) {
            STM_LOG_WARN("jobs", L"丢弃未开始的队列任务 seq={}", jobs_.front().first);
            jobs_.pop_front();
        }
    }
    cv_.notify_all();

    if (waitMs > 0) {
        // Give the in-flight job up to waitMs to finish.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!busy_) break;
            }
            Sleep(15);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (busy_ && !worker_.joinable()) return;
        if (busy_) {
            // In-flight job refused to finish in time; detach (process teardown will follow).
            STM_LOG_WARN("jobs", L"在途任务超时未完成，worker 分离退出");
            worker_.detach();
            return;
        }
    }
    if (worker_.joinable()) worker_.join();
}

void JobQueue::Run() {
    for (;;) {
        std::function<void()> job;
        uint64_t seq = 0;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return !jobs_.empty() || !running_; });
            if (jobs_.empty()) {
                if (!running_) return;
                continue;
            }
            seq = jobs_.front().first;
            job = std::move(jobs_.front().second);
            jobs_.pop_front();
            busy_ = true;
        }
        job();
        {
            std::lock_guard<std::mutex> lock(mu_);
            busy_ = false;
        }
    }
}

}  // namespace stm

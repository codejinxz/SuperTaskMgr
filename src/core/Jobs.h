#pragma once
// Serial worker job queue (arch section 5). One worker thread; destructive/slow ops only.
// Exit protocol: Shutdown waits at most waitMs for the in-flight job, drops queued jobs.
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace stm {

class JobQueue {
public:
    JobQueue() = default;
    ~JobQueue() { Shutdown(0); }
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    bool Start();
    // Returns sequence number, or 0 if the queue is not running.
    uint64_t Submit(std::function<void()> job);
    size_t PendingCount() const;  // queued (not including in-flight)
    // waitMs>0: let the in-flight job finish; queued jobs are dropped and logged.
    void Shutdown(uint32_t waitMs);

private:
    void Run();

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<uint64_t, std::function<void()>>> jobs_;
    uint64_t nextSeq_ = 1;
    bool running_ = false;
    bool busy_ = false;
    std::thread worker_;
};

}  // namespace stm

#pragma once
// 串行工作任务队列（架构第 5 节）。单工作线程；仅用于破坏性/耗时操作。
// 退出协议：Shutdown 至多等待 waitMs 让执行中的任务完成，排队任务被丢弃。
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
    // 返回序号；队列未运行时返回 0。
    uint64_t Submit(std::function<void()> job);
    size_t PendingCount() const;  // 仅排队数（不含执行中的任务）
    // waitMs>0：等待执行中的任务完成；排队任务被丢弃并记录日志。
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

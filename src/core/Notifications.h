#pragma once
// 跨线程通知，由 UI 线程每帧统一取空处理（架构第 5 节）。
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace stm {

struct Notification {
    enum class Kind { JobDone, JobFailed, Info, Warn };
    Kind kind = Kind::Info;
    uint64_t seq = 0;      // 所指的 JobQueue 序号（0 = 无）
    std::wstring text;     // 面向用户的中文文案；不含敏感值
};

class NotificationQueue {
public:
    void Push(Notification n);
    void Drain(std::vector<Notification>* out);

private:
    std::mutex mu_;
    std::deque<Notification> items_;
};

}  // namespace stm

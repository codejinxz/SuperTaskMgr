#pragma once
// Cross-thread notifications drained by the UI thread once per frame (arch section 5).
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace stm {

struct Notification {
    enum class Kind { JobDone, JobFailed, Info, Warn };
    Kind kind = Kind::Info;
    uint64_t seq = 0;      // JobQueue sequence this refers to (0 = none)
    std::wstring text;     // user-facing, Chinese; no sensitive values
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

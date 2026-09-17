#include "core/Notifications.h"

namespace stm {

void NotificationQueue::Push(Notification n) {
    std::lock_guard<std::mutex> lock(mu_);
    items_.push_back(std::move(n));
}

void NotificationQueue::Drain(std::vector<Notification>* out) {
    out->clear();
    std::lock_guard<std::mutex> lock(mu_);
    while (!items_.empty()) {
        out->push_back(std::move(items_.front()));
        items_.pop_front();
    }
}

}  // namespace stm

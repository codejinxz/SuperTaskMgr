#pragma once
// Single-instance named mutex (arch section 7). Local\ namespace: per login session.
#include "core/HandleGuard.h"

namespace stm {
namespace ops {

class SingleInstance {
public:
    // Try to acquire "Local\SuperTaskMgr.SingleInstance"; waits up to waitMs
    // (elevation-relaunch handshake relies on old instance releasing it).
    bool TryAcquire(uint32_t waitMs);
    bool Acquired() const { return handle_ != nullptr; }

private:
    UniqueHandle handle_;
};

}  // namespace ops
}  // namespace stm

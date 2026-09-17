#include "ops/SingleInstance.h"
#include <windows.h>

namespace stm::ops {

bool SingleInstance::TryAcquire(uint32_t waitMs) {
    if (handle_) return true;
    UniqueHandle h(CreateMutexW(nullptr, TRUE, L"Local\\SuperTaskMgr.SingleInstance"));
    if (!h) return false;
    const DWORD wait = WaitForSingleObject(h.get(), waitMs);
    // WAIT_ABANDONED also counts: the previous owner died without releasing.
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        handle_ = std::move(h);
        return true;
    }
    return false;  // WAIT_TIMEOUT: another instance still holds it
}

}  // namespace stm::ops

#pragma once
// RAII ownership for Win32 HANDLEs (resource charter, arch section 5).
#include <memory>
#include <windows.h>

namespace stm {

struct HandleCloser {
    void operator()(void* h) const noexcept { if (h) CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

}  // namespace stm

#pragma once
// Win32 HANDLE 的 RAII 所有权管理（资源章程，架构第 5 节）。
#include <memory>
#include <windows.h>

namespace stm {

struct HandleCloser {
    void operator()(void* h) const noexcept { if (h) CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

}  // namespace stm

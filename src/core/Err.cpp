#include "core/Err.h"
#include "core/Str.h"
#include <windows.h>

namespace stm {

uint32_t LastHr() { return HRESULT_FROM_WIN32(GetLastError()); }

std::wstring HrMessage(uint32_t hr) {
    // Etparameter-format: system messages; strip trailing newline noise.
    LPWSTR buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                                       FORMAT_MESSAGE_ALLOCATE_BUFFER,
                                   nullptr, hr, 0,
                                   reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring msg;
    if (n > 0 && buf) {
        msg.assign(buf, n);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) msg.pop_back();
        LocalFree(buf);
    }
    return Fmt(L"{} (0x{:08X})", msg.empty() ? std::wstring(L"未知错误") : msg, hr);
}

std::wstring ErrContext(std::wstring_view what, uint32_t hr) {
    return Fmt(L"{}：{}", what, HrMessage(hr));
}

}  // namespace stm

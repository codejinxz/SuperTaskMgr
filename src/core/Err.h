#pragma once
// Win32 error plumbing. Exceptions must not cross module boundaries (arch section 9).
#include <cstdint>
#include <string>

namespace stm {

uint32_t LastHr();  // HRESULT_FROM_WIN32(GetLastError())
// "拒绝访问。(5)" style message via FormatMessage; falls back to hex code.
std::wstring HrMessage(uint32_t hr);
// Combine "what failed: reason(0x……)" for user-facing error out-params.
std::wstring ErrContext(std::wstring_view what, uint32_t hr);

}  // namespace stm

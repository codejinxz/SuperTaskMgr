#pragma once
// Win32 错误传递设施。异常不得跨模块边界（架构第 9 节）。
#include <cstdint>
#include <string>

namespace stm {

uint32_t LastHr();  // HRESULT_FROM_WIN32(GetLastError())
// "拒绝访问。(5)" style message via FormatMessage; falls back to hex code.
std::wstring HrMessage(uint32_t hr);
// 为面向用户的错误出参拼接 "什么失败：原因(0x……)" 格式的文本。
std::wstring ErrContext(std::wstring_view what, uint32_t hr);

}  // namespace stm

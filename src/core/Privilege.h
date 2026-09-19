#pragma once
// 令牌特权辅助。每次特权启用都会记录日志（可审计，架构第 7 节）。
#include <string>

namespace stm {

bool IsProcessElevated();
// 按名称在进程令牌上启用/禁用特权（如 L"SeDebugPrivilege"）。
// 失败时返回 false 并设置 *err。每次成功的启用/禁用都会记录日志。
bool EnablePrivilege(const wchar_t* name, bool enable, std::wstring* err = nullptr);

}  // namespace stm

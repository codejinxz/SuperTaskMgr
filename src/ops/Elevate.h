#pragma once
// 提权辅助（架构第 7 节）。契约头——归架构所有。
#include <string>

namespace stm {
namespace ops {

bool IsElevated();        // 转发到 core::IsProcessElevated
bool CanElevate();        // 系统支持 UAC 且当前令牌可请求提权

// 以 lpVerb=L"runas" 重启自身。ERROR_CANCELLED 被吞掉（用户拒绝 => false，不退出）。
// 仅当新进程已启动时返回 TRUE——调用方必须立即退出。
// 会话交接（调用前 SaveSession）是调用方的职责，见架构第 7 节。
bool RelaunchAsAdmin(const std::wstring& args);

}  // namespace ops
}  // namespace stm

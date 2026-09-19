#pragma once
// 内置关键进程保护名单（架构第 6 节；契约从 ops 移到 core，
// 使 stm_collect 能标记 PF_Protected 而无需依赖 stm_ops）。
#include <cstdint>
#include <string>

namespace stm {

// 若 (pid,name,path) 受保护则返回非空中文原因字符串，否则返回空。
// 匹配规则：pid 0/4，或按大小写不敏感的映像名与内置名单精确匹配。
std::wstring ProtectedReason(uint32_t pid, const std::wstring& name, const std::wstring& path);

// 便捷封装：受保护时在 *flags 中置位 PF_Protected。
void MarkProtectedFlag(uint32_t pid, const std::wstring& name, const std::wstring& path, uint32_t* flags);

}  // namespace stm

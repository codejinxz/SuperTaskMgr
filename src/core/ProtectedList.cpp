#include "core/ProtectedList.h"
#include "core/ProcData.h"
#include "core/Str.h"

namespace stm {

namespace {
// 大小写不敏感的映像名名单。pid 0/4 在 ProtectedReason 中单独处理。
const wchar_t* const kProtectedNames[] = {
    L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
    L"lsass.exe", L"lsaiso.exe", L"registry.exe", L"memory compression", L"dwm.exe",
};
}  // namespace

std::wstring ProtectedReason(uint32_t pid, const std::wstring& name, const std::wstring& path) {
    (void)path;  // 保留 path 参数，便于将来做父进程链/签名加固
    if (pid == 0) return L"System Idle Process 是系统保留进程";
    if (pid == 4) return L"System 进程是内核进程";
    for (const wchar_t* n : kProtectedNames) {
        if (_wcsicmp(name.c_str(), n) == 0) {
            return Fmt(L"{} 是系统关键进程，终止可能导致系统崩溃或蓝屏", name);
        }
    }
    return {};
}

void MarkProtectedFlag(uint32_t pid, const std::wstring& name, const std::wstring& path, uint32_t* flags) {
    if (!ProtectedReason(pid, name, path).empty()) *flags |= PF_Protected;
}

}  // namespace stm

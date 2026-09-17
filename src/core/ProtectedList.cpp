#include "core/ProtectedList.h"
#include "core/ProcData.h"
#include "core/Str.h"

namespace stm {

namespace {
// Case-insensitive image-name list. pid 0/4 covered separately in ProtectedReason.
const wchar_t* const kProtectedNames[] = {
    L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
    L"lsass.exe", L"lsaiso.exe", L"registry.exe", L"memory compression", L"dwm.exe",
};
}  // namespace

std::wstring ProtectedReason(uint32_t pid, const std::wstring& name, const std::wstring& path) {
    (void)path;  // path kept in signature for future parent-chain / signature hardening
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

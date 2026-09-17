#include "core/Privilege.h"
#include "core/Err.h"
#include "core/HandleGuard.h"
#include "core/Log.h"
#include "core/Str.h"
#include <windows.h>

namespace stm {

bool IsProcessElevated() {
    BOOL isAdmin = FALSE;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    UniqueHandle token(raw);
    DWORD len = 0;
    uint8_t elevation[sizeof(TOKEN_ELEVATION)]{};
    if (!GetTokenInformation(token.get(), TokenElevation, elevation, sizeof(elevation), &len)) return false;
    isAdmin = reinterpret_cast<TOKEN_ELEVATION*>(elevation)->TokenIsElevated;
    return isAdmin != FALSE;
}

bool EnablePrivilege(const wchar_t* name, bool enable, std::wstring* err) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) {
        if (err) *err = ErrContext(L"打开进程令牌失败", LastHr());
        return false;
    }
    UniqueHandle token(raw);
    TOKEN_PRIVILEGES tp{};
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        if (err) *err = ErrContext(Fmt(L"查找特权 {} 失败", name), LastHr());
        return false;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
    if (!AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        if (err) *err = ErrContext(Fmt(L"调整特权 {} 失败", name), LastHr());
        return false;
    }
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        if (err) *err = Fmt(L"令牌未持有特权 {}（需要管理员身份）", name);
        return false;
    }
    STM_LOG_INFO("priv", L"{} 特权已{}", name, enable ? L"启用" : L"释放");
    return true;
}

}  // namespace stm

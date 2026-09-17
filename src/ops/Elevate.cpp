#include "ops/Elevate.h"
#include "core/Err.h"
#include "core/FsUtil.h"
#include "core/Log.h"
#include "core/Privilege.h"
#include "core/Str.h"
#include <windows.h>
#include <shellapi.h>

namespace stm::ops {

bool IsElevated() { return IsProcessElevated(); }

bool CanElevate() {
    // UAC disabled (EnableLUA=0) means runas would silently fail; default true when unset.
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD enableLua = 1, type = 0, size = sizeof(enableLua);
    RegQueryValueExW(key, L"EnableLUA", nullptr, &type,
                     reinterpret_cast<LPBYTE>(&enableLua), &size);
    RegCloseKey(key);
    return enableLua != 0;
}

bool RelaunchAsAdmin(const std::wstring& args) {
    SHELLEXECUTEINFOW se{sizeof(se)};
    se.lpVerb = L"runas";
    se.lpFile = ExePath().c_str();
    se.lpParameters = args.c_str();
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se)) {
        const uint32_t hr = LastHr();
        if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_CANCELLED))) {
            STM_LOG_INFO("elevate", L"用户取消了 UAC 提权");
            return false;
        }
        STM_LOG_WARN("elevate", L"提权重启失败：{}", ErrContext(L"ShellExecuteEx(runas)", hr));
        return false;
    }
    STM_LOG_INFO("elevate", L"已发起提权重启，当前实例即将退出");
    return true;  // caller must exit now (arch section 7)
}

}  // namespace stm::ops

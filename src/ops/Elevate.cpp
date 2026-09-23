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
    // UAC 被禁用（EnableLUA=0）时 runas 会静默失败；未设置时默认为 true。
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
    // 关键：ExePath() 返回临时 std::wstring，若写成 se.lpFile = ExePath().c_str()
    // 则临时对象在本语句结束即销毁 —— lpFile 成为悬垂指针。ShellExecuteExW 在
    // 之后的语句才执行，读到的是已被复用的堆内存（表现为间歇性的
    // "Windows 找不到文件 'xxxx'"）。必须让字符串活到调用结束。
    const std::wstring exePath = ExePath();
    SHELLEXECUTEINFOW se{sizeof(se)};
    se.lpVerb = L"runas";
    se.lpFile = exePath.c_str();
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
    return true;  // 调用方现在必须退出（架构第 7 节）
}

}  // namespace stm::ops

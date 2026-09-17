// Loaded kernel driver enumeration (contract ops/DriverOps.h; R5 row 13).
//
// Windows 11 24H2 hardening (documented on the EnumDeviceDrivers page): without
// SeDebugPrivilege the call "succeeds" but hands back an array whose addresses are
// all NULL. We detect that shape explicitly and — when elevated — retry once with
// SeDebugPrivilege enabled, otherwise fail honestly with 需要管理员权限 instead of
// presenting an empty list as data.
#include "ops/DriverOps.h"
#include "core/Err.h"
#include "core/Log.h"
#include "core/Privilege.h"
#include "core/Str.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <memory>
#include <vector>

#pragma comment(lib, "psapi")

namespace stm::ops {
namespace {

// One grow-retry guarded EnumDeviceDrivers pass. Fills *bases; false on API failure.
bool EnumDriverBases(std::vector<LPVOID>* bases) {
    DWORD needed = 0;
    if (!::EnumDeviceDrivers(nullptr, 0, &needed) && ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }
    bases->clear();
    for (int i = 0; i < 4; ++i) {
        const DWORD count = needed / sizeof(LPVOID);
        if (count == 0) return true;  // empty array is a "successful" answer
        bases->assign(count, nullptr);
        DWORD got = needed;
        if (::EnumDeviceDrivers(bases->data(), got, &needed)) return true;
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    }
    return false;
}

// "\\SystemRoot\system32\drivers\x.sys" -> "%SystemRoot% expanded"; "\\??\C:\..." ->
// device-namespace prefix stripped (task contract). Unmappable kernel paths
// (\Device\...) are returned untouched — honest data beats a fake path.
std::wstring NormalizeDriverPath(const std::wstring& raw) {
    std::wstring p = raw;
    if (p.rfind(L"\\\\SystemRoot\\", 0) == 0 || p.rfind(L"\\SystemRoot\\", 0) == 0) {
        const size_t slash = p.find(L'\\', 1);  // second backslash: end of the marker
        if (slash != std::wstring::npos) {
            wchar_t winDir[MAX_PATH]{};
            UINT n = ::GetSystemWindowsDirectoryW(winDir, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) n = ::GetWindowsDirectoryW(winDir, MAX_PATH);
            if (n > 0 && n < MAX_PATH) return std::wstring(winDir, n) + p.substr(slash);
        }
        return p;
    }
    if (p.rfind(L"\\\\??\\", 0) == 0) return p.substr(5);      // strip leading \??\ marker
    if (p.rfind(L"\\??\\", 0) == 0) return p.substr(4);        // strip short \??\ marker
    return p;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

}  // namespace

std::vector<DriverInfo> EnumDrivers(std::wstring* err) {
    std::vector<DriverInfo> out;
    std::vector<LPVOID> bases;
    if (!EnumDriverBases(&bases)) {
        if (err) *err = stm::ErrContext(L"枚举内核驱动失败", stm::LastHr());
        return out;
    }

    // 24H2 semantics probe: non-empty array whose every base is NULL means the call was
    // neutered for our token (R5 row 13, documented on the EnumDeviceDrivers page).
    // An empty array is treated the same way: a booted Windows always has drivers.
    const bool allZero = std::all_of(bases.begin(), bases.end(),
                                     [](LPVOID p) { return p == nullptr; });
    if (allZero) {
        bool retried = false;
        if (stm::IsProcessElevated()) {
            if (stm::EnablePrivilege(L"SeDebugPrivilege", true, nullptr)) {
                retried = EnumDriverBases(&bases);
                stm::EnablePrivilege(L"SeDebugPrivilege", false, nullptr);  // 用后即释
            } else {
                STM_LOG_WARN("drivers", L"SeDebugPrivilege 启用失败（已提权但仍不可用）");
            }
        }
        bool stillZero = true;
        if (retried) {
            stillZero = std::all_of(bases.begin(), bases.end(),
                                    [](LPVOID p) { return p == nullptr; });
        }
        if (stillZero) {
            STM_LOG_WARN("drivers", L"EnumDeviceDrivers 返回全空地址：24H2 语义，降级为需管理员");
            if (err) {
                *err = L"需要管理员权限（Windows 11 24H2 起，驱动枚举需要 SeDebugPrivilege）";
            }
            return {};
        }
    }

    out.reserve(bases.size());
    for (LPVOID base : bases) {
        if (!base) continue;
        DriverInfo di;
        di.imageBase = reinterpret_cast<uint64_t>(base);

        wchar_t path[MAX_PATH * 2]{};
        // Purpose-built psapi API for kernel base addresses; the EnumDeviceDrivers
        // remarks pattern (GetModuleFileNameEx against the current process) fails on
        // current builds, so it is only a fallback. Confirmed on Win11 23H2 non-admin.
        if (::GetDeviceDriverFileNameW(base, path, MAX_PATH * 2) > 0) {
            di.path = NormalizeDriverPath(path);
            di.name = FileNameOf(di.path);
        } else if (::GetModuleFileNameExW(::GetCurrentProcess(), static_cast<HMODULE>(base),
                                          path, MAX_PATH * 2) > 0) {
            di.path = NormalizeDriverPath(path);
            di.name = FileNameOf(di.path);
        } else {
            STM_LOG_DEBUG("drivers", L"驱动路径解析失败 base=0x{:X}", di.imageBase);
        }
        MODULEINFO mi{};
        if (::GetModuleInformation(::GetCurrentProcess(), static_cast<HMODULE>(base),
                                   &mi, sizeof(mi))) {
            di.imageSize = mi.SizeOfImage;
        }
        out.push_back(std::move(di));
    }
    STM_LOG_INFO("drivers", L"驱动枚举完成：{} 项", out.size());
    if (err) err->clear();
    return out;
}

}  // namespace stm::ops

// 已加载内核驱动枚举（契约 ops/DriverOps.h；R5 第 13 行）。
//
// Windows 11 24H2 的加固（EnumDeviceDrivers 页面有文档）：没有
// SeDebugPrivilege 时调用会"成功"但返回的数组地址
// 全为 NULL。我们显式检测该形态，并且在已提权时带
// SeDebugPrivilege enabled, otherwise fail honestly with 需要管理员权限 instead of
// 而不是把空列表当数据呈现。
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

// 一轮带增长重试保护的 EnumDeviceDrivers。填充 *bases；API 失败返回 false。
bool EnumDriverBases(std::vector<LPVOID>* bases) {
    DWORD needed = 0;
    if (!::EnumDeviceDrivers(nullptr, 0, &needed) && ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }
    bases->clear();
    for (int i = 0; i < 4; ++i) {
        const DWORD count = needed / sizeof(LPVOID);
        if (count == 0) return true;  // 空数组也算"成功"的回答
        bases->assign(count, nullptr);
        DWORD got = needed;
        if (::EnumDeviceDrivers(bases->data(), got, &needed)) return true;
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    }
    return false;
}

// "\\SystemRoot\system32\drivers\x.sys" -> "%SystemRoot% 展开"；"\\??\C:\..." ->
// 去掉设备命名空间前缀（任务契约）。无法映射的内核路径
//（\Device\...）原样返回——诚实数据胜过伪造路径。
std::wstring NormalizeDriverPath(const std::wstring& raw) {
    std::wstring p = raw;
    if (p.rfind(L"\\\\SystemRoot\\", 0) == 0 || p.rfind(L"\\SystemRoot\\", 0) == 0) {
        const size_t slash = p.find(L'\\', 1);  // 第二个反斜杠：标记结束处
        if (slash != std::wstring::npos) {
            wchar_t winDir[MAX_PATH]{};
            UINT n = ::GetSystemWindowsDirectoryW(winDir, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) n = ::GetWindowsDirectoryW(winDir, MAX_PATH);
            if (n > 0 && n < MAX_PATH) return std::wstring(winDir, n) + p.substr(slash);
        }
        return p;
    }
    if (p.rfind(L"\\\\??\\", 0) == 0) return p.substr(5);      // 去掉开头的 \??\ 标记
    if (p.rfind(L"\\??\\", 0) == 0) return p.substr(4);        // 去掉短 \??\ 标记
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

    // 24H2 语义探测：数组非空且每个基址都是 NULL，说明调用被
    // 针对我们的令牌废掉了功能（R5 第 13 行，EnumDeviceDrivers 页面有文档）。
    // 空数组按同样方式处理：启动完成的 Windows 必有驱动。
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
        // psapi 专为内核基址提供的 API；EnumDeviceDrivers 备注里
        // 的模式（对当前进程用 GetModuleFileNameEx）在当前构建上失效，
        // 因此只作兜底。已在 Win11 23H2 非管理员下确认。
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

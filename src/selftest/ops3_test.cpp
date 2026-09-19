// 第 3 阶段 ops 扩展冒烟测试（服务/启动项四来源/驱动）。
// 这里一切都被设计为无需提权即可通过：需要管理员的路径
// 以 [info] 说明跳过（架构第 7 节权限矩阵），绝不断言。
#include "selftest/TestFramework.h"
#include "core/Err.h"
#include "core/FsUtil.h"
#include "core/HandleGuard.h"
#include "core/Str.h"
#include "ops/DriverOps.h"
#include "ops/ServiceOps.h"
#include "ops/StartupOps.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <string>
#include <vector>

namespace {

// RtlGetVersion 是读取版本号的有文档、免疫清单的方式
//（RtlGetVersion 页面；GetVersionEx 在清单背后说谎）。
bool OsBuild(uint32_t* build) {
    using RtlGetVersionFn = long (WINAPI*)(RTL_OSVERSIONINFOW*);
    const HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    const auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(::GetProcAddress(nt, "RtlGetVersion")));
    if (!fn) return false;
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    *build = vi.dwBuildNumber;
    return true;
}

std::vector<std::wstring> ListBackupTxtFiles() {
    std::vector<std::wstring> out;
    const std::wstring dir = stm::LocalAppDataRoot() + L"\\startup_backup";
    WIN32_FIND_DATAW fd{};
    HANDLE raw = ::FindFirstFileExW((dir + L"\\*.txt").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, 0);
    if (raw == INVALID_HANDLE_VALUE) return out;
    do {
        out.push_back(fd.cFileName);
    } while (::FindNextFileW(raw, &fd));
    ::FindClose(raw);
    return out;
}

void DeleteBackupFiles(const std::vector<std::wstring>& names) {
    const std::wstring dir = stm::LocalAppDataRoot() + L"\\startup_backup";
    for (const std::wstring& n : names) ::DeleteFileW((dir + L"\\" + n).c_str());
}

std::wstring NamesDiff(const std::vector<std::wstring>& before,
                       const std::vector<std::wstring>& after) {
    std::wstring added;
    for (const std::wstring& n : after) {
        if (std::find(before.begin(), before.end(), n) == before.end()) {
            if (!added.empty()) added += L", ";
            added += n;
        }
    }
    return added;
}

const stm::ops::StartupItem* FindItem(const std::vector<stm::ops::StartupItem>& items,
                                      const std::wstring& id) {
    for (const auto& it : items) {
        if (it.id == id) return &it;
    }
    return nullptr;
}

}  // namespace

// 服务：批量枚举非管理员可用，包含知名服务，状态与
// pid 正常（dwProcessId 仅对非停止状态有文档保证，R5 7b）。
namespace {
// pid 可打开（存活）返回 true，否则带 *lastErr 返回 false。沙箱
// 一概拒绝跨进程打开——对普通令牌而言 QLI 不可能报 ERROR_ACCESS_DENIED
//（PPL 仍授予 QLI），因此按 envBlocked 上报而不是
// 数据失败（任务规格：环境受限 => 跳过）。
bool PidOpenable(uint32_t pid, uint32_t* lastErr) {
    SetLastError(0);
    stm::UniqueHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (h) return true;
    *lastErr = stm::LastHr();
    return false;
}

const stm::ops::ServiceInfo* FindService(const std::vector<stm::ops::ServiceInfo>& svcs,
                                         const std::wstring& name) {
    for (const auto& s : svcs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}
}  // namespace

STM_TEST(services_enum_ok) {
    std::wstring e;
    std::vector<stm::ops::ServiceInfo> svcs = stm::ops::EnumServices(&e);
    if (svcs.empty()) {
        *err = L"EnumServices 返回空列表：" + e;
        return false;
    }
    bool hasKnown = false;
    int envBlocked = 0;  // 被沙箱拒绝打开的运行中服务 pid
    std::vector<const stm::ops::ServiceInfo*> deadPid;  // 瞬态候选
    for (const auto& s : svcs) {
        if (_wcsicmp(s.name.c_str(), L"Schedule") == 0 || _wcsicmp(s.name.c_str(), L"Themes") == 0) {
            hasKnown = true;
        }
        if (s.name.empty()) {
            *err = L"枚举结果包含空服务名";
            return false;
        }
        if (s.state < 1 || s.state > 7) {
            *err = stm::Fmt(L"服务 {} 状态非法：{}", s.name, s.state);
            return false;
        }
        // pid 仅在运行/暂停状态非零（ops 按 dwProcessId 文档有效性把挂起/停止行
        // 归一化为 0，R5 7b）。这里的死 pid 可能是真实竞争
        //（宿主已退出而 SCM 仍报 RUNNING）——这些服务
        // 失败前会再做一次重枚举，SCM 过期行不至于误报。
        if (s.pid != 0) {
            uint32_t lastErr = 0;
            if (!PidOpenable(s.pid, &lastErr)) {
                if (lastErr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))) {
                    ++envBlocked;
                } else {
                    deadPid.push_back(&s);
                }
            }
        }
    }
    for (const stm::ops::ServiceInfo* s : deadPid) {  // 重枚举一次再判定
        ::Sleep(300);
        svcs = stm::ops::EnumServices(&e);
        const stm::ops::ServiceInfo* again = FindService(svcs, s->name);
        uint32_t lastErr = 0;
        if (again && again->pid != 0 && !PidOpenable(again->pid, &lastErr) &&
            lastErr != static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))) {
            *err = stm::Fmt(L"服务 {} 两次枚举均报告运行中 pid {}，但进程不存在（0x{:08X}）",
                            s->name, s->pid, lastErr);
            return false;
        }
    }
    if (!hasKnown) {
        *err = L"枚举结果不含 Schedule/Themes 之一";
        return false;
    }
    if (envBlocked > 0) {
        printf("  [info] 环境限制：%d 个运行中服务 pid 无法 OpenProcess（沙箱拒绝，视为跳过）\n",
               envBlocked);
    }
    if (!deadPid.empty()) {
        printf("  [info] %zu 个服务的 pid 首查不存在、复核枚举已更新（SCM 瞬态，视为通过）\n",
               deadPid.size());
    }
    printf("  [info] 服务枚举 %zu 项（非管理员）\n", svcs.size());
    return true;
}

// 停止/启动管线错误契约：未知服务必须以非空中文错误失败，
// 依赖查询对同一输入不得崩溃。
// 不触碰真实服务（启停真实服务需要管理员 = 环境跳过）。
STM_TEST(services_stop_unknown_err) {
    std::wstring e;
    if (stm::ops::StopServiceByName(L"stm_selftest_no_such_service", false, &e)) {
        *err = L"停止不存在的服务不应成功";
        return false;
    }
    if (e.empty()) {
        *err = L"停止不存在服务的错误信息为空";
        return false;
    }
    const std::vector<std::wstring> deps = stm::ops::GetDependentServices(L"stm_selftest_no_such_service");
    if (!deps.empty()) {
        *err = L"不存在服务的依赖列表应为空";
        return false;
    }
    return true;
}

// 四个启动来源：枚举不得崩溃、id 必须唯一、且不可能出现
// 静默全失败（空结果 + 空 err）。单来源缺口可容忍
//（按任务规格放宽）但须上报。
STM_TEST(startup_enum_four_sources) {
    std::wstring e;
    const std::vector<stm::ops::StartupItem> items = stm::ops::EnumStartupItems(&e);
    if (items.empty()) {
        if (e.empty()) {
            *err = L"枚举结果为空且错误信息也为空（静默失败）";
        } else {
            *err = L"启动项枚举返回空：" + e;
        }
        return false;
    }
    std::unordered_set<std::wstring> ids;
    size_t regRun = 0, regRun32 = 0, folder = 0, task = 0, uwp = 0;
    for (const auto& it : items) {
        if (it.id.empty() || !ids.insert(it.id).second) {
            *err = L"启动项 id 为空或重复：" + it.id;
            return false;
        }
        switch (it.source) {
            case stm::ops::StartupSource::RegRun: ++regRun; break;
            case stm::ops::StartupSource::RegRun32: ++regRun32; break;
            case stm::ops::StartupSource::StartupFolder: ++folder; break;
            case stm::ops::StartupSource::ScheduledTask: ++task; break;
            case stm::ops::StartupSource::UwpStartupTask: ++uwp; break;
        }
    }
    if (!e.empty()) {
        printf("  [info] 部分源失败（契约允许，err=%s）\n", stm::WideToUtf8(e).c_str());
    }
    if (regRun == 0 || task == 0) {
        printf("  [info] 覆盖放宽：RegRun=%zu RegRun32=%zu 文件夹=%zu 计划任务=%zu UWP=%zu\n",
               regRun, regRun32, folder, task, uwp);
    }
    printf("  [info] 启动项共 %zu 项，id 无重复\n", items.size());
    return true;
}

// 对临时 HKCU Run 值做切换往返（绝不触碰真实用户条目）：
// 创建 -> 可见/启用 -> 禁用（写备份）-> 启用 -> 清理。
// 纯 HKCU，无需管理员。
STM_TEST(startup_toggle_roundtrip) {
    const wchar_t* runSub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const std::wstring id = std::wstring(L"HKCU\\") + runSub + L"\\STMTest3";
    std::wstring e;

    // 0. 快照备份目录，预清理此前中断运行留下的残余。
    const std::vector<std::wstring> before = ListBackupTxtFiles();
    {
        HKEY raw = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, runSub, 0, KEY_SET_VALUE, &raw) != ERROR_SUCCESS) {
            *err = L"打开 HKCU Run 键失败（环境异常）";
            return false;
        }
        ::RegDeleteValueW(raw, L"STMTest3");  // 忽略：可能不存在
        const wchar_t value[] = L"cmd.exe";
        const LONG rc = ::RegSetValueExW(raw, L"STMTest3", 0, REG_SZ,
                                         reinterpret_cast<const BYTE*>(value), sizeof(value));
        ::RegCloseKey(raw);
        if (rc != ERROR_SUCCESS) {
            *err = stm::ErrContext(L"创建临时 Run 值失败", stm::LastHr());
            return false;
        }
    }

    bool ok = false;
    std::wstring newBackups;
    for (;;) {  // 单次循环：break = 清理点
        std::vector<stm::ops::StartupItem> items = stm::ops::EnumStartupItems(&e);
        const stm::ops::StartupItem* it = FindItem(items, id);
        if (!it) {
            e = L"临时启动项未出现在枚举结果中";
            break;
        }
        if (!it->enabled) {
            e = L"新建启动项初始应为启用态";
            break;
        }
        // --- 禁用（必须先写备份）---
        if (!stm::ops::SetStartupEnabled(*it, false, &e)) {
            e = L"禁用失败：" + e;
            break;
        }
        newBackups = NamesDiff(before, ListBackupTxtFiles());
        if (newBackups.empty()) {
            e = L"禁用后未发现新增备份文件";
            break;
        }
        items = stm::ops::EnumStartupItems(&e);
        it = FindItem(items, id);
        if (!it || it->enabled) {
            e = it ? L"禁用后枚举仍显示启用" : L"禁用后项从枚举消失";
            break;
        }
        // --- 启用 ---
        if (!stm::ops::SetStartupEnabled(*it, true, &e)) {
            e = L"启用失败：" + e;
            break;
        }
        items = stm::ops::EnumStartupItems(&e);
        it = FindItem(items, id);
        if (!it || !it->enabled) {
            e = L"启用后枚举仍显示禁用";
            break;
        }
        ok = true;
        break;
    }

    // 清理：临时值 + 本测试创建的所有备份文件。每次切换都写
    // 自己的时间戳备份（评审 V9 P1-2），因此这里重新 diff——
    // 只 diff 禁用后会漏掉启用步骤写的备份。
    newBackups = NamesDiff(before, ListBackupTxtFiles());
    {
        HKEY raw = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, runSub, 0, KEY_SET_VALUE, &raw) == ERROR_SUCCESS) {
            ::RegDeleteValueW(raw, L"STMTest3");
            ::RegCloseKey(raw);
        }
    }
    if (!newBackups.empty()) {
        std::vector<std::wstring> names;
        size_t start = 0;
        for (;;) {
            const size_t comma = newBackups.find(L", ", start);
            names.push_back(newBackups.substr(start, comma == std::wstring::npos
                                                        ? std::wstring::npos
                                                        : comma - start));
            if (comma == std::wstring::npos) break;
            start = comma + 2;
        }
        DeleteBackupFiles(names);
    }
    if (!ok) {
        *err = e;
        return false;
    }
    printf("  [info] 往返成功：禁用(备份 %s) -> 启用 -> 清理\n",
           stm::WideToUtf8(newBackups).c_str());
    return true;
}

// 驱动：21H2-23H2 非管理员必须成功且非空；24H2+ 时诚实的
// "需要管理员" degradation counts as a pass (noted). At least SOME paths must resolve.
STM_TEST(drivers_enum_ok) {
    std::wstring e;
    const std::vector<stm::ops::DriverInfo> drv = stm::ops::EnumDrivers(&e);
    uint32_t build = 0;
    OsBuild(&build);
    if (drv.empty()) {
        if (e.find(L"需要管理员") != std::wstring::npos && build >= 26100) {
            printf("  [info] 24H2+ (build %lu) 非管理员降级为需提权，视为通过：%s\n",
                   static_cast<unsigned long>(build), stm::WideToUtf8(e).c_str());
            return true;
        }
        *err = L"驱动枚举为空（build=" + std::to_wstring(build) + L"）：" + e;
        return false;
    }
    const size_t withPath = static_cast<size_t>(
        std::count_if(drv.begin(), drv.end(),
                      [](const stm::ops::DriverInfo& d) { return !d.path.empty(); }));
    if (withPath == 0) {
        *err = L"全部驱动路径为空（规范化/解析失败）";
        return false;
    }
    if (withPath != drv.size()) {
        printf("  [info] %zu/%zu 驱动解析到路径（内核 \\Device\\ 路径保留原样属正常）\n",
               withPath, drv.size());
    }
    bool systemRootSeen = false;
    for (const auto& d : drv) {
        if (d.path.rfind(L"\\\\SystemRoot\\", 0) == 0 || d.path.rfind(L"\\??\\", 0) == 0) {
            systemRootSeen = true;  // 残留未归一化前缀
        }
    }
    if (systemRootSeen) {
        *err = L"存在未规范化的 \\\\SystemRoot\\/\\??\\ 前缀路径";
        return false;
    }
    printf("  [info] 驱动 %zu 项（build %lu），%zu 项有路径\n",
           drv.size(), static_cast<unsigned long>(build), withPath);
    return true;
}

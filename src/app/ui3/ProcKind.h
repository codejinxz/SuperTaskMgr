#pragma once
// F4#1 区分系统进程：进程类别判定（header-only 纯逻辑，UI 与 stm_selftest 共用）。
//
// 类别判定优先级（从高到低，保证每个类别可达）：
//   Critical   — core::ProtectedReason 非空（保护名单：csrss/lsass 等系统关键进程）
//   ServiceHost— PF_ServiceHost（svchost 类共享宿主；比 "Windows 进程" 更细，故先判）
//   Uwp        — PF_Uwp（UWP 属用户应用，"只看用户进程"时保留）
//   Windows    — 镜像路径位于 %SystemRoot% 之下（大小写不敏感）；路径未知时退化为
//                保守的知名系统进程名表（仅收录无争议的系统组件，绝不猜测用户程序）
//   User       — 其余（默认）
//   Unknown    — 名称与路径皆空且无任何徽标（诚实：无法判定时不假装是用户进程）
//
// 无 ImGui / 无 app 对象依赖；核心函数接受显式 systemRoot 以便纯函数自测，
// 运行时包装 ClassifyProc 用 GetWindowsDirectoryW 取真实根。
#include <cwctype>
#include <string>
#include <windows.h>
#include "core/ProcData.h"
#include "core/ProtectedList.h"

namespace stm {
namespace ui3 {

enum class ProcKind { Critical, Windows, ServiceHost, Uwp, User, Unknown };

// path 未知时的保守知名系统进程名表（大小写不敏感精确匹配 image name）。
// 原则：宁漏勿错 —— 不在该表中的进程一律按 User/Uwp 对待。
inline bool IsWellKnownSystemName(const std::wstring& name) {
    static const wchar_t* const kNames[] = {
        L"system", L"system idle process", L"secure system", L"memory compression",
        L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
        L"lsass.exe", L"lsaiso.exe", L"registry.exe", L"svchost.exe", L"dwm.exe",
        L"fontdrvhost.exe", L"sihost.exe", L"taskhostw.exe", L"audiodg.exe",
        L"spoolsv.exe", L"conhost.exe", L"wudfhost.exe",
    };
    for (const wchar_t* n : kNames) {
        if (_wcsicmp(name.c_str(), n) == 0) return true;
    }
    return false;
}

// 大小写不敏感的 %SystemRoot% 前缀判定；'/' 与 '\\' 等价（逐字符归一后比较），
// 且要求根目录后跟路径分隔符（"C:\Windowsa" 不得匹配 "C:\Windows"）。
inline bool PathUnderSystemRoot(const std::wstring& path, const std::wstring& systemRoot) {
    if (path.empty() || systemRoot.empty()) return false;
    const size_t n = systemRoot.size();
    if (path.size() < n) return false;
    const auto norm = [](wchar_t ch) {
        ch = static_cast<wchar_t>(towlower(ch));
        return ch == L'/' ? L'\\' : ch;
    };
    for (size_t i = 0; i < n; ++i) {
        if (norm(path[i]) != norm(systemRoot[i])) return false;
    }
    if (path.size() == n) return true;  // 恰为根目录本身
    const wchar_t next = norm(path[n]);
    return next == L'\\';
}

// 纯函数入口（systemRoot 显式传入，供自测）。
inline ProcKind ClassifyProc(uint32_t pid, const std::wstring& name, const std::wstring& path,
                             uint32_t flags, const std::wstring& systemRoot) {
    if (!ProtectedReason(pid, name, path).empty()) return ProcKind::Critical;
    if ((flags & PF_ServiceHost) != 0) return ProcKind::ServiceHost;
    if ((flags & PF_Uwp) != 0) return ProcKind::Uwp;
    if (PathUnderSystemRoot(path, systemRoot)) return ProcKind::Windows;
    if (path.empty() && IsWellKnownSystemName(name)) return ProcKind::Windows;
    if (name.empty() && path.empty()) return ProcKind::Unknown;
    return ProcKind::User;
}

// 运行时入口：systemRoot 取自 GetWindowsDirectoryW（%SystemRoot%，如 C:\WINDOWS）。
inline ProcKind ClassifyProc(uint32_t pid, const std::wstring& name, const std::wstring& path,
                             uint32_t flags) {
    wchar_t root[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(root, MAX_PATH);
    const std::wstring systemRoot = len > 0 && len < MAX_PATH ? std::wstring(root) : std::wstring();
    return ClassifyProc(pid, name, path, flags, systemRoot);
}

// 图例/着色文案（UI 以 U8() 渲染；颜色映射在 Pages.cpp，属渲染细节不入纯逻辑）。
inline const wchar_t* ProcKindLegendLabel(ProcKind k) {
    switch (k) {
        case ProcKind::Critical:    return L"系统关键进程";
        case ProcKind::Windows:     return L"Windows 系统进程";
        case ProcKind::ServiceHost: return L"服务宿主";
        case ProcKind::Uwp:         return L"UWP 应用";
        case ProcKind::User:        return L"用户进程";
        default:                    return L"未知";
    }
}

// "只看用户进程" 过滤：隐藏 Critical / Windows / ServiceHost；Uwp 属用户应用保留。
inline bool ProcKindVisibleInUserFilter(ProcKind k) {
    return k != ProcKind::Critical && k != ProcKind::Windows && k != ProcKind::ServiceHost;
}

// cfg "sysDistMode" 的取值与文案（0 关闭 / 1 高亮 / 2 只看用户进程）。
inline const wchar_t* SysDistModeLabel(int mode) {
    switch (mode) {
        case 1:  return L"高亮系统进程";
        case 2:  return L"只看用户进程";
        default: return L"关闭";
    }
}

}  // namespace ui3
}  // namespace stm

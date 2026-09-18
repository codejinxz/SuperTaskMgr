#pragma once
// F4 次梯队 窗口管理：顶层窗口枚举与温和操作的 UI 工具层（header-only）。
//
// 定位说明：本架构 ops 层没有 WindowOps（阶段约束），窗口管理按裁决以
// "文档化 API 的纯 UI 工具"落地 —— 仅使用 EnumWindows / GetWindowThreadProcessId /
// GetWindowTextW / GetClassNameW / IsWindowVisible / PostMessageW(WM_CLOSE) /
// SetWindowPos(HWND_TOPMOST/NOTOPMOST) / ShowWindow，全部非阻塞毫秒级返回，
// 不进 ops 队列也不会卡 UI。不提供"强制结束窗口"（那是进程页终止进程的职责）。
// 失败一律返回 false 并携带 GetLastError 文案（诚实数据）。
//
// 纯文案函数（状态/尺寸标签）供 stm_selftest 覆盖（ui_windowutil_labels）。
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>
#include "core/Str.h"

namespace stm {
namespace ui3 {

struct WindowEntry {
    HWND hwnd = nullptr;
    std::wstring title;      // 窗口标题；空标题显示为 —（页面职责）
    std::wstring className;
    std::wstring procName;   // 进程映像名（best effort；无权限时为空）
    uint32_t pid = 0;
    int32_t width = 0;       // GetWindowRect 尺寸（最小化时是 -32000，如实记录）
    int32_t height = 0;
    bool iconic = false;     // IsIconic
    bool zoomed = false;     // IsZoomed
    bool topmost = false;    // WS_EX_TOPMOST
};

// 状态列文案：最小化 / 最大化 / 正常，置顶追加（已置顶）。
inline std::wstring WindowStatusLabel(const WindowEntry& w) {
    std::wstring base = w.iconic ? L"最小化" : (w.zoomed ? L"最大化" : L"正常");
    if (w.topmost) base += L"（已置顶）";
    return base;
}

// 尺寸列文案："1234×567"；非法尺寸（<=0，如未就绪）显示 "—"。
inline std::wstring WindowSizeLabel(const WindowEntry& w) {
    if (w.width <= 0 || w.height <= 0) return L"—";
    return Fmt(L"{}×{}", w.width, w.height);
}

// ---------------------------------------------------------------------------
// 枚举：仅可见、未 cloak 的顶层窗口（DWMWA_CLOAKED 过滤 UWP 幽灵窗口）。
// 每窗口一次 OpenProcess+QueryFullProcessImageNameW（文档化、快返回），
// 无读取权限时 procName 为空 —— 页面按 "—" 渲染，绝不伪造。
// ---------------------------------------------------------------------------
inline std::vector<WindowEntry> EnumTopLevelWindows(std::wstring* err) {
    std::vector<WindowEntry> out;
    const auto fetch = [&out](HWND hwnd) {
        if (!IsWindowVisible(hwnd)) return;
        // Cloaked 的 UWP 宿主窗口对用户不可见，跳过（诚实呈现用户能看到的东西）。
        BOOL cloaked = FALSE;
        if (const HMODULE dwm = GetModuleHandleW(L"dwmapi.dll")) {
            using Fn = HRESULT(WINAPI*)(HWND, DWORD, VOID*);
            if (const Fn getCloaked =
                    reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(
                        dwm, "DwmGetWindowAttribute")))) {
                if (getCloaked(hwnd, 14 /*DWMWA_CLOAKED*/, &cloaked) == S_OK && cloaked) return;
            }
        }
        WindowEntry w;
        w.hwnd = hwnd;
        wchar_t buf[512] = {};
        if (GetWindowTextW(hwnd, buf, 512) > 0) w.title = buf;
        if (GetClassNameW(hwnd, buf, 512) > 0) w.className = buf;
        w.iconic = IsIconic(hwnd) != FALSE;
        w.zoomed = IsZoomed(hwnd) != FALSE;
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        w.topmost = (ex & WS_EX_TOPMOST) != 0;
        RECT r{};
        if (GetWindowRect(hwnd, &r)) {
            w.width = r.right - r.left;
            w.height = r.bottom - r.top;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        w.pid = static_cast<uint32_t>(pid);
        if (pid != 0) {
            if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                DWORD size = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
                if (QueryFullProcessImageNameW(proc, 0, buf, &size)) {
                    std::wstring full(buf, size);
                    const size_t slash = full.find_last_of(L"\\/");
                    w.procName = slash == std::wstring::npos ? full : full.substr(slash + 1);
                }
                CloseHandle(proc);
            }
        }
        out.push_back(std::move(w));
    };
    if (!EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            (*reinterpret_cast<decltype(fetch)*>(lp))(hwnd);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&fetch))) {
        if (err != nullptr) *err = Fmt(L"EnumWindows 失败（错误码 {}）", GetLastError());
        return {};
    }
    if (err != nullptr) err->clear();
    return out;
}

// 以下操作全部走文档化 API；返回值即 API 成败，失败附带 GetLastError 文案。

// WM_CLOSE 确认框打开期间句柄可能被系统复用（原窗口销毁后新窗口拿到同值
// HWND）：发送关闭消息前复核 IsWindow + pid 与确认时一致（V15-P2-4）。
inline bool WindowMatchesProcess(HWND hwnd, uint32_t expectedPid) {
    if (hwnd == nullptr || !IsWindow(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid != 0 && static_cast<uint32_t>(pid) == expectedPid;
}

// 温和关闭：PostMessageW(WM_CLOSE)——进程可能弹保存提示（确认对话框文案的依据）。
inline bool GracefulCloseWindow(HWND hwnd, std::wstring* err) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        if (err != nullptr) *err = L"窗口已关闭或句柄已失效";
        return false;
    }
    if (!PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
        if (err != nullptr) *err = Fmt(L"发送关闭消息失败（错误码 {}）", GetLastError());
        return false;
    }
    if (err != nullptr) err->clear();
    return true;
}

inline bool SetWindowTopmost(HWND hwnd, bool topmost, std::wstring* err) {
    const BOOL ok = SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!ok) {
        if (err != nullptr) *err = Fmt(L"SetWindowPos 失败（错误码 {}）", GetLastError());
        return false;
    }
    if (err != nullptr) err->clear();
    return true;
}

inline bool MinimizeWindow(HWND hwnd, std::wstring* err) {
    ShowWindow(hwnd, SW_MINIMIZE);  // 返回 FALSE 仅表示此前已是最小化，并非失败
    if (err != nullptr) err->clear();
    return true;
}

// 受前台锁限制可能失败（Windows 限制后台进程抢前台）——失败如实报告。
inline bool ForegroundWindowSafe(HWND hwnd, std::wstring* err) {
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (!SetForegroundWindow(hwnd)) {
        if (err != nullptr)
            *err = L"系统拒绝切换前台（前台锁限制），请重试或从任务栏点击";
        return false;
    }
    if (err != nullptr) err->clear();
    return true;
}

}  // namespace ui3
}  // namespace stm

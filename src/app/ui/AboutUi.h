#pragma once
// ============================================================================
//  关于对话框（H-A）：工具条「关于」按钮（Pages.cpp::DrawToolbar 提交）+ 模态
//  （每帧 BeginPopupModal、Esc 可关，与现有确认框同一模式）。按钮本身在 A1
//  统一风格改造后归工具条所有（与「暂停采集」同款普通按钮）；本单元负责
//  OpenAbout() 触发标志的消费与模态绘制。版本/发布时间/仓库地址/许可证全部
//  来自架构契约头 app/AboutInfo.h，此处不维护第二份发布信息。
//  纯逻辑（标题合成 + 「Windows 版本」运行环境文本）放在本头文件，供
//  selftest/ui_about_test.cpp 无 ImGui 验证。
// ============================================================================
#include <cstdint>
#include <string>

#include "core/Str.h"    // Fmt（WindowsBuildTextWith 组合逻辑用）
#include <windows.h>     // 注册表 / ntdll!RtlGetVersion 真实读取器（内联）

namespace stm {
namespace ui {

// 窗口标题版本后缀规则（与 AboutInfo.h 的 WindowTitleWithVersion() 一致）：
// 版本为空 => 纯标题；非空 => "标题 vX"。AboutInfo.h 是契约头（kAppVersion 是
// 编译期常量，空/非空两分支无法在同一二进制里切换），这里参数化以便单测覆盖。
inline std::wstring AboutWindowTitleFor(const wchar_t* version) {
    std::wstring t = L"超级任务管理器";
    if (version != nullptr && version[0] != L'\0') {
        t += std::wstring(L" ") + version;
    }
    return t;
}

// ---------------------------------------------------------------------------
// 「运行环境 → Windows 版本」文本（A1 任务二）。
// 组合逻辑与真实读取器全部内联在本头文件：stm_selftest 只链接
// core/collect/ops，读不到 AboutUi.cpp —— 头文件内联让单测走与生产完全
// 相同的代码路径。
//
// 读取优先级：
//   1) HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion 的 CurrentBuild
//      (REG_SZ) + UBR (REG_DWORD)，显示 "Build 26100.4652"；UBR 读不到则
//      只显示 "Build 26100"。
//   2) 注册表读不到时：ntdll!RtlGetVersion 动态绑定兜底，仍显示
//      "Build 26100"（GetVersionExW 受 manifest 契约撒谎，不可用）。
//   3) 全部失败 => 如实显示"不可用"（不伪造版本号）。
// ---------------------------------------------------------------------------
struct WindowsBuildReaders {
    // 读注册表 REG_SZ（name = "CurrentBuild"）；out 恒以 '\0' 结尾。
    bool (*regString)(void* ud, const wchar_t* name, wchar_t* out, size_t outChars);
    // 读注册表 REG_DWORD（name = "UBR"）。
    bool (*regDword)(void* ud, const wchar_t* name, uint32_t* out);
    // 兜底：RtlGetVersion 的 dwBuildNumber。
    bool (*rtlBuild)(void* ud, uint32_t* build);
    void* ud;  // 透传给上面三个读取器（selftest 注入伪对象用）。
};

// 组合逻辑（可注入）：注册表优先、RtlGetVersion 兜底、全失败"不可用"。
inline std::wstring WindowsBuildTextWith(const WindowsBuildReaders& r) {
    wchar_t build[32] = {};
    if (r.regString != nullptr && r.regString(r.ud, L"CurrentBuild", build, 32)) {
        uint32_t ubr = 0;
        if (r.regDword != nullptr && r.regDword(r.ud, L"UBR", &ubr)) {
            return Fmt(L"Build {}.{}", build, ubr);
        }
        return std::wstring(L"Build ") + build;
    }
    uint32_t rtlBuild = 0;
    if (r.rtlBuild != nullptr && r.rtlBuild(r.ud, &rtlBuild) && rtlBuild != 0) {
        return Fmt(L"Build {}", rtlBuild);
    }
    return L"不可用";
}

// 真实读取器：HKLM CurrentVersion，KEY_WOW64_64KEY 固定 64 位视图
//（32 位进程也不会被 WOW64 重定向骗到 32 位注册表）。
inline bool WindowsBuildRegString(void* ud, const wchar_t* name, wchar_t* out,
                                  size_t outChars) {
    (void)ud;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = static_cast<DWORD>(outChars * sizeof(wchar_t));
    const LSTATUS st =
        RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, out, &size);
    RegCloseKey(key);
    return st == ERROR_SUCCESS && out[0] != L'\0';
}

inline bool WindowsBuildRegDword(void* ud, const wchar_t* name, uint32_t* out) {
    (void)ud;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD v = 0;
    DWORD size = sizeof(v);
    const LSTATUS st =
        RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) return false;
    *out = v;
    return true;
}

// 兜底读取器：ntdll!RtlGetVersion 动态绑定（ntdll 恒已加载；RtlGetVersion
// 自 XP 起存在且不受 manifest 兼容性垫片影响，probe 已实证 build=22631）。
inline bool WindowsBuildRtlVersion(void* ud, uint32_t* out) {
    (void)ud;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return false;
    const auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(
        GetProcAddress(ntdll, "RtlGetVersion")));
    if (fn == nullptr) return false;
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    *out = vi.dwBuildNumber;
    return vi.dwBuildNumber != 0;
}

// 生产入口：真实读取器装配。系统构建号运行期内不变——函数级 static 只算一次
//（V22-P2-2：原每帧 2 次 RegOpen + 2 次 RegGetValue，约 23µs/帧纯浪费）。
inline std::wstring WindowsBuildText() {
    static const std::wstring cached = [] {
        WindowsBuildReaders r;
        r.regString = &WindowsBuildRegString;
        r.regDword = &WindowsBuildRegDword;
        r.rtlBuild = &WindowsBuildRtlVersion;
        r.ud = nullptr;
        return WindowsBuildTextWith(r);
    }();
    return cached;
}

// 每帧由外壳调用：消费 OpenAbout() 标志（打开关于模态）并绘制模态体。
// 工具条「关于」按钮本身由 Pages.cpp::DrawToolbar 提交（A1 统一风格改造）。
void DrawAboutUi();

// 外部请求打开关于模态（工具条「关于」按钮 / 托盘等入口）：置一次性标志，
// DrawAboutUi 每帧消费。
void OpenAbout();

// 关于页徽标（Logo）：注入 D3D 设备上下文，模态首次打开时从 RCDATA 资源
// 加载 logo_256.png 并创建纹理（进程级缓存；设备/资源缺失时静默不显示）。
void SetAboutGraphics(void* device, void* context);

// 释放徽标纹理（在 D3D 设备销毁前调用；此后关于页仅显示文字，可再次 Set 复活）。
void ShutdownAboutUi();

// --- headless 回归支持（--autotest about） ----------------------------------
// 工具条「关于」按钮的屏幕矩形由 Pages.cpp::DrawToolbar 每帧发布（按钮归其
// 提交）；模态可见性/关闭按钮矩形由 DrawAboutUi 发布。AutotestDialog 的
// AboutClickDriver 据此向真实渲染的控件注入合成鼠标事件（同 dialogclick 法）。
struct AboutAutotestState {
    bool btnValid = false;  // 本帧「关于」按钮已提交且可见
    float btnMinX = 0.0f;
    float btnMinY = 0.0f;
    float btnMaxX = 0.0f;
    float btnMaxY = 0.0f;
    bool modalOpen = false;
    int modalFrames = 0;  // 模态连续提交帧数（单帧化回归哨兵）
    // 诊断（--autotest about）：「关于」按钮本帧是否被 ImGui 判定悬停
    //（驱动据此确认注入按下落在真实按钮上）。
    bool btnHovered = false;
    // 模态内「关闭」按钮矩形（模态打开的帧才有效）——供 --autotest about 用
    // 真实管线点击关闭，验证「打开->保持->关闭」完整回路。
    bool closeValid = false;
    bool closeHovered = false;
    float closeMinX = 0.0f;
    float closeMinY = 0.0f;
    float closeMaxX = 0.0f;
    float closeMaxY = 0.0f;
};
AboutAutotestState& AboutAutotestStateMut();
inline const AboutAutotestState& AboutAutotestStateForAutotest() {
    return AboutAutotestStateMut();
}

}  // namespace ui
}  // namespace stm

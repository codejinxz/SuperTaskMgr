// 关于对话框（H-A）：应用与版本信息、发布时间、可点击仓库地址、运行环境
// （Windows Build.UBR / 权限 / ImGui-ImPlot 版本 / 本次运行时长）与许可证行。
// 所有用户可见文本走 ui::U8()；发布信息只读 app/AboutInfo.h（架构契约头）。
#include "app/ui/AboutUi.h"
#include "app/AboutInfo.h"
#include "app/ui/UiText.h"
#include "ops/Elevate.h"
#include "core/Str.h"
#include "imgui.h"
#include "implot.h"
#include <chrono>
#include <windows.h>
#include <shellapi.h>

namespace stm {
namespace ui {

namespace {

// Windows 版本：HKLM CurrentVersion 的 CurrentBuild（REG_SZ）+ UBR（REG_DWORD），
// 显示为 "Build 26100.4652"；任一读不到则如实显示"不可用"（不伪造版本号）。
std::wstring WindowsBuildText() {
    HKEY key = nullptr;
    constexpr wchar_t kPath[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPath, 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return L"不可用";
    }
    wchar_t build[32] = {};
    DWORD buildSize = sizeof(build);
    const LSTATUS stBuild = RegGetValueW(key, nullptr, L"CurrentBuild", RRF_RT_REG_SZ,
                                         nullptr, build, &buildSize);
    DWORD ubr = 0;
    DWORD ubrSize = sizeof(ubr);
    const LSTATUS stUbr = RegGetValueW(key, nullptr, L"UBR", RRF_RT_REG_DWORD,
                                       nullptr, &ubr, &ubrSize);
    RegCloseKey(key);
    if (stBuild != ERROR_SUCCESS) return L"不可用";
    if (stUbr != ERROR_SUCCESS) return std::wstring(L"Build ") + build;
    return Fmt(L"Build {}.{}", build, ubr);
}

// 本次运行时长（首次调用即进程 UI 启动时刻）；复用核心的 FormatDuration 文案。
std::wstring UptimeText() {
    static const std::chrono::steady_clock::time_point kStart = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - kStart).count();
    return FormatDuration(secs);
}

// 仓库地址行：ImGui 链接样式（主题感知的蓝色 + 悬停手型光标 + 下划线提示）。
// 只有用户主动点击才 ShellExecuteW 打开浏览器（本进程不主动联网）。
void DrawRepoLink() {
    ImGui::TextDisabled("%s", U8(L"仓库地址"));
    ImGui::SameLine(110.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.18f, 0.44f, 0.85f, 1.0f));
    ImGui::TextUnformatted(U8(kRepoUrl));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", U8(std::wstring(L"在浏览器中打开：") + kRepoUrl));
        if (ImGui::IsMouseClicked(0)) {
            ShellExecuteW(nullptr, L"open", kRepoUrl, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

void EnvField(const wchar_t* label, const std::wstring& value) {
    ImGui::TextDisabled("%s", U8(label));
    ImGui::SameLine(110.0f);
    ImGui::TextUnformatted(U8(value));
}

}  // namespace

void DrawAboutUi() {
    // 工具条「?」按钮（定位由调用方 SameLine 决定）。
    if (ImGui::Button(U8(L"?"))) {
        ImGui::OpenPopup("##about");
    }
    if (ImGui::IsItemHovered()) {
        // tooltip 显示当前版本（AboutInfo.h 单一来源）。
        ImGui::SetTooltip("%s", U8(kAppVersion[0] ? std::wstring(L"关于（版本 ") +
                                                       kAppVersion + L"）"
                                                 : std::wstring(L"关于")));
    }

    // 模态：与确认框相同的每帧 Begin 模式 —— Esc 关闭后 Begin 返回 false 且
    // 弹窗不在栈中，无待清理状态（纯展示对话框）。
    if (!ImGui::IsPopupOpen("##about")) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("##about", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    // 应用名 + 版本（发布信息单一来源：AboutInfo.h）。
    ImGui::TextUnformatted(U8(kAppVersion[0] ? std::wstring(kAppName) + L" " + kAppVersion
                                             : std::wstring(kAppName)));
    if (kBuildDate[0] != L'\0') {  // 发布时间为空则隐藏该行
        ImGui::TextDisabled("%s", U8(Fmt(L"发布时间：{}", kBuildDate)));
    }
    if (kRepoUrl[0] != L'\0') {    // 仓库地址为空则隐藏该行
        DrawRepoLink();
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", U8(L"运行环境"));
    EnvField(L"Windows 版本", WindowsBuildText());
    EnvField(L"运行权限", ops::IsElevated() ? L"管理员" : L"普通权限");
    EnvField(L"界面框架", Fmt(L"ImGui {} / ImPlot {}",
                              Utf8ToWide(IMGUI_VERSION), Utf8ToWide(IMPLOT_VERSION)));
    EnvField(L"本次运行时长", UptimeText());

    ImGui::Separator();
    ImGui::TextWrapped("%s", U8(kLicenseLine));

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
    if (ImGui::Button(U8(L"关闭"), ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace ui
}  // namespace stm

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
#include <d3d11.h>
#include <stb_image.h>
#include <wrl/client.h>
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

// 关于页徽标（Logo agent 交付的资源接线）：RC 内嵌 logo_256.png → stb 解码 →
// D3D11 纹理。进程级缓存一次加载；设备/资源缺失时静默不显示（headless 安全）。
void* g_logoDevice = nullptr;
void* g_logoContext = nullptr;
ImTextureRef g_logoTexture;
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> g_logoSrv;
bool g_logoTried = false;

void EnsureAboutLogo() {
    if (g_logoTried || !g_logoDevice) return;
    g_logoTried = true;
    const HRSRC rc = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(200), RT_RCDATA);
    const HGLOBAL handle = rc ? LoadResource(GetModuleHandleW(nullptr), rc) : nullptr;
    const uint8_t* data = handle ? static_cast<const uint8_t*>(LockResource(handle)) : nullptr;
    const DWORD size = rc ? SizeofResource(GetModuleHandleW(nullptr), rc) : 0;
    if (!data || size == 0) return;
    int w = 0, h = 0, comp = 0;
    uint8_t* rgba = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!rgba) return;
    auto* dev = static_cast<ID3D11Device*>(g_logoDevice);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init { rgba, static_cast<UINT>(w) * 4, 0 };
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(dev->CreateTexture2D(&desc, &init, tex.GetAddressOf())) &&
        SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, g_logoSrv.GetAddressOf()))) {
        // 1.92 后端无托管纹理注册表：裸 SRV 指针即最终绑定句柄（同 Wallpaper）。
        g_logoTexture = ImTextureRef(static_cast<void*>(g_logoSrv.Get()));
    }
    stbi_image_free(rgba);
}

}  // namespace

void SetAboutGraphics(void* device, void* context) {
    g_logoDevice = device;
    g_logoContext = context;
}

void ShutdownAboutUi() {
    g_logoSrv.Reset();       // 先于 D3D 设备销毁释放徽标纹理（V20-P2-3）
    g_logoTexture = ImTextureRef();
    g_logoTried = false;     // 允许设备复活后重新加载
    g_logoDevice = nullptr;
    g_logoContext = nullptr;
}

void DrawAboutUi() {
    AboutAutotestState& at = AboutAutotestStateMut();
    at.btnValid = false;  // 每帧重置：只有本帧真实提交过按钮才有效
    at.btnHovered = false;

    // 工具条「?」按钮（定位由调用方 SameLine 决定）。
    if (ImGui::Button(U8(L"?"))) {
        ImGui::OpenPopup("##about");
    }
    if (ImGui::IsItemVisible()) {  // 已提交且未被裁剪：记录矩形（--autotest about）
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        at.btnValid = true;
        at.btnMinX = mn.x;
        at.btnMinY = mn.y;
        at.btnMaxX = mx.x;
        at.btnMaxY = mx.y;
        at.btnHovered = ImGui::IsItemHovered();
    }
    if (ImGui::IsItemHovered()) {
        // tooltip 显示当前版本（AboutInfo.h 单一来源）。
        ImGui::SetTooltip("%s", U8(kAppVersion[0] ? std::wstring(L"关于（版本 ") +
                                                       kAppVersion + L"）"
                                                 : std::wstring(L"关于")));
    }

    // 模态：与确认框相同的每帧 Begin 模式 —— Esc 关闭后 Begin 返回 false 且
    // 弹窗不在栈中，无待清理状态（纯展示对话框）。
    if (!ImGui::IsPopupOpen("##about")) {
        at.modalOpen = false;
        at.modalFrames = 0;
        return;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0f, 0.0f), ImVec2(460.0f, FLT_MAX));
    if (!ImGui::BeginPopupModal("##about", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        at.modalOpen = false;
        at.modalFrames = 0;
        return;
    }
    at.modalOpen = true;
    ++at.modalFrames;  // 连续提交帧数（单帧化回归哨兵，同确认框 kMinOpenFrames）

    // 应用名 + 版本（发布信息单一来源：AboutInfo.h）。徽标在标题左侧（缺失时仅文字）。
    EnsureAboutLogo();
    if (g_logoSrv) {
        ImGui::Image(g_logoTexture, ImVec2(56.0f, 56.0f));
        ImGui::SameLine(72.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    }
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
    {
        // --autotest about：发布「关闭」按钮矩形（真实管线关闭路径）。
        // 每帧重置（V19-P2-2）：模态意外关闭时不得残留上一帧的哨兵脏读。
        at.closeValid = false;
        at.closeHovered = false;
        at.closeValid = ImGui::IsItemVisible();
        at.closeHovered = ImGui::IsItemHovered();
        const ImVec2 cmn = ImGui::GetItemRectMin();
        const ImVec2 cmx = ImGui::GetItemRectMax();
        at.closeMinX = cmn.x;
        at.closeMinY = cmn.y;
        at.closeMaxX = cmx.x;
        at.closeMaxY = cmx.y;
    }
    ImGui::EndPopup();
}

AboutAutotestState& AboutAutotestStateMut() {
    static AboutAutotestState s;  // UI 线程单例（同 shell 其余每帧状态）
    return s;
}

}  // namespace ui
}  // namespace stm

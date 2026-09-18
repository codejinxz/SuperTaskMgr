#include "app/Theme.h"
#include "imgui.h"
#include <windows.h>

namespace stm {

namespace {

// 最近一次 Apply 的实际观感（System 已解析为深/浅）。仅 UI 线程访问：
// 启动 Apply、外观菜单切换与 WM_SETTINGCHANGE 处理都在窗口线程上。
bool g_effectiveLight = false;

// 壁纸后端（main 注册一次；仅 UI 线程访问，无需同步）。
void* g_wpDevice = nullptr;
void* g_wpContext = nullptr;
bool g_appearanceSmokePreview = false;

bool ReadAppsUseLightTheme(void* /*ud*/, bool* light) {
    HKEY key = nullptr;
    constexpr wchar_t kPath[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD v = 0;
    DWORD size = sizeof(v);
    const LSTATUS st = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                                        reinterpret_cast<LPBYTE>(&v), &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || size < sizeof(v)) return false;  // 缺值 => 诚实失败 => 深色
    *light = v != 0;
    return true;
}

// 两种模式共用的布局值：切换主题时圆角/间距完全一致，界面不回流。
void ApplyLayout(ImGuiStyle& s) {
    s.WindowRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.WindowBorderSize = 1.0f;
}

void ApplyDark() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.090f, 0.106f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.106f, 0.110f, 0.129f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.118f, 0.122f, 0.145f, 0.98f);
    c[ImGuiCol_FrameBg] = ImVec4(0.157f, 0.165f, 0.196f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.196f, 0.208f, 0.247f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.235f, 0.247f, 0.294f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.106f, 0.110f, 0.129f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.227f, 0.431f, 0.647f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.227f, 0.431f, 0.647f, 0.80f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.278f, 0.474f, 0.702f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.106f, 0.110f, 0.129f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.227f, 0.431f, 0.647f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.137f, 0.145f, 0.173f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    c[ImGuiCol_CheckMark] = ImVec4(0.400f, 0.620f, 0.878f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.400f, 0.620f, 0.878f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.549f, 0.714f, 0.925f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.090f, 0.094f, 0.110f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.502f, 0.522f, 0.580f, 1.0f);
}

void ApplyLight() {
    ImGui::StyleColorsLight();
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    // 协调的浅灰窗体 + 深色文字（StyleColorsLight 基础上统一到与深色版同一套
    // 蓝色强调，圆角等布局值由 ApplyLayout 保证一致）。
    c[ImGuiCol_WindowBg] = ImVec4(0.960f, 0.961f, 0.968f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.992f, 0.992f, 1.000f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.984f, 0.984f, 0.992f, 0.99f);
    c[ImGuiCol_FrameBg] = ImVec4(0.900f, 0.905f, 0.920f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.858f, 0.865f, 0.885f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.815f, 0.824f, 0.850f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.878f, 0.882f, 0.898f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.878f, 0.882f, 0.898f, 1.0f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.925f, 0.928f, 0.940f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.227f, 0.431f, 0.647f, 0.45f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.227f, 0.431f, 0.647f, 0.70f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.180f, 0.380f, 0.590f, 1.0f);
    // 选中页签用浅蓝底（保持 StyleColorsLight 的深色文字可读）。
    c[ImGuiCol_Tab] = ImVec4(0.900f, 0.905f, 0.920f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.790f, 0.865f, 0.960f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.872f, 0.878f, 0.895f, 1.0f);
    // 斑马纹：白底上叠加 4.5% 黑，浅色下同样可辨。
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.0f, 0.0f, 0.0f, 0.045f);
    c[ImGuiCol_CheckMark] = ImVec4(0.149f, 0.380f, 0.722f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.149f, 0.380f, 0.722f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.102f, 0.310f, 0.630f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.935f, 0.938f, 0.950f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.420f, 0.432f, 0.465f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.680f, 0.690f, 0.720f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.760f, 0.768f, 0.790f, 1.0f);
}

}  // namespace

ThemeMode ResolveSystem() { return ResolveSystemWith(&ReadAppsUseLightTheme, nullptr); }

bool ThemeIsLight() { return g_effectiveLight; }

void RegisterWallpaperBackend(void* d3dDevice, void* d3dContext) {
    g_wpDevice = d3dDevice;
    g_wpContext = d3dContext;
}
void* WallpaperBackendDevice() { return g_wpDevice; }
void* WallpaperBackendContext() { return g_wpContext; }

void SetAppearanceSmokePreview(bool on) { g_appearanceSmokePreview = on; }
bool AppearanceSmokePreview() { return g_appearanceSmokePreview; }

std::uint32_t ThemeAccentColor(ThemeAccent a) {
    if (g_effectiveLight) {
        // 浅色：加深到白底可读（对比度优先，色相不变）。
        switch (a) {
            case ThemeAccent::Done:           return IM_COL32(26, 127, 66, 255);
            case ThemeAccent::Fail:           return IM_COL32(178, 34, 34, 255);
            case ThemeAccent::Warn:           return IM_COL32(184, 122, 11, 255);
            case ThemeAccent::Info:           return IM_COL32(30, 105, 190, 255);
            case ThemeAccent::KindCritical:   return IM_COL32(178, 34, 34, 255);
            case ThemeAccent::KindWindows:    return IM_COL32(47, 86, 133, 255);
            case ThemeAccent::KindServiceHost:return IM_COL32(10, 122, 122, 255);
            case ThemeAccent::KindUwp:        return IM_COL32(120, 66, 185, 255);
            case ThemeAccent::KindUser:       return IM_COL32(70, 72, 78, 255);
            case ThemeAccent::Purple:         return IM_COL32(108, 56, 168, 255);
            case ThemeAccent::Gray:           return IM_COL32(96, 96, 102, 255);
        }
    }
    // 深色：维持既有配色（Pages.cpp 原硬编码值，语义不变）。
    switch (a) {
        case ThemeAccent::Done:           return IM_COL32(115, 204, 115, 255);
        case ThemeAccent::Fail:           return IM_COL32(235, 92, 92, 255);
        case ThemeAccent::Warn:           return IM_COL32(242, 199, 77, 255);
        case ThemeAccent::Info:           return IM_COL32(140, 184, 242, 255);
        case ThemeAccent::KindCritical:   return IM_COL32(235, 92, 92, 255);
        case ThemeAccent::KindWindows:    return IM_COL32(158, 179, 204, 255);
        case ThemeAccent::KindServiceHost:return IM_COL32(89, 204, 204, 255);
        case ThemeAccent::KindUwp:        return IM_COL32(184, 140, 235, 255);
        case ThemeAccent::KindUser:       return IM_COL32(217, 217, 217, 255);
        case ThemeAccent::Purple:         return IM_COL32(191, 140, 242, 255);
        case ThemeAccent::Gray:           return IM_COL32(153, 153, 153, 255);
    }
    return IM_COL32(255, 255, 255, 255);  // 不可达（枚举全覆盖）；-W4 也需要返回值
}

void Theme::Apply(ThemeMode m) {
    bool light = false;
    switch (m) {
        case ThemeMode::Dark: light = false; break;
        case ThemeMode::Light: light = true; break;
        case ThemeMode::System: light = ResolveSystem() == ThemeMode::Light; break;
    }
    ImGuiStyle& s = ImGui::GetStyle();
    ApplyLayout(s);
    if (light) {
        ApplyLight();
    } else {
        ApplyDark();
    }
    g_effectiveLight = light;
}

}  // namespace stm

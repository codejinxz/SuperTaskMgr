#pragma once
// ============================================================================
//  三态主题（深色 / 浅色 / 跟随系统）+ 外观持久化辅助。
//  本头文件刻意保持无 ImGui / 无 OS 依赖（仅标准库），stm_selftest
//  （selftest/ui_about_test.cpp）直接包含它做纯逻辑验证；ImGui 样式落地在
//  Theme.cpp，UI 侧取色经 ThemeAccentColor()（打包 0xAABBGGRR，调用处转换）。
// ============================================================================
#include <cstdint>

namespace stm {

// cfg 键 "themeMode"（默认 0=Dark；跟随系统 = 每次解析注册表实时值）。
enum class ThemeMode : int { Dark = 0, Light = 1, System = 2 };

// 持久化原始值 -> 合法模式；越界/未知一律回退 Dark（诚实降级）。
inline ThemeMode ThemeModeFromInt(std::int64_t v) {
    if (v < 0 || v > static_cast<std::int64_t>(ThemeMode::System)) return ThemeMode::Dark;
    return static_cast<ThemeMode>(static_cast<int>(v));
}

// 可注入的系统主题探测器（HKCU\...\Themes\Personalize AppsUseLightTheme）。
// 契约：reader 返回 false = 读不到（键缺失/权限不足）=> Resolve 结果为 Dark。
using AppsThemeReader = bool (*)(void* ud, bool* light);

// 注入版（header-only 纯逻辑，selftest 直接验证；真实探测 ResolveSystem 的实现
// 在 Theme.cpp，走注册表）。
inline ThemeMode ResolveSystemWith(AppsThemeReader reader, void* ud) {
    bool light = false;
    if (reader == nullptr || !reader(ud, &light)) return ThemeMode::Dark;  // 契约：读不到=深色
    return light ? ThemeMode::Light : ThemeMode::Dark;
}
ThemeMode ResolveSystem();

class Theme {
public:
    // 按模式应用整套 ImGui 样式（两种模式圆角/间距等布局值一致，切换不回流）。
    // System 在此实时解析注册表；启动与运行期切换共用同一入口。
    static void Apply(ThemeMode m = ThemeMode::Dark);
};

// 最近一次 Apply 后的实际观感（System 已解析）；仅 UI 线程读写。
bool ThemeIsLight();

// ============================================================================
//  模式感知的界面强调色（深/浅色各一套）。返回 IM_COL32 布局的 0xAABBGGRR，
//  调用处用 ImGui::ColorConvertU32ToFloat4 转换 —— 这样本头文件无需 imgui.h。
//  原先散落在 Pages.cpp 的深色硬编码色（徽标/类别高亮/结果色）统一走这里，
//  保证浅色模式下斑马纹表格与彩色徽标仍然可读。
// ============================================================================
enum class ThemeAccent : int {
    Done,                                        // 操作成功 / 完成提示
    Fail,                                        // 失败 / 危险动作
    Warn,                                        // 警告（深色亮琥珀 / 浅色深琥珀）
    Info,                                        // 信息 / 链接蓝
    KindCritical, KindWindows, KindServiceHost,  // 进程类别高亮（F4#1）
    KindUwp, KindUser,
    Purple,                                      // 服务宿主徽标 "S"
    Gray,                                        // 挂起徽标 "Z"
};
std::uint32_t ThemeAccentColor(ThemeAccent a);

// ============================================================================
//  壁纸后端注册（Phase-6 接线）。WallpaperLoad/Clear 需要渲染器不暴露给 UI 层的
//  D3D11 对象，main 在 renderer.Init + ui.Init 之后注册一次；仅 UI 线程使用
//  （D3D11 立即上下文非线程安全 —— 壁纸加载固定走 UI 线程同步调用）。
// ============================================================================
void RegisterWallpaperBackend(void* d3dDevice, void* d3dContext);
void* WallpaperBackendDevice();
void* WallpaperBackendContext();

// --smoke 下把「外观→自定义壁纸」控件组画进离屏窗口（覆盖滑条/状态行/性能提示
// 的渲染路径；选图对话框与加载/清除按钮的点击路径无法 headless 驱动，见菜单注释）。
void SetAppearanceSmokePreview(bool on);
bool AppearanceSmokePreview();

}  // namespace stm

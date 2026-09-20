#pragma once
// ============================================================================
//  RELEASE INFO — EDIT THIS FILE WHEN PUBLISHING A NEW VERSION (架构师注释)
//
//  发布新版本时只需要改下面三个值：
//    1. kAppVersion   版本号，显示在"关于"对话框与窗口标题
//    2. kBuildDate    发布时间（自由格式，建议 "YYYY-MM-DD"）
//    3. kRepoUrl      GitHub 仓库地址（显示在"关于"里，可点击打开；留空则隐藏该行）
//
//  同步提示：src/app/app.manifest 里的 assemblyIdentity version 建议一并更新。
//  其余信息（构建时的 ImGui/ImPlot 版本、系统信息）运行时自动填充。
// ============================================================================
#include <string>

namespace stm {

inline constexpr const wchar_t* kAppName     = L"SuperTaskMgr 超级任务管理器";
inline constexpr const wchar_t* kAppVersion  = L"1.0.0";   // <-- 在此编辑（发布时修改）
inline constexpr const wchar_t* kBuildDate   = L"2026-09-21";             // <-- 在此编辑（如 L"2026-09-18"）
inline constexpr const wchar_t* kRepoUrl     = L"";             // <-- 在此编辑（如 L"https://github.com/yourname/SuperTaskMgr"）

inline constexpr const wchar_t* kLicenseLine = L"本项目基于 MIT 许可证发布（见 LICENSE）。第三方组件：Dear ImGui / ImPlot (MIT)、stb_image (公有领域)。";

// 窗口标题用：版本非空时追加 " vX"
inline std::wstring WindowTitleWithVersion() {
    std::wstring t = L"超级任务管理器";
    if (kAppVersion[0]) t += std::wstring(L" ") + kAppVersion;
    return t;
}

}  // namespace stm

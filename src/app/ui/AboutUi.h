#pragma once
// ============================================================================
//  关于对话框（H-A）：工具条「?」按钮 + 模态（每帧 BeginPopupModal、Esc 可关，
//  与现有确认框同一模式）。版本/发布时间/仓库地址/许可证全部来自架构契约头
//  app/AboutInfo.h，此处不维护第二份发布信息。
//  纯逻辑（标题合成）放在本头文件，供 selftest/ui_about_test.cpp 无 ImGui 验证。
// ============================================================================
#include <string>

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

// 每帧由外壳工具条调用：绘制「?」按钮（tooltip 显示当前版本）+ 关于模态。
// 调用方先用 ImGui::SameLine(...) 定位（按钮放在工具条右端）。
void DrawAboutUi();

}  // namespace ui
}  // namespace stm

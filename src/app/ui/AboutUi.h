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

// 关于页徽标（Logo）：注入 D3D 设备上下文，模态首次打开时从 RCDATA 资源
// 加载 logo_256.png 并创建纹理（进程级缓存；设备/资源缺失时静默不显示）。
void SetAboutGraphics(void* device, void* context);

// 释放徽标纹理（在 D3D 设备销毁前调用；此后关于页仅显示文字，可再次 Set 复活）。
void ShutdownAboutUi();

// --- headless 回归支持（R-Fix Bug3，--autotest about） -----------------------
// DrawAboutUi 每帧刷新「?」按钮的屏幕矩形与模态可见性；AutotestDialog 的
// AboutClickDriver 据此向真实渲染的按钮注入合成鼠标事件（同 dialogclick 法）。
struct AboutAutotestState {
    bool btnValid = false;  // 本帧「?」按钮已提交且可见
    float btnMinX = 0.0f;
    float btnMinY = 0.0f;
    float btnMaxX = 0.0f;
    float btnMaxY = 0.0f;
    bool modalOpen = false;
    int modalFrames = 0;  // 模态连续提交帧数（单帧化回归哨兵）
    // R-Fix 诊断（--autotest about）：「?」按钮本帧是否被 ImGui 判定悬停
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

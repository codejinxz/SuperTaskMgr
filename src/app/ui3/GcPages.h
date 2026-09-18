#pragma once
// F4 落地批（G-C）新增页面与共享模态的注册入口。
//  - RegisterGcPages：追加"崩溃记录"（F4#5）与"窗口"（F4 次梯队）两个 IPage，
//    由 app/ui/Pages.cpp 的 RegisterPages 在 phase-3 页之后调用。
//  - DrawGcModals：每帧由 DrawShell 调用的共享模态（当前仅"宿主服务"列表）。
//  - RequestHostServicesModal：任意页发起"查看宿主服务"（走 ops 队列）。
//  - HostServiceNames：服务宿主徽标 tooltip 的宿主服务名查询（走 ops 队列缓存）。
//  - 全局热键（F4#10）：Ctrl+Alt+M 呼出/隐藏主窗，main.cpp 在窗口创建后绑定。
#include "app/AppContext.h"
#include <windows.h>

namespace stm {
namespace ui3 {

void RegisterGcPages(AppContext& ctx);
void DrawGcModals(AppContext& ctx);
void RequestHostServicesModal(uint32_t pid, const std::wstring& procName);

// 服务宿主徽标 tooltip 数据源：按 pid 请求（jobs）并缓存宿主服务列表。
void EnsureHostServices(uint32_t pid);
// 返回 tooltip 文案：服务名列表 / "查询中…" / 失败原因（诚实，不伪造）。
std::wstring HostServiceNamesText(uint32_t pid);

// ---- 全局热键 Ctrl+Alt+M（cfg hotkeyEnabled，默认关） ----------------------
// main.cpp 在主窗创建后调用 Bind；SetEnabled 由设置开关（工具条）驱动，
// 失败（热键被占用等）通过 *err 如实报告。
constexpr int kGcHotkeyId = 0xB34D;  // 应用本地 id；RegisterHotKey 文档范围 0x0000-0xBFFF
                                     // （V15-P2-3：0x47434D 超范围，已改）
void GcHotkeyBindWindow(HWND hwnd);
void GcHotkeyUnbindWindow();
bool GcHotkeySetEnabled(bool enabled, std::wstring* err);

}  // namespace ui3
}  // namespace stm

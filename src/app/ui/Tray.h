#pragma once
// 经 Shell_NotifyIconW 的通知区域（托盘）图标，回调消息 WM_APP+1。
// 安装进 MainWindow 的自定义消息钩子；Remove() 必须在宿主窗口
// 销毁前运行，托盘里才不会残留失效图标。
#include <windows.h>
#include <functional>
#include <string>

namespace stm {
namespace ui {

class Tray {
public:
    // 应用回调，全部由消息钩子在 UI 线程上调用。
    std::function<void()> onToggleWindow;     // 左键：显示/隐藏主窗口
    std::function<void()> onShowWindow;       // 菜单：主窗口置前
    std::function<void()> onRestartElevated;  // 仅在未提权时添加的菜单项
    std::function<void()> onExit;             // 菜单：退出应用

    bool Create(HWND owner, bool elevated);
    void Remove();  // 幂等
    void SetTip(const std::wstring& tip);
    // 第 3 阶段告警：Shell_NotifyIconW 气泡（NIF_INFO）。图标未安装时
    // 为空操作（如 --smoke 下）。
    void ShowBalloon(const std::wstring& title, const std::wstring& text);

    // 接入 MainWindow 的 onMessage 钩子。对本类消费的消息（回调消息、
    // TaskbarCreated）置 *handled=true；调用方随后必须返回
    // 结果并跳过 DefWindowProc。
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool* handled);

private:
    void AddIcon();
    void ShowMenu(HWND hwnd);

    HWND owner_ = nullptr;
    UINT callbackMsg_ = 0;        // WM_APP+1（回调消息）
    UINT taskbarCreatedMsg_ = 0;  // explorer 重启：托盘被重建
    bool added_ = false;
    bool elevated_ = false;
    std::wstring tip_;
};

}  // namespace ui
}  // namespace stm

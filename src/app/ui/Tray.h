#pragma once
// Notification-area (tray) icon via Shell_NotifyIconW, callback message WM_APP+1.
// Installed into MainWindow's custom-message hook; Remove() must run before the
// host window is destroyed so no stale icon is left in the tray.
#include <windows.h>
#include <functional>
#include <string>

namespace stm {
namespace ui {

class Tray {
public:
    // App callbacks, all invoked on the UI thread from the message hook.
    std::function<void()> onToggleWindow;     // left click: show/hide main window
    std::function<void()> onShowWindow;       // menu: bring main window to front
    std::function<void()> onRestartElevated;  // menu item only added when not elevated
    std::function<void()> onExit;             // menu: exit the app

    bool Create(HWND owner, bool elevated);
    void Remove();  // idempotent
    void SetTip(const std::wstring& tip);
    // Phase-3 alerts: Shell_NotifyIconW balloon (NIF_INFO). No-op while the
    // icon is not installed (e.g. under --smoke).
    void ShowBalloon(const std::wstring& title, const std::wstring& text);

    // Plug into MainWindow's onMessage hook. Sets *handled=true for messages this
    // class consumes (callback message, TaskbarCreated); caller must then return
    // the result and skip DefWindowProc.
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool* handled);

private:
    void AddIcon();
    void ShowMenu(HWND hwnd);

    HWND owner_ = nullptr;
    UINT callbackMsg_ = 0;        // WM_APP+1
    UINT taskbarCreatedMsg_ = 0;  // explorer restart: tray re-created
    bool added_ = false;
    bool elevated_ = false;
    std::wstring tip_;
};

}  // namespace ui
}  // namespace stm

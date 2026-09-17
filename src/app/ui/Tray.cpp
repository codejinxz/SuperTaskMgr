#include "app/ui/Tray.h"
#include <shellapi.h>

namespace stm {
namespace ui {

namespace {
constexpr UINT kCallbackMsg = WM_APP + 1;  // arch: tray callback message
constexpr UINT kIconId = 1;                // single-icon app
constexpr int kMenuShow = 1;
constexpr int kMenuElevate = 2;
constexpr int kMenuExit = 3;
}  // namespace

bool Tray::Create(HWND owner, bool elevated) {
    owner_ = owner;
    elevated_ = elevated;
    callbackMsg_ = kCallbackMsg;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    AddIcon();
    return added_;
}

void Tray::AddIcon() {
    if (!owner_ || added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = callbackMsg_;
    // No embedded resource yet; the default application icon is acceptable
    // (taskbar grouping icon likewise, documented limitation).
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    tip_ = L"超级任务管理器";
    wcscpy_s(nid.szTip, tip_.c_str());
    if (Shell_NotifyIconW(NIM_ADD, &nid) != FALSE) added_ = true;
}

void Tray::Remove() {
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void Tray::SetTip(const std::wstring& tip) {
    tip_ = tip;
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

LRESULT Tray::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool* handled) {
    (void)wParam;
    *handled = false;
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        // Explorer restarted and rebuilt the tray; re-register the icon.
        added_ = false;
        AddIcon();
        *handled = true;
        return 0;
    }
    if (msg != callbackMsg_ || !added_) return 0;
    *handled = true;
    switch (LOWORD(lParam)) {  // legacy icon version: mouse message in lParam low word
        case WM_LBUTTONUP:
            if (onToggleWindow) onToggleWindow();
            break;
        case WM_RBUTTONUP:
            ShowMenu(hwnd);
            break;
        default:
            break;
    }
    return 0;
}

void Tray::ShowMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuShow, L"显示主窗口");
    if (!elevated_) AppendMenuW(menu, MF_STRING, kMenuElevate, L"以管理员身份重启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
    // Tray menus require the window in the foreground or they do not dismiss.
    SetForegroundWindow(hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                   pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);  // let the menu close reliably
    DestroyMenu(menu);
    switch (cmd) {
        case kMenuShow:
            if (onShowWindow) onShowWindow();
            break;
        case kMenuElevate:
            if (onRestartElevated) onRestartElevated();
            break;
        case kMenuExit:
            if (onExit) onExit();
            break;
        default:
            break;
    }
}

}  // namespace ui
}  // namespace stm

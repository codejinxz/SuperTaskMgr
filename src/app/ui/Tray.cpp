#include "app/ui/Tray.h"
#include <shellapi.h>

namespace stm {
namespace ui {

namespace {
constexpr UINT kCallbackMsg = WM_APP + 1;  // 架构约定：托盘回调消息
constexpr UINT kIconId = 1;                // 单图标应用
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
    // 应用 Logo 图标（LR_SHARED 共享句柄：进程生命周期内有效，无需 DestroyIcon）。
    nid.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                              IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON),
                                              LR_DEFAULTCOLOR | LR_SHARED));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);  // 资源缺失兜底
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

void Tray::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

LRESULT Tray::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool* handled) {
    (void)wParam;
    *handled = false;
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        // Explorer 重启并重建了托盘；重新注册图标。
        added_ = false;
        AddIcon();
        *handled = true;
        return 0;
    }
    if (msg != callbackMsg_ || !added_) return 0;
    *handled = true;
    switch (LOWORD(lParam)) {  // 旧版图标版本：鼠标消息在 lParam 低位字
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
    // 托盘菜单需要窗口处于前台，否则不会消失。
    SetForegroundWindow(hwnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                   pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);  // 让菜单可靠关闭
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

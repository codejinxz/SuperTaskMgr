#include "app/Win32Window.h"
#include "imgui_impl_win32.h"

// imgui_impl_win32: backend declares this inside its .cpp; user code declares it
// at global scope (backend convention, imgui 1.92).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace stm {

void MainWindow::EnableDpiAwareness() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return 1;

    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (self && self->cbs_.onMessage) {
        bool handled = false;
        const LRESULT r = self->cbs_.onMessage(hWnd, msg, wParam, lParam, &handled);
        if (handled) return r;
    }
    switch (msg) {
        case WM_SIZE:
            if (self && self->cbs_.onResize && wParam != SIZE_MINIMIZED) {
                self->width_ = LOWORD(lParam);
                self->height_ = HIWORD(lParam);
                self->cbs_.onResize(self->cbs_.ud, self->width_, self->height_);
            }
            return 0;
        case WM_SYSCOMMAND:
            // Block ALT-application-key menu beep noise; keep screensaver behavior default.
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            if (self && self->cbs_.quit) self->cbs_.quit->store(true);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool MainWindow::Create(HINSTANCE inst, const wchar_t* title, int w, int h, int cmdShow,
                        const WindowCallbacks& cbs) {
    cbs_ = cbs;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // D3D clears every frame
    wc.lpszClassName = L"SuperTaskMgrWnd";
    if (!RegisterClassExW(&wc)) return false;

    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    width_ = w;
    height_ = h;
    ShowWindow(hwnd_, cmdShow);
    UpdateWindow(hwnd_);
    return true;
}

void MainWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(L"SuperTaskMgrWnd", GetModuleHandleW(nullptr));
}

}  // namespace stm

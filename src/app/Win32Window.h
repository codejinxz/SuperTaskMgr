#pragma once
// Win32 top-level window hosting the D3D11 swap chain (thin shell; no business logic).
#include <windows.h>
#include <atomic>
#include <functional>

namespace stm {

struct WindowCallbacks {
    std::atomic<bool>* quit = nullptr;                   // set on WM_DESTROY (cross-thread)
    void (*onResize)(void* ud, int w, int h) = nullptr;  // WM_SIZE (w/h may be 0 while minimizing)
    void* ud = nullptr;
    // Optional custom-message hook (tray icon etc.). Runs before the built-in switch;
    // set *handled=true (and return the result) to skip the default handling.
    std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool* handled)> onMessage;
};

class MainWindow {
public:
    bool Create(HINSTANCE inst, const wchar_t* title, int w, int h, int cmdShow,
                const WindowCallbacks& cbs);
    void Destroy();

    HWND Hwnd() const { return hwnd_; }
    int Width() const { return width_; }
    int Height() const { return height_; }

    // Per-Monitor-V2 awareness; call once before creating any window.
    static void EnableDpiAwareness();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    int width_ = 0, height_ = 0;
    WindowCallbacks cbs_;
};

}  // namespace stm

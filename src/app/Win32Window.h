#pragma once
// 承载 D3D11 交换链的 Win32 顶层窗口（薄壳；无业务逻辑）。
#include <windows.h>
#include <atomic>
#include <functional>

namespace stm {

struct WindowCallbacks {
    std::atomic<bool>* quit = nullptr;                   // WM_DESTROY 时置位（跨线程）
    void (*onResize)(void* ud, int w, int h) = nullptr;  // WM_SIZE（最小化时 w/h 可能为 0）
    void* ud = nullptr;
    // 可选的自定义消息钩子（托盘图标等）。在内建 switch 之前运行；
    // 置 *handled=true（并返回结果）可跳过默认处理。
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

    // Per-Monitor-V2 感知；在创建任何窗口前调用一次。
    static void EnableDpiAwareness();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    int width_ = 0, height_ = 0;
    WindowCallbacks cbs_;
};

}  // namespace stm

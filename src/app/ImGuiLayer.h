#pragma once
// ImGui 上下文 + win32/dx11 后端 + 中文字体加载（架构第 8 节）。
#include <string>
#include <windows.h>

namespace stm {

class D3DRenderer;

class ImGuiLayer {
public:
    bool Init(HWND hwnd, D3DRenderer* renderer, float fontSizePx = 16.0f);
    void Shutdown();
    void NewFrame();
    void Render();  // DrawData -> dx11 后端

    // 找不到支持中文的字体文件时为 nullptr（UI 仍应能渲染拉丁文）。
    const std::wstring& FontFileUsed() const { return fontUsed_; }

private:
    std::wstring fontUsed_;
    HWND hwnd_ = nullptr;
};

}  // namespace stm

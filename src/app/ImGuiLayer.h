#pragma once
// ImGui context + win32/dx11 backends + CJK font loading (arch section 8).
#include <string>
#include <windows.h>

namespace stm {

class D3DRenderer;

class ImGuiLayer {
public:
    bool Init(HWND hwnd, D3DRenderer* renderer, float fontSizePx = 16.0f);
    void Shutdown();
    void NewFrame();
    void Render();  // DrawData -> dx11 backend

    // nullptr when no Chinese-capable font file was found (UI should still render Latin).
    const std::wstring& FontFileUsed() const { return fontUsed_; }

private:
    std::wstring fontUsed_;
    HWND hwnd_ = nullptr;
};

}  // namespace stm

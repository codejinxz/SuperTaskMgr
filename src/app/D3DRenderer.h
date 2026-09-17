#pragma once
// Minimal D3D11 wrapper around the swap chain back buffer (following the official
// imgui_impl_dx11 example lifecycle). UI layer only; no app logic here.
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace stm {

class D3DRenderer {
public:
    bool Init(HWND hwnd, int w, int h);
    void Shutdown();
    void Resize(int w, int h);
    void BeginFrame();          // bind back buffer + clear
    void Present();             // vsync

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

private:
    void CreateTargets();
    void ReleaseTargets();

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> mainTarget_;
};

}  // namespace stm

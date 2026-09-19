#pragma once
// 围绕交换链后备缓冲的极简 D3D11 封装（遵循官方 imgui_impl_dx11
// 示例的生命周期）。仅 UI 层；此处无应用逻辑。
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace stm {

class D3DRenderer {
public:
    bool Init(HWND hwnd, int w, int h);
    void Shutdown();
    void Resize(int w, int h);
    void BeginFrame();          // 绑定后备缓冲 + 清屏
    void Present();             // 垂直同步

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

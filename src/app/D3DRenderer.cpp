#include "app/D3DRenderer.h"
#include <wrl/client.h>

namespace stm {

bool D3DRenderer::Init(HWND hwnd, int w, int h) {
    hwnd_ = hwnd;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = static_cast<UINT>(w);
    sd.BufferDesc.Height = static_cast<UINT>(h);
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = 0;

    // No D3D11_CREATE_DEVICE_DEBUG: the debug layer is an optional OS feature and its
    // absence would silently push us onto WARP. We are not using the debug layer.
    const UINT flags = 0;
    // FL 10.0 baseline (arch: WARP fallback guaranteed by DX11 requirement set).
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                               &sd, swapChain_.GetAddressOf(),
                                               device_.GetAddressOf(), &got, context_.GetAddressOf());
    if (FAILED(hr)) {
        // Hardware path failed (RDP / broken driver): WARP software fallback.
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                           levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                           &sd, swapChain_.GetAddressOf(),
                                           device_.GetAddressOf(), &got, context_.GetAddressOf());
        if (FAILED(hr)) return false;
    }
    CreateTargets();
    return true;
}

void D3DRenderer::CreateTargets() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())))) {
        device_->CreateRenderTargetView(back.Get(), nullptr, mainTarget_.GetAddressOf());
    }
}

void D3DRenderer::ReleaseTargets() { mainTarget_.Reset(); }

void D3DRenderer::Resize(int w, int h) {
    if (!swapChain_ || w <= 0 || h <= 0) return;
    ReleaseTargets();
    swapChain_->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h),
                              DXGI_FORMAT_UNKNOWN, 0);
    CreateTargets();
}

void D3DRenderer::BeginFrame() {
    if (!mainTarget_) return;
    const float clear[4] = {0.086f, 0.090f, 0.106f, 1.0f};  // matches dark theme bg
    context_->OMSetRenderTargets(1, mainTarget_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(mainTarget_.Get(), clear);
}

void D3DRenderer::Present() {
    if (swapChain_) swapChain_->Present(1, 0);
}

void D3DRenderer::Shutdown() {
    ReleaseTargets();
    if (swapChain_) swapChain_->SetFullscreenState(FALSE, nullptr);
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

}  // namespace stm

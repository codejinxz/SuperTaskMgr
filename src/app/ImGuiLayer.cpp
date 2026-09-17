#include "app/ImGuiLayer.h"
#include "app/D3DRenderer.h"
#include "core/Log.h"
#include "core/Str.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

namespace stm {

namespace {
// msyh.ttc ships with Win10/11 (zh-capable); fall back to other CJK system fonts.
// System fonts are NOT redistributable; we only load from disk at runtime.
const wchar_t* const kFontCandidates[] = {
    L"C:\\Windows\\Fonts\\msyh.ttc",
    L"C:\\Windows\\Fonts\\msyhl.ttc",
    L"C:\\Windows\\Fonts\\simhei.ttf",
    L"C:\\Windows\\Fonts\\Deng.ttf",
};
}  // namespace

bool ImGuiLayer::Init(HWND hwnd, D3DRenderer* renderer, float fontSizePx) {
    hwnd_ = hwnd;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // we persist layout in our own config

    // Font: dynamic atlas in 1.92 loads glyphs on demand; FontNo=0 selects the first face
    // of msyh.ttc ("Microsoft YaHei UI"). Sizes are DIP-ish; DPI rescaling is per-viewport.
    ImFontConfig cfg;
    cfg.FontNo = 0;
    bool fontOk = false;
    for (const wchar_t* path : kFontCandidates) {
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            // ImGui expects narrow (UTF-8) paths.
            const std::string pathU8 = WideToUtf8(path);
            if (io.Fonts->AddFontFromFileTTF(pathU8.c_str(), fontSizePx, &cfg)) {
                fontUsed_ = path;
                fontOk = true;
                break;
            }
        }
    }
    if (!fontOk) {
        STM_LOG_WARN("ui", L"未找到系统中文字体，界面将退化为默认字体");
        io.Fonts->AddFontDefault();
    }

    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    if (!ImGui_ImplDX11_Init(renderer->Device(), renderer->Context())) return false;
    return true;
}

void ImGuiLayer::Shutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::NewFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::Render() {
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace stm

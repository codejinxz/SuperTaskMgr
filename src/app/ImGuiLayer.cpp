#include "app/ImGuiLayer.h"
#include "app/D3DRenderer.h"
#include "core/Log.h"
#include "core/Str.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "implot.h"

namespace stm {

namespace {
// msyh.ttc 随 Win10/11 提供（支持中文）；必要时回退到其他 CJK 系统字体。
// 系统字体不可再分发；我们只在运行时从磁盘加载。
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
    ImPlot::CreateContext();  // 性能页图表用
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // 布局持久化保存在我们自己的配置里

    // 字体：1.92 的动态图集按需加载字形；FontNo=0 选择 msyh.ttc 的
    // 第一个字面（"Microsoft YaHei UI"）。尺寸接近 DIP；DPI 缩放按视口进行。
    ImFontConfig cfg;
    cfg.FontNo = 0;
    bool fontOk = false;
    for (const wchar_t* path : kFontCandidates) {
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            // ImGui 期望窄字符（UTF-8）路径。
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

    if (!ImGui_ImplWin32_Init(hwnd)) {
        // V7-P1-4：回滚 Init 已创建的资源（ImPlot + ImGui 上下文、
        // 字体图集），而不是在后端失败时泄漏它们。
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(renderer->Device(), renderer->Context())) {
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        return false;
    }
    return true;
}

void ImGuiLayer::Shutdown() {
    ImPlot::DestroyContext();
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

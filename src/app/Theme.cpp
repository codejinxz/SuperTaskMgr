#include "app/Theme.h"
#include "imgui.h"

namespace stm {

void Theme::Apply() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.WindowBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.090f, 0.106f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.106f, 0.110f, 0.129f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.118f, 0.122f, 0.145f, 0.98f);
    c[ImGuiCol_FrameBg] = ImVec4(0.157f, 0.165f, 0.196f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.196f, 0.208f, 0.247f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.235f, 0.247f, 0.294f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.071f, 0.075f, 0.086f, 1.0f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.106f, 0.110f, 0.129f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.227f, 0.431f, 0.647f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.227f, 0.431f, 0.647f, 0.80f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.278f, 0.474f, 0.702f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.106f, 0.110f, 0.129f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.227f, 0.431f, 0.647f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.137f, 0.145f, 0.173f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
    c[ImGuiCol_CheckMark] = ImVec4(0.400f, 0.620f, 0.878f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.400f, 0.620f, 0.878f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.549f, 0.714f, 0.925f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.090f, 0.094f, 0.110f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.502f, 0.522f, 0.580f, 1.0f);
}

}  // namespace stm

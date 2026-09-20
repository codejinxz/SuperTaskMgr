#pragma once
// ============================================================================
//  U1（2026-09）：网络页纵向区块之间的可拖拽分隔条（header-only，仅依赖
//  ImGui 公共 API 与 PageLayout.h 纯函数；分配/钳制数学由 stm_selftest
//  ui_m2_test.cpp 无 GUI 覆盖，本文件只是薄手柄）。
//
//  交互契约（Pages3.cpp NetworkPage 在 适配器卡区|实时监视段 之间使用）：
//   - 常态：kNetSplitterThickness（6px，经 P1④ 布局缩放）高的整宽交互带，
//     中央画 2px 分隔线（ImGuiCol_Separator）—— 视觉上取代原 Separator；
//   - 悬停：整带高亮（ImGuiCol_FrameBgHovered）+ ResizeNS 鼠标光标；
//   - 拖动：*value 实时 += MouseDelta.y（拖动预览），经 ClampRegionH 钳到
//     [minH, maxH]；值只改内存，落盘节流由调用方负责；
//   - 松手：返回 true 一次 —— 调用方此刻才写 cfg。
//
//  状态机（由 ImGui 的 item 激活态承载，无额外状态存储）：
//    Idle --hover--> Hovered --LMB down--> Dragging --release--> Idle
//  InvisibleButton 激活期间鼠标移出交互带仍保持 active（ImGui 按钮语义），
//  拖动不因手滑出带而中断；IsItemDeactivated 在松手帧恰好为 true 一次。
// ============================================================================
#include "app/ui3/PageLayout.h"

#include "imgui.h"

namespace stm {
namespace ui3 {

// 纵向分隔条：调整其**上方**区块的高度（向下拖 = 上方区块变高）。
// 返回 true = 本次调用恰逢松手帧（调用方把 *value 写 cfg）。
inline bool RegionSplitterY(const char* id, float* value, float minH,
                            float maxH) {
    const float thickness = Scaled(kNetSplitterThickness);
    float w = ImGui::GetContentRegionAvail().x;
    if (!(w > 8.0f)) w = 8.0f;  // 离屏 smoke 顺序绘制可能耗尽宽度：保正值
    ImGui::InvisibleButton(id, ImVec2(w, thickness));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();  // true = 拖动中（按住未放）
    if (active) {
        *value = ClampRegionH(*value + ImGui::GetIO().MouseDelta.y, minH, maxH);
    }
    // 可见反馈：悬停/拖动整带高亮；中央 2px 分隔线常态可见（拖动中加亮）。
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pMin = ImGui::GetItemRectMin();
    const ImVec2 pMax = ImGui::GetItemRectMax();
    const float cy = (pMin.y + pMax.y) * 0.5f;
    if (hovered || active) {
        dl->AddRectFilled(pMin, pMax, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
    }
    dl->AddRectFilled(ImVec2(pMin.x, cy - 1.0f), ImVec2(pMax.x, cy + 1.0f),
                      ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive
                                                : ImGuiCol_Separator));
    if (hovered || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);  // 上下拖拽光标
    }
    return ImGui::IsItemDeactivated();  // 松手帧（含未拖动的纯点击，值未变）
}

}  // namespace ui3
}  // namespace stm

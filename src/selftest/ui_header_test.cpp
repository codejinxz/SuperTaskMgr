// R-Fix Bug1 布局数学回归（2026-09）：ui::LayoutHeaderRight /
// ui::FlowSegmentFits（app/ui/HeaderLayout.h，纯函数 + 仅头文件 + 不依赖 ImGui）。
//
// 缺陷背景：外壳工具栏过去用硬编码的 ImGui::SameLine(GetWindowWidth() - NNN)
// 摆放右侧，无视条目的真实宽度——窄窗口下条目相互重叠并
// 盖住「?」关于按钮（Bug3）。重构后的外壳用这里测试的纯函数
// 摆放工具栏「?」与状态栏右侧段
// right segment [全局热键][权限徽标][帧耗时] with the pure functions tested here.
// 契约：任意窗口宽度 >= 600 时摆位零重叠、绝不遮盖左侧流、
// 优先隐藏低重要性条目，窗口 >= 900px 时全部显示。
//
#include "selftest/TestFramework.h"
#include "app/ui/HeaderLayout.h"

using stm::ui::FlowSegmentFits;
using stm::ui::HeaderItem;
using stm::ui::HeaderPlacement;
using stm::ui::LayoutHeaderRight;

namespace {

// 真实外壳测量（默认样式，中文字体）：工具栏右侧 = 方形「?」按钮；
// 状态栏右侧 = 热键复选框 / 徽标 / 帧耗时文本。
constexpr float kSpacing = 8.0f;
constexpr float kToolbarLeftEnd = 700.0f;
constexpr float kStatusLeftEnd = 430.0f;

// 完整不变式清扫：不遮左侧流、无两两重叠、优先级单调
//（可见条目的每个更高优先级——更小数字——条目
// 也必须可见）、priority-0 恒存。
bool CheckInvariants(float w, float leftEnd, const HeaderItem* items,
                     const HeaderPlacement* out, int count, std::wstring* err) {
    for (int i = 0; i < count; ++i) {
        if (!out[i].visible) continue;
        if (out[i].x < leftEnd - 0.001f) {
            *err = L"项覆盖左侧流程";
            return false;
        }
        for (int j = 0; j < count; ++j) {
            if (i == j) continue;
            if (items[j].priority < items[i].priority && !out[j].visible) {
                *err = L"优先级单调性破坏：项可见但更高优先级项被隐藏";
                return false;
            }
        }
        for (int j = i + 1; j < count; ++j) {
            if (!out[j].visible) continue;
            if (out[i].x + items[i].width + kSpacing > out[j].x + 0.001f) {
                *err = L"两项重叠";
                return false;
            }
        }
    }
    (void)w;
    return true;
}

}  // namespace

// 工具栏右侧只有「?」关于按钮：每个宽度 >= 600 下它都必须
// 保持可见且绝不遮盖左侧流（Bug1/Bug3 契约）；
// 宽度物理上允许时保持在内容区内
//（低于该宽度时，纯函数的退化钳制把它钉在 leftFlowEndX，
// 真实外壳此时还会把次要动作折叠进「⋮」）。
STM_TEST(header_toolbar_about_any_width) {
    const HeaderItem items[1] = {{26.0f, 0}};
    for (float leftEnd : {560.0f, 700.0f}) {  // 折叠进「⋮」与完整左侧流两种
        for (int w = 600; w <= 2560; w += 40) {
            HeaderPlacement place[1] = {};
            LayoutHeaderRight(static_cast<float>(w), leftEnd, kSpacing, items, 1, place);
            if (!place[0].visible) {
                *err = L"「?」(priority 0) 永不隐藏";
                return false;
            }
            if (place[0].x < leftEnd - 0.001f) {
                *err = L"「?」压住左侧动作区（宽 " + std::to_wstring(w) + L"）";
                return false;
            }
            const bool clamped = place[0].x == leftEnd;  // 退化兜底
            if (!clamped &&
                place[0].x + 26.0f > static_cast<float>(w) + 0.001f) {
                *err = L"「?」越界（宽 " + std::to_wstring(w) + L"）";
                return false;
            }
            if (!clamped && place[0].x + 26.0f <= static_cast<float>(w) &&
                place[0].x < static_cast<float>(w) - 26.0f - 0.001f) {
                *err = L"「?」未右对齐（宽 " + std::to_wstring(w) + L"）";
                return false;
            }
        }
    }
    return true;
}

// 状态栏右侧段 [热键(pri 2)][徽标(pri 1)][帧耗时(pri 0)]：
// 任意宽度零重叠、>=900 全部可见、更低宽度热键先隐藏。
STM_TEST(header_statusbar_right_group) {
    const HeaderItem items[3] = {{190.0f, 2}, {66.0f, 1}, {72.0f, 0}};
    for (int w = 600; w <= 2560; w += 37) {
        HeaderPlacement place[3] = {};
        const int vis = LayoutHeaderRight(static_cast<float>(w), kStatusLeftEnd, kSpacing,
                                          items, 3, place);
        if (!CheckInvariants(static_cast<float>(w), kStatusLeftEnd, items, place, 3, err)) {
            *err = L"宽 " + std::to_wstring(w) + L"：" + *err;
            return false;
        }
        if (!place[2].visible) {
            *err = L"帧耗时 (priority 0) 永不隐藏";
            return false;
        }
        if (w >= 900 && vis != 3) {
            *err = L"宽 >=900 时状态栏三项应全可见（宽 " + std::to_wstring(w) + L"）";
            return false;
        }
        if (w < 774 && place[0].visible) {
            // 430 左侧流 + 344 右侧组：低于 774px 时热键复选框
            //（重要性最低）必须让位而不是与任何东西重叠。
            *err = L"窄窗应优先隐藏「全局热键」（宽 " + std::to_wstring(w) + L"）";
            return false;
        }
    }
    return true;
}

// 模糊：宽度 x 左侧流末端 x 宽度集合——所有位置不变式都必须成立，
// 且 priority-0 条目绝不隐藏。
STM_TEST(header_layout_fuzz_invariants) {
    const float wsets[][3] = {
        {190.0f, 66.0f, 72.0f}, {120.0f, 30.0f, 20.0f}, {260.0f, 90.0f, 40.0f},
        {40.0f, 200.0f, 15.0f}, {0.0f, 0.0f, 0.0f},     {500.0f, 10.0f, 10.0f},
    };
    for (int w = 200; w <= 2000; w += 53) {
        for (float le : {0.0f, 100.0f, 300.0f, 430.0f, 700.0f}) {
            for (const auto& ws : wsets) {
                const HeaderItem it[3] = {{ws[0], 2}, {ws[1], 1}, {ws[2], 0}};
                HeaderPlacement place[3] = {};
                LayoutHeaderRight(static_cast<float>(w), le, kSpacing, it, 3, place);
                if (!CheckInvariants(static_cast<float>(w), le, it, place, 3, err)) {
                    *err = L"宽 " + std::to_wstring(w) + L" leftEnd " +
                           std::to_wstring((int)le) + L"：" + *err;
                    return false;
                }
                if (!place[2].visible) {
                    *err = L"模糊：priority 0 被隐藏";
                    return false;
                }
            }
        }
    }
    return true;
}

// 退化用例：左侧流已越过右缘——每个更低优先级条目都被隐藏，
// priority-0 条目被钳制到左侧流末端
// 而不是与它重叠。
STM_TEST(header_layout_degenerate_clamp) {
    const HeaderItem it[3] = {{190.0f, 2}, {66.0f, 1}, {26.0f, 0}};
    HeaderPlacement place[3] = {};
    const int vis = LayoutHeaderRight(400.0f, 500.0f, kSpacing, it, 3, place);
    if (vis != 1 || !place[2].visible || place[2].x != 500.0f) {
        *err = L"退化布局：应只保留 priority 0 项并钳制在 leftFlowEndX";
        return false;
    }
    return true;
}

// FlowSegmentFits：状态栏左侧段溢出护栏。
STM_TEST(header_flow_segment_fits) {
    if (!FlowSegmentFits(0.0f, 100.0f, 100.0f)) {
        *err = L"恰好放下的段应通过";
        return false;
    }
    if (FlowSegmentFits(0.0f, 100.0f, 100.5f)) {
        *err = L"超出的段应拒绝";
        return false;
    }
    if (FlowSegmentFits(40.0f, 100.0f, 61.0f)) {
        *err = L"游标推进后放不下的段应拒绝";
        return false;
    }
    return true;
}

#pragma once
// ============================================================================
//  M2 页面布局防抖（2026-09）：网络页 / 传感器页「顶栏位置固定」的
//  纯函数契约。仅头文件、不依赖 ImGui 渲染状态（调用方注入行高等实测值），
//  与 StatusLayout.h（L1 状态栏定宽槽位）同一思路：
//
//  根因（用户报告「页面顶部控制行/区块标题随内容上下移动」）：
//   - 网络页适配器卡区（AutoResizeY 卡片 + 可展开「更多适配器」）高度随
//     适配器数量/展开状态变化，把下方「实时监视」「连接表」上下顶动；
//   - 传感器页分组区（各组 AutoResizeY）高度随读数条目出现/消失、LHM 行
//     合入、组显隐切换而变化 —— 页面总高变化使父级滚动参与布局，
//     顶栏随滚动偏移上下移动。
//
//  修复契约（本头文件 + Pages3.cpp 调用方共同遵守，selftest
//  src/selftest/ui_m2_test.cpp 断言）：
//   1. 两页的内容区各包进一个**定高滚动区**（BeginChild 固定高度，内部
//      滚动）。定高区高度只由（调用方实测的可用高度、编译期常量上限/
//      下限）决定 —— 内容增减绝不是高度的输入，故区外任何元素的 y 坐标
//      逐帧恒定（窗口缩放是唯一合法的布局变化来源）。
//   2. 加载态/错误态/说明行等出现即改变高度的行，一律画进定高区**内部**。
//   3. 头部行内可能出现的提示文字必须 SameLine（行数恒定）。
//   4. 高度/可视行数/溢出判定全部经本头文件的纯函数计算，便于
//      stm_selftest 无 GUI 覆盖。
// ============================================================================

#include <cstddef>

#include "app/ui3/PageHelpers.h"  // SensorGroup（分组显隐集合的元素类型）

namespace stm {
namespace ui3 {

// ---- P1④：运行时布局缩放（「一键优化布局」）--------------------------------
// 需求：按当前分辨率/PPI 一键缩放布局（区域定高、表格行高与 FramePadding、
// 图表块高等），免去手动拖拽。契约：
//   - 缩放槽是进程级单槽（函数内 static，UI 单线程访问）；默认 1.0。
//   - Scaled(base) = base * 缩放；缩放=1.0f 时与原编译期常量**逐位一致**
//    （浮点乘 1.0 是恒等），selftest 有回归断言。
//   - 缩放策略（LayoutScaleFromEnv，纯函数）：
//       分辨率因子（按主视口工作区高度）：≥2000px（4K 级）→ 1.30；
//       ≥1300px（1440p 级）→ 1.15；其余（1080p 及以下）→ 1.0。
//       DPI 因子 = dpi / 96（96 = 100%；144（150% 缩放）→ 1.5）。
//       取两者**最大值**而非乘积：Windows 显示缩放通常同时推高分辨率与
//       DPI，取 max 已足够补偿；乘积会双重放大（4K+150% → 2.6）。
//       结果钳制到 [1.0, 2.0]，非法输入（0/负/NaN）按缺失处理回退 1.0。
inline float& LayoutScaleRef() {
    static float s = 1.0f;
    return s;
}

inline float LayoutScale() { return LayoutScaleRef(); }

// 设置运行时缩放；返回实际生效值（钳制 [1.0, 2.0]，非法输入回 1.0）。
inline float SetLayoutScale(float s) {
    if (!(s > 0.0f)) s = 1.0f;  // 0/负/NaN 防御
    if (s < 1.0f) s = 1.0f;
    if (s > 2.0f) s = 2.0f;
    LayoutScaleRef() = s;
    return s;
}

// 基准值 × 运行时缩放。scale=1 时与原常量逐位一致（回归断言钉死）。
inline float Scaled(float base) { return base * LayoutScaleRef(); }

inline float LayoutScaleFromEnv(float workH, float dpi) {
    // 分辨率因子。
    float res = 1.0f;
    if (workH > 0.0f) {
        if (workH >= 2000.0f) res = 1.30f;
        else if (workH >= 1300.0f) res = 1.15f;
    }
    // DPI 因子（96 = 100%）；缺失/非法按 96 处理。
    float dpiF = 1.0f;
    if (dpi > 0.0f) dpiF = dpi / 96.0f;
    float s = res > dpiF ? res : dpiF;
    if (!(s > 0.0f)) s = 1.0f;  // NaN 防御
    if (s < 1.0f) s = 1.0f;
    if (s > 2.0f) s = 2.0f;
    return s;
}

// ---- P1⑤：定高区内容不足时收缩到内容高 ------------------------------------
// 用户报告「传感器页底部有一个白色块」：##sensorgroups 定高 Child（上限
// 600px）在分组内容不足时露出大片空白底。收缩规则（纯）：
//   - contentH 合法且小于 region → 返回 contentH（空白消失；本区位于页尾，
//     其下无内容，收缩不影响任何区外元素的 y 坐标 —— 顶栏固定契约不破）；
//   - contentH ≥ region（或 contentH 非法/未知 ≤0/NaN，如首帧未测得）→
//     返回 region（内容超出时维持定高滚动，M2 语义不变）。
inline float FixedRegionShrinkToContent(float regionH, float contentH) {
    if (!(regionH > 0.0f)) return contentH > 0.0f ? contentH : 0.0f;
    if (!(contentH > 0.0f)) return regionH;  // 未知内容：维持定高
    return contentH < regionH ? contentH : regionH;
}

// ---- P-A（2026-09）：页头固定的「精确填满」滚动区高度 -----------------------
// 用户报告：①性能/网络页下滑后页面顶部控制行随内容滚走；②传感器页底部
// 有一块随主题变黑/变白的空白。根因与修复契约（三页共用）：
//   - 性能/网络页内容总高超视口 → 此前父级（##pagearea）整体滚动，页头
//     随滚动偏移离开视野；传感器页 ##sensorgroups 的 [240,600] 钳制在
//     页面可用高 > 606px 时留下「页尾空隙」（ChildBg 随主题变黑/变白，
//     即用户所指空白块；P1⑤ 的内容收缩同样收缩不掉这段空隙）。
//   - 修复：页头（恒定行数）绘制完成后，其余内容包进高度 = 本函数结果
//     的滚动 Child。Child 在页头**之下**恰好填满页面剩余高度 → 页头 y
//     恒定（缩放窗口是唯一合法变化）；内容增减只改变区内滚动量。
//   - 扣减 ItemSpacing.y：EndChild 后 ImGui 按子高 + 条目间距推进父级
//     光标，不扣减则父级恰好超高一个间距 → 出现 ~6px 微滚动条（页头
//     随之抖动）；扣减后恰好贴合，父级永不出现滚动条。
//   - 入参只有实测可用高度与条目间距（运行时值，已含布局缩放）——
//     内容高度绝不是输入。退化：availY ≤ 0/NaN（离屏 smoke 顺序绘制
//     耗尽空间）→ 1px；间距 ≥ 可用高 → 放弃扣减保正。
//   - 取代口径：传感器页不再走 SensorGroupsRegionHeight*（[240,600]
//     钳制）与 FixedRegionShrinkToContent（内容不足收缩）——纯函数与其
//     selftest 契约保留不动；内容不足时 Child 空底与本页背景同为
//     ChildBg（含壁纸模式的透明推送），不再形成可辨区块。
inline float FillScrollRegionHeight(float availY, float itemSpacingY) {
    if (!(availY > 0.0f)) return 1.0f;  // 0/负/NaN 防御（离屏绘制）
    float h = availY;
    if (itemSpacingY > 0.0f) h -= itemSpacingY;
    if (!(h > 0.0f)) h = availY;  // 间距 ≥ 可用高：放弃扣减，保正
    return h;
}

// ---- 网络页：适配器卡区 ----------------------------------------------------

// 适配器定高卡区的默认高度（约 200px；窗口极矮时退化为可用高度，
// 见 AdapterRegionHeight）。
inline constexpr float kAdapterRegionHeight = 200.0f;

// 单张适配器卡片的固定行数：1 标题行（名称/类型/状态/速度/复制 IP）
// + 6 明细行（IPv4、IPv6、网关、DNS、MAC、DHCP —— 与 DrawAdapterCard 的
// 基线明细一致；超出基线的部分是 extraAddressRows）。
inline constexpr int kAdapterCardFixedRows = 7;

// 卡片子窗口上下内边距的保守行数换算（Border + FramePadding.y*2）。
inline constexpr int kAdapterCardPadRows = 2;

// 适配器卡区高度（纯）：正常窗口恒为 kAdapterRegionHeight；仅当页面
// 可用高度更小（极矮窗口 / 离屏 smoke 顺序绘制耗尽空间）时退化为
// 正的可用值。入参只有可用高度 —— 卡片数量/展开状态绝非输入。
inline float AdapterRegionHeight(float pageAvailY) {
    float h = pageAvailY;
    if (!(h > 0.0f)) h = 1.0f;  // 0/负/NaN 防御（离屏绘制可能耗尽空间）
    if (h > kAdapterRegionHeight) h = kAdapterRegionHeight;
    return h;
}

// 单卡高度估算（纯）：rowH = 单行实测高（GetTextLineHeightWithSpacing），
// extraAddressRows = 超出 6 行基线明细的附加地址行（多 IPv6 地址）。
// 仅用于行内溢出提示（估算），不参与真实布局。
inline float AdapterCardHeightEstimate(float rowH, int extraAddressRows) {
    if (!(rowH > 0.0f)) rowH = 0.0f;  // 0/负/NaN 防御
    const int extra = extraAddressRows > 0 ? extraAddressRows : 0;
    return rowH * static_cast<float>(kAdapterCardFixedRows + kAdapterCardPadRows +
                                     extra);
}

// 卡片总高估算（纯）：Σ 单卡 + 卡片间条目间距（按 1 行/张估算，
// n 张共 n-1 个间隙）。
inline float AdapterCardsTotalHeight(float rowH, const int* extraAddressRows,
                                     int count) {
    if (count <= 0) return 0.0f;
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        total += AdapterCardHeightEstimate(
            rowH, extraAddressRows != nullptr ? extraAddressRows[i] : 0);
    }
    return total + rowH * static_cast<float>(count - 1);
}

// 定高区域内可完整可见的行数（纯，floor 语义；非法输入按 0 行）。
inline int VisibleRowsInFixedHeight(float regionH, float rowH) {
    if (!(regionH > 0.0f) || !(rowH > 0.0f)) return 0;
    const int n = static_cast<int>(regionH / rowH);
    return n > 0 ? n : 0;
}

// 卡片是否溢出定高区（纯）：超出时头部出现 SameLine 的「可滚动」提示。
// 提示出现/消失不改变行数 —— 行内文字绝不影响顶栏高度契约。
inline bool AdapterCardsOverflow(float regionH, float cardsTotalH) {
    return cardsTotalH > regionH;
}

// ---- 传感器页：分组展示定高区 ----------------------------------------------

// 分组展示区高度上限：约 600px —— 大窗口下页面总高 = 顶栏 + 600，
// 仍有富余，父级永不出现滚动条。
inline constexpr float kSensorGroupsMaxHeight = 600.0f;

// 分组展示区下限：极矮窗口按可用高度收缩到本值为止；再矮则接受父级
// 滚动条重现（降级：定高仍恒定，顶栏只在用户主动滚动时离开视野）。
inline constexpr float kSensorGroupsMinHeight = 240.0f;

// 收缩时给父级底部保留的余量（防四舍五入诱发的父级滚动条）。
inline constexpr float kSensorRegionBottomMargin = 6.0f;

// 分组展示区高度（纯）：实现选「页高减顶栏」（availBelowChrome 由调用方
// 在顶栏绘制完成后用 GetContentRegionAvail().y 测取 —— 顶栏行数恒定，
// 故该值只随窗口缩放变化），减去常量余量后钳制到 [240, 600]。
// 内容（读数条目、LHM 行、组显隐、加载/说明行）绝不是输入。
inline float SensorGroupsRegionHeight(float availBelowChrome) {
    float h = availBelowChrome - kSensorRegionBottomMargin;
    if (!(h > 0.0f)) h = kSensorGroupsMinHeight;  // 0/负/NaN 防御
    if (h > kSensorGroupsMaxHeight) h = kSensorGroupsMaxHeight;
    if (h < kSensorGroupsMinHeight) h = kSensorGroupsMinHeight;
    return h;
}

// M2 常量的缩放版（P1④；调用方用本函数替代直接使用常量）：
// 先按缩放折算可用高度，走同一纯函数（退化/钳制语义不变），再还原。
// scale=1 时与原函数逐位一致（回归断言钉死）。
inline float AdapterRegionHeightScaled(float pageAvailY, float scale) {
    if (!(scale > 0.0f)) return AdapterRegionHeight(pageAvailY);
    return AdapterRegionHeight(pageAvailY / scale) * scale;
}

inline float SensorGroupsRegionHeightScaled(float availBelowChrome, float scale) {
    if (!(scale > 0.0f)) return SensorGroupsRegionHeight(availBelowChrome);
    float h = SensorGroupsRegionHeight(availBelowChrome / scale) * scale;
    // 缩放 > 1 时下限（240*scale）可能越过可用高度：钳回可用高度 —— 定高区
    // 绝不放大父级。scale == 1 时不钳制，与原函数逐位一致（含「极矮窗口
    // 接受父级滚动条」的已声明降级，ui_m2_test 契约）。
    if (scale > 1.0f && availBelowChrome > 0.0f && h > availBelowChrome) {
        h = availBelowChrome;
    }
    if (!(h > 0.0f)) h = SensorGroupsRegionHeight(availBelowChrome);
    return h;
}

// ---- 传感器页：显隐状态 → 分组可见集合（纯）--------------------------------

// 分组可见集合：bit i = SensorGroup 枚举序 i 是否可见。
using SensorGroupMask = unsigned;

static_assert(sizeof(SensorGroupMask) * 8 >=
                  static_cast<int>(SensorGroup::Count),
              "SensorGroupMask 位宽必须容纳全部分组");

// 布尔可见性数组（按 SensorGroup 枚举序）→ 位掩码。count 超出
// SensorGroup::Count 的部分忽略，负数按 0 处理。
inline SensorGroupMask SensorGroupMaskFromFlags(const bool* visible, int count) {
    SensorGroupMask m = 0;
    const int limit = static_cast<int>(SensorGroup::Count);
    const int n = count < limit ? (count < 0 ? 0 : count) : limit;
    for (int i = 0; i < n; ++i) {
        if (visible[i]) m |= (1u << i);
    }
    return m;
}

// 分组是否在可见集合中（枚举越界一律 false）。
inline bool SensorGroupMaskHas(SensorGroupMask m, SensorGroup g) {
    const int i = static_cast<int>(g);
    if (i < 0 || i >= static_cast<int>(SensorGroup::Count)) return false;
    return (m & (1u << i)) != 0;
}

// 可见分组个数（集合基数，供诊断/提示使用）。
inline int SensorGroupMaskCount(SensorGroupMask m) {
    int n = 0;
    for (int i = 0; i < static_cast<int>(SensorGroup::Count); ++i) {
        if ((m & (1u << i)) != 0) ++n;
    }
    return n;
}

}  // namespace ui3
}  // namespace stm

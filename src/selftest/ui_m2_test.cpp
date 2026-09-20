// M2 布局防抖回归（2026-09）：网络页 / 传感器页「顶栏位置固定」的纯函数契约。
// app/ui3/PageLayout.h（仅头文件、不依赖 ImGui 渲染状态 —— 与 StatusLayout.h
// 的 L1 思路一致：布局高度/坐标只用常量与入参派生的定值，内容增减只改变
// 定高区域内部的滚动量）。
//
// 用户报告的根因：网络页适配器卡区与传感器页分组区都是 AutoResizeY 的
// 可变高度内容，增减（适配器卡片、温度条目、LHM 行合入、组显隐切换）会
// 顶动下方区块与页头。修复契约（这里逐条断言）：
//  - 定高区高度只依赖（可用高度、编译期常量）—— 内容绝不是输入；
//  - 钳制边界 [kSensorGroupsMinHeight, kSensorGroupsMaxHeight] 与
//    kAdapterRegionHeight 上限稳定，退化输入（0/负/NaN）有防御；
//  - 定高常量下的可视行数、卡片高度/溢出估算数学确定且单调合理；
//  - 显隐状态 → 分组可见集合（SensorGroupMask）与逐组
//    SensorGroupDefaultVisible/SensorGroupMaskHas 语义一致。
#include "selftest/TestFramework.h"
#include "app/ui3/PageLayout.h"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace stm::ui3;

namespace {

constexpr int kGroupCount = static_cast<int>(SensorGroup::Count);
constexpr size_t kGroupCountSz = static_cast<size_t>(kGroupCount);

// 产品默认可见性（与 DrawVisibilityRow 的复选框初值同一来源）。
std::array<bool, kGroupCountSz> DefaultVisibility() {
    std::array<bool, kGroupCountSz> v = {};
    for (size_t i = 0; i < kGroupCountSz; ++i) {
        v[i] = SensorGroupDefaultVisible(static_cast<SensorGroup>(i));
    }
    return v;
}

}  // namespace

// 契约 1：传感器分组区高度只依赖可用高度 —— 内容（读数条目/LHM 行/说明行）
// 绝不是输入；同一 avail 下任意「内容状态」反复取值必须逐位相同，且钳制到
// [240, 600]（定高 → 区外坐标恒定 → 页头不随内容移动）。
STM_TEST(ui_m2_sensor_region_height_constant) {
    const float kAvail[] = {480.0f, 600.0f, 640.0f, 900.0f, 1440.0f};
    for (float avail : kAvail) {
        // 内容状态只出现在注释里：0 条 / 50 条读数 / 说明行出现……高度函数
        // 的签名没有内容参数 —— 对同一 avail 重复取值（模拟任意内容增减）
        // 必须完全一致。
        const float base = SensorGroupsRegionHeight(avail);
        for (int state = 0; state < 16; ++state) {
            if (SensorGroupsRegionHeight(avail) != base) {
                *err = L"分组区高度随取值次数变化（应只依赖 avail）";
                return false;
            }
        }
        if (base < kSensorGroupsMinHeight || base > kSensorGroupsMaxHeight) {
            *err = L"分组区高度越出 [240, 600] 钳制边界";
            return false;
        }
    }
    // 大窗口恒为上限 600（页总高 = 顶栏 + 600，父级永不滚动）。
    if (SensorGroupsRegionHeight(1440.0f) != kSensorGroupsMaxHeight) {
        *err = L"大窗口分组区高度应为上限 600";
        return false;
    }
    // 中段线性：600 可用 → 600 - 余量（余量为正的常量）。
    const float mid = SensorGroupsRegionHeight(600.0f);
    if (mid <= 0.0f || mid >= 600.0f ||
        mid < 600.0f - kSensorRegionBottomMargin - 0.001f) {
        *err = L"中段高度应等于 avail - 常量余量";
        return false;
    }
    // 极矮窗口收缩到下限为止（降级：定高仍恒定）。
    if (SensorGroupsRegionHeight(100.0f) != kSensorGroupsMinHeight) {
        *err = L"极矮窗口分组区高度应为下限 240";
        return false;
    }
    // 退化输入防御：0 / 负 / NaN 都必须产出钳制范围内的定值。
    if (!(SensorGroupsRegionHeight(0.0f) == kSensorGroupsMinHeight) ||
        !(SensorGroupsRegionHeight(-50.0f) == kSensorGroupsMinHeight)) {
        *err = L"0/负可用高度应退化为下限";
        return false;
    }
    const float nanH =
        SensorGroupsRegionHeight(std::numeric_limits<float>::quiet_NaN());
    if (!(nanH >= kSensorGroupsMinHeight && nanH <= kSensorGroupsMaxHeight)) {
        *err = L"NaN 可用高度应落入钳制范围（不得传播 NaN）";
        return false;
    }
    return true;
}

// 契约 2：适配器卡区定高上限与可视行数 / 卡片高度估算数学（网络页头部
// 「卡片较多…」行内提示的依据；SameLine 提示不增减行数）。
STM_TEST(ui_m2_adapter_region_visible_rows) {
    // 正常窗口恒为 200px 上限；卡片数量/展开状态不是输入（同 avail 恒定）。
    for (int cards = 0; cards <= 12; ++cards) {
        if (AdapterRegionHeight(900.0f) != kAdapterRegionHeight) {
            *err = L"适配器卡区高度应恒为上限 200（与卡片数量无关）";
            return false;
        }
    }
    if (AdapterRegionHeight(50.0f) != 50.0f) {
        *err = L"极矮窗口适配器卡区应退化为可用高度";
        return false;
    }
    if (!(AdapterRegionHeight(0.0f) > 0.0f) ||
        !(AdapterRegionHeight(-1.0f) > 0.0f) ||
        std::isnan(AdapterRegionHeight(std::numeric_limits<float>::quiet_NaN()))) {
        *err = L"0/负/NaN 可用高度的退化值必须为正且非 NaN";
        return false;
    }
    // 可视行数：floor 语义 + 防御。
    constexpr float kRow = 20.0f;
    if (VisibleRowsInFixedHeight(kAdapterRegionHeight, kRow) != 10) {
        *err = L"200px 定高、20px 行高应可见 10 行";
        return false;
    }
    if (VisibleRowsInFixedHeight(199.0f, kRow) != 9) {
        *err = L"199px 应向下取整为 9 行";
        return false;
    }
    if (VisibleRowsInFixedHeight(0.0f, kRow) != 0 ||
        VisibleRowsInFixedHeight(kAdapterRegionHeight, 0.0f) != 0 ||
        VisibleRowsInFixedHeight(-5.0f, kRow) != 0 ||
        VisibleRowsInFixedHeight(kAdapterRegionHeight,
                                 std::numeric_limits<float>::quiet_NaN()) != 0) {
        *err = L"非法输入的可视行数应为 0";
        return false;
    }
    // 单卡估算：基线 9 行（1 标题 + 6 明细 + 2 内边距），随额外 IPv6 行线性。
    const float card0 = AdapterCardHeightEstimate(kRow, 0);
    if (card0 != kRow * static_cast<float>(kAdapterCardFixedRows + kAdapterCardPadRows)) {
        *err = L"单卡基线高度应为 9 行";
        return false;
    }
    if (AdapterCardHeightEstimate(kRow, 3) != card0 + 3.0f * kRow) {
        *err = L"额外地址行应按行高线性累加";
        return false;
    }
    if (AdapterCardHeightEstimate(kRow, -2) != card0) {
        *err = L"负的额外行数应按 0 处理";
        return false;
    }
    // 总高 = Σ 单卡 + 额外地址行 + 卡片间条目间距（n 张共 n-1 个间隙）。
    const int extras[3] = {0, 1, 0};
    const float total = AdapterCardsTotalHeight(kRow, extras, 3);
    const float expect = 3.0f * card0 +  // 3 张基线卡
                         1.0f * kRow +   // 第 2 张卡 +1 额外地址行
                         2.0f * kRow;    // 3 张卡之间的 2 个间隙
    if (total != expect) {
        *err = L"卡片总高估算应为 Σ单卡 + 额外行 + (n-1) 个间隙";
        return false;
    }
    if (AdapterCardsTotalHeight(kRow, nullptr, 0) != 0.0f ||
        AdapterCardsTotalHeight(kRow, nullptr, 2) != 2.0f * card0 + kRow) {
        *err = L"空卡列表总高应为 0；无额外行时 = 2 卡 + 1 间隙";
        return false;
    }
    // 溢出判定：恰好等于定高不溢出，超过即溢出（提示出现的唯一依据）。
    const float region = AdapterRegionHeight(900.0f);
    if (AdapterCardsOverflow(region, region) ||
        !AdapterCardsOverflow(region, region + 0.5f)) {
        *err = L"溢出判定边界错误（等于不溢出，超过才溢出）";
        return false;
    }
    return true;
}

// 契约 3：显隐状态 → 分组可见集合（SensorGroupMask）—— 与逐组默认值
// 语义一致，位序即 SensorGroup 枚举序；越界/超长输入有防御。
STM_TEST(ui_m2_sensor_group_mask) {
    const std::array<bool, kGroupCountSz> defaults = DefaultVisibility();
    const SensorGroupMask m =
        SensorGroupMaskFromFlags(defaults.data(), kGroupCount);
    // 产品默认：除风扇外全部可见（与 SensorGroupDefaultVisible 逐组一致）。
    int expectCount = 0;
    for (int i = 0; i < kGroupCount; ++i) {
        const SensorGroup g = static_cast<SensorGroup>(i);
        const bool has = SensorGroupMaskHas(m, g);
        if (has != SensorGroupDefaultVisible(g)) {
            *err = L"默认可见集合与 SensorGroupDefaultVisible 不一致：组 ";
            *err += std::to_wstring(i);
            return false;
        }
        if (has) ++expectCount;
    }
    if (SensorGroupMaskHas(m, SensorGroup::Fan)) {
        *err = L"风扇默认必须不可见";
        return false;
    }
    if (SensorGroupMaskCount(m) != expectCount ||
        expectCount != kGroupCount - 1) {
        *err = L"默认可见集合基数应为 组总数-1（风扇关闭）";
        return false;
    }
    // 显隐切换：翻一个开关 → 集合恰好多/少一个组（用户报告的场景：
    // 组显隐切换不得改变任何区外布局，这里锁定集合数学本身）。
    std::array<bool, kGroupCountSz> toggled = defaults;
    toggled[static_cast<size_t>(SensorGroup::Fan)] = true;   // 打开风扇
    toggled[static_cast<size_t>(SensorGroup::Cpu)] = false;  // 关闭 CPU
    const SensorGroupMask m2 = SensorGroupMaskFromFlags(toggled.data(), kGroupCount);
    if (!SensorGroupMaskHas(m2, SensorGroup::Fan) ||
        SensorGroupMaskHas(m2, SensorGroup::Cpu)) {
        *err = L"开关翻转未正确反映到可见集合";
        return false;
    }
    if (SensorGroupMaskCount(m2) != expectCount) {
        *err = L"一开一关后基数应不变";
        return false;
    }
    // 全开 / 全关 / 仅 CPU。
    const std::array<bool, kGroupCountSz> allOn = [] {
        std::array<bool, kGroupCountSz> a = {};
        a.fill(true);
        return a;
    }();
    const std::array<bool, kGroupCountSz> allOff = {};
    if (SensorGroupMaskCount(SensorGroupMaskFromFlags(allOn.data(), kGroupCount)) !=
        kGroupCount) {
        *err = L"全开集合基数应等于组总数";
        return false;
    }
    const SensorGroupMask none = SensorGroupMaskFromFlags(allOff.data(), kGroupCount);
    if (none != 0u || SensorGroupMaskCount(none) != 0) {
        *err = L"全关集合应为 0";
        return false;
    }
    const std::array<bool, kGroupCountSz> cpuOnly = [] {
        std::array<bool, kGroupCountSz> a = {};
        a[static_cast<size_t>(SensorGroup::Cpu)] = true;
        return a;
    }();
    const SensorGroupMask m3 = SensorGroupMaskFromFlags(cpuOnly.data(), kGroupCount);
    if (!SensorGroupMaskHas(m3, SensorGroup::Cpu) ||
        SensorGroupMaskCount(m3) != 1) {
        *err = L"仅 CPU 可见的集合应只含 CPU（未初始化的 false 位不得置位）";
        return false;
    }
    // 防御：count 超长忽略、负数按 0、枚举越界一律 false。
    if (SensorGroupMaskFromFlags(allOn.data(), kGroupCount + 10) !=
        SensorGroupMaskFromFlags(allOn.data(), kGroupCount)) {
        *err = L"超长 count 应被钳制到组总数";
        return false;
    }
    if (SensorGroupMaskFromFlags(allOn.data(), -1) != 0u) {
        *err = L"负 count 应按 0 处理";
        return false;
    }
    if (SensorGroupMaskHas(m, static_cast<SensorGroup>(kGroupCount)) ||
        SensorGroupMaskHas(m, static_cast<SensorGroup>(-1))) {
        *err = L"越界枚举的可见性应为 false";
        return false;
    }
    return true;
}

// 契约 4（补充）：溢出提示对输入单调确定 —— 卡片增多/地址行增多只会
// 让估算总高单调不减，从而提示出现与否逐帧确定（不抖动）。
STM_TEST(ui_m2_adapter_overflow_monotonic) {
    constexpr float kRow = 20.0f;
    float prev = 0.0f;
    for (int n = 0; n <= 8; ++n) {
        std::vector<int> extras(static_cast<size_t>(n), 0);
        const float total =
            AdapterCardsTotalHeight(kRow, extras.data(), static_cast<int>(extras.size()));
        if (total < prev) {
            *err = L"卡片增多后估算总高不得减少";
            return false;
        }
        prev = total;
    }
    // 地址行增多同样单调不减。
    const int extras[3] = {0, 2, 5};
    if (!(AdapterCardsTotalHeight(kRow, extras, 3) >=
          AdapterCardsTotalHeight(kRow, extras, 2))) {
        *err = L"计入更多卡片后总高不得减少";
        return false;
    }
    // 同一输入下确定性：两次估算逐位相等（提示不逐帧抖动的前提）。
    const float a = AdapterCardsTotalHeight(kRow, extras, 3);
    const float b = AdapterCardsTotalHeight(kRow, extras, 3);
    if (a != b) {
        *err = L"同一输入的估算必须逐位确定";
        return false;
    }
    return true;
}

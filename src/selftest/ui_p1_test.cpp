// P1 维护轮回归（2026-09）：五项用户报告修复的纯函数契约。
//  ① 状态栏：锚点末端数学（AnchorEndX）与降级原因按宽截断（EllipsizeTextUtf8，
//     应用注入 ImGui::CalcTextSize，这里注入确定性字宽模型 —— 与
//     ui_status_slots_test.cpp 的 FakeMeasure 同一套约定）。
//  ② 进程表列拖动重排：列枚举 ↔ UserID ↔ 持久化键映射（SortKey.h）与
//     colOrder 逗号串序列化/校验。
//  ③ 网络页列宽持久化：netcol_<表名>_<列> 键名生成与登记清单
//    （ThemeCfg.h），含按前缀/精确键剔除。
//  ④ 一键优化布局：分辨率/DPI → 缩放系数（LayoutScaleFromEnv）、运行时
//     缩放槽（SetLayoutScale/Scaled，缩放=1 与原常量逐位一致的回归断言）、
//     定高区域缩放变体。
//  ⑤ 传感器页定高区内容不足收缩（FixedRegionShrinkToContent）。
#include "selftest/TestFramework.h"
#include "app/ui/HeaderLayout.h"
#include "app/ui/SortKey.h"
#include "app/ui3/NetMonUi.h"  // kNetMonSectionHeight（缩放回归断言用）
#include "app/ui3/PageLayout.h"
#include "app/ui3/StatusLayout.h"
#include "app/ui3/ThemeCfg.h"
#include "core/Cfg.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace stm;
using namespace stm::ui;
using namespace stm::ui3;

namespace {

// 确定性字宽模型（仿真 ImGui::CalcTextSize）：UTF-8 多字节序列 = 18，
// ASCII 数字 = 8，小数点/空格 = 5，其他 ASCII = 9。只要求确定性。
float FakeMeasure(const char* u8) {
    float w = 0.0f;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(u8); *p; ++p) {
        if (*p >= 0xC0) w += 18.0f;
        else if (*p >= 0x80) continue;
        else if (*p >= '0' && *p <= '9') w += 8.0f;
        else if (*p == '.' || *p == ' ') w += 5.0f;
        else w += 9.0f;
    }
    return w;
}

std::wstring TempFile(const wchar_t* name) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + name;
}

}  // namespace

// ---- ①：锚点末端与按宽截断 --------------------------------------------------

STM_TEST(p1_status_anchor_end_math) {
    // 锚点末端 = 起点 + 文本实测宽（绝不依赖 ImGui 光标 —— 旧实现读回的
    // GetCursorPosX() 恒为行首，是重叠的直接根因）。
    if (AnchorEndX(8.0f, 0.0f) != 8.0f) { *err = L"零宽文本锚点末端应等于起点"; return false; }
    if (AnchorEndX(8.0f, 42.5f) != 50.5f) { *err = L"锚点末端应为起点+宽度"; return false; }
    if (!(AnchorEndX(8.0f, -3.0f) == 8.0f)) { *err = L"负宽度应按 0 处理"; return false; }
    // 与槽位布局衔接：锚点之后从 anchorEnd + 间距起布槽，槽内不叠锚点。
    const float anchorEnd = AnchorEndX(8.0f, FakeMeasure("完整模式"));
    float xs[1] = {};
    const float slotW[1] = {60.0f};
    LayoutFlowSlots(anchorEnd + 8.0f, 8.0f, 4096.0f, slotW, 1, xs);
    if (xs[0] < anchorEnd + 8.0f) { *err = L"首槽起点必须不小于锚点末端+间距"; return false; }
    return true;
}

STM_TEST(p1_status_ellipsize_short_text_untouched) {
    // 放得下的文本原样保留（含多字节中文）。
    const char* shortU8 = "兼容模式：采集能力受限";
    char out[256];
    const int n = EllipsizeTextUtf8(shortU8, 4096.0f, FakeMeasure, out, sizeof(out));
    if (n <= 0 || std::string(out) != shortU8) {
        *err = L"预算充足时文本不得被截断";
        return false;
    }
    // 空输入：空输出。
    if (EllipsizeTextUtf8("", 100.0f, FakeMeasure, out, sizeof(out)) != 0 || out[0] != '\0') {
        *err = L"空输入应产生空输出";
        return false;
    }
    // 非法预算（0/负）：空输出（调用方有最小预算保底，见 Pages.cpp）。
    if (EllipsizeTextUtf8(shortU8, 0.0f, FakeMeasure, out, sizeof(out)) != 0 ||
        EllipsizeTextUtf8(shortU8, -1.0f, FakeMeasure, out, sizeof(out)) != 0) {
        *err = L"0/负预算应产生空输出";
        return false;
    }
    return true;
}

STM_TEST(p1_status_ellipsize_truncates_with_ellipsis) {
    // 一串中文（每字符 18px）：预算 100 → 放得下 5 字符（90）+「…」。
    std::string longU8;
    for (int i = 0; i < 20; ++i) longU8 += "\xE5\xBE\x88";  // "很" x20（3 字节/字符）
    char out[256];
    const int n = EllipsizeTextUtf8(longU8.c_str(), 100.0f, FakeMeasure, out, sizeof(out));
    if (n <= 0) { *err = L"超长文本应被截断而非清空"; return false; }
    const std::string result(out);
    if (result.size() >= longU8.size()) {
        *err = L"截断结果必须短于原文";
        return false;
    }
    // 以「…」（U+2026 = E2 80 A6）结尾。
    if (result.size() < 3 || result.compare(result.size() - 3, 3, "\xE2\x80\xA6") != 0) {
        *err = L"截断结果必须以省略号结尾";
        return false;
    }
    // 结果实测宽（含省略号）必须不超预算。
    if (FakeMeasure(result.c_str()) > 100.0f) {
        *err = L"截断结果宽度超出预算";
        return false;
    }
    // 完整码点边界：字节数 - 3（省略号）必须能被 3 整除（"很" 是 3 字节）。
    if ((result.size() - 3) % 3 != 0) {
        *err = L"截断必须落在完整码点边界上";
        return false;
    }
    // 极小预算：连省略号（18px）都放不下 → 空输出（调用方以最小预算保底，
    // 见 Pages.cpp DrawStatusBar；此处钉死纯函数本身的诚实降级契约）。
    const int tiny = EllipsizeTextUtf8(longU8.c_str(), 10.0f, FakeMeasure, out, sizeof(out));
    if (tiny != 0 || out[0] != '\0') {
        *err = L"极小预算（省略号也放不下）应输出空";
        return false;
    }
    // 缓冲区上限：4 字节输出槽只装得下省略号（含 NUL 恰好 4 字节）。
    char small[4];
    const int cap = EllipsizeTextUtf8(longU8.c_str(), 100.0f, FakeMeasure, small, sizeof(small));
    if (cap != 3 || std::string(small) != "\xE2\x80\xA6") {
        *err = L"缓冲区不足时应安全截断到省略号";
        return false;
    }
    return true;
}

// ---- ②：进程表列 UserID / colOrder ------------------------------------------

STM_TEST(p1_proc_column_userid_mapping) {
    // 槽位 ↔ UserID 恒等且覆盖全部 13 列（0..10 可排序 + 徽标 + 描述）。
    for (int slot = 0; slot < kProcColSlots; ++slot) {
        if (ProcColumnUserId(slot) != slot) {
            *err = L"UserID 必须与列槽位一一对应";
            return false;
        }
    }
    // 可排序性：0..Count-1 可排，徽标/描述/越界不可排。
    if (!ProcColumnIsSortableUserId(0) ||
        !ProcColumnIsSortableUserId(static_cast<int>(SortColumn::CtxSwitches))) {
        *err = L"数据列必须可排序";
        return false;
    }
    if (ProcColumnIsSortableUserId(kProcColUserIdBadges) ||
        ProcColumnIsSortableUserId(kProcColUserIdDesc) ||
        ProcColumnIsSortableUserId(-1) ||
        ProcColumnIsSortableUserId(kProcColSlots + 5)) {
        *err = L"徽标/描述/越界 UserID 不可排序";
        return false;
    }
    // UserID -> SortColumn 逆映射：可排列有效，徽标/描述拒绝。
    SortColumn c = SortColumn::Name;
    for (int i = 0; i < static_cast<int>(SortColumn::Count); ++i) {
        if (!ProcColumnFromUserId(i, &c) || c != static_cast<SortColumn>(i)) {
            *err = L"UserID 逆映射失败";
            return false;
        }
    }
    if (ProcColumnFromUserId(kProcColUserIdBadges, &c) ||
        ProcColumnFromUserId(kProcColUserIdDesc, &c)) {
        *err = L"徽标/描述列不得产出排序键";
        return false;
    }
    // 键槽契约：SortColumnId 仍按槽位对应（重排后按 UserID 取键，显示序无关）。
    if (std::wstring(SortColumnId(static_cast<SortColumn>(1))) != L"pid") {
        *err = L"SortColumnId(1) 应为 pid（槽位序契约）";
        return false;
    }
    return true;
}

STM_TEST(p1_proc_colorder_roundtrip) {
    int order[kProcColSlots] = {};
    if (ProcColumnDefaultOrder(order, kProcColSlots) != kProcColSlots) {
        *err = L"默认序生成失败";
        return false;
    }
    // 默认序序列化/解析往返。
    const std::wstring s = ProcColumnOrderToCfg(order, kProcColSlots);
    if (s != L"0,1,2,3,4,5,6,7,8,9,10,11,12") {
        *err = L"默认 colOrder 串不符预期：" + s;
        return false;
    }
    int back[kProcColSlots] = {};
    if (ProcColumnOrderFromCfg(s.c_str(), back, kProcColSlots) != kProcColSlots) {
        *err = L"默认 colOrder 解析失败";
        return false;
    }
    for (int i = 0; i < kProcColSlots; ++i) {
        if (back[i] != order[i]) { *err = L"默认序往返不一致"; return false; }
    }
    // 拖动后的排列（0 与 1 交换）往返保持。
    std::swap(order[0], order[1]);
    int back2[kProcColSlots] = {};
    if (ProcColumnOrderFromCfg(ProcColumnOrderToCfg(order, kProcColSlots).c_str(),
                               back2, kProcColSlots) != kProcColSlots) {
        *err = L"交换序解析失败";
        return false;
    }
    if (back2[0] != 1 || back2[1] != 0) { *err = L"交换序往返不一致"; return false; }
    return true;
}

STM_TEST(p1_proc_colorder_rejects_invalid) {
    const wchar_t* kBad[] = {
        L"",                                   // 空
        L"0,1,2",                              // 字段不足
        L"0,1,2,3,4,5,6,7,8,9,10,11,12,13",    // 越界值 + 字段过多
        L"1,1,2,3,4,5,6,7,8,9,10,11,12",       // 重复
        L"0,1,2,3,4,5,6,7,8,9,10,11,",         // 尾逗号
        L"0,,2,3,4,5,6,7,8,9,10,11,12",        // 空字段
        L"a,1,2,3,4,5,6,7,8,9,10,11,12",       // 非数字
        L"0,1,2,3,4,5,6,7,8,9,10,11,99",       // 越界
    };
    int out[kProcColSlots] = {};
    for (const wchar_t* bad : kBad) {
        if (ProcColumnOrderFromCfg(bad, out, kProcColSlots) != -1) {
            *err = L"非法 colOrder 未被拒绝";
            return false;
        }
    }
    // 缺失排列校验：0..11 有、缺 12 的串必须被拒（不是合法排列）。
    if (ProcColumnOrderFromCfg(L"0,1,2,3,4,5,6,7,8,9,10,12", out, kProcColSlots) != -1) {
        *err = L"缺列的串必须被拒绝";
        return false;
    }
    return true;
}

// ---- ③：netcol_* 键名与剔除 --------------------------------------------------

STM_TEST(p1_netcol_key_names) {
    // 键名形态：netcol_<表名>_<列>。
    if (ui3::NetColCfgKey("netconn", 0) != L"netcol_netconn_0") {
        *err = L"键名形态错误：" + ui3::NetColCfgKey("netconn", 0);
        return false;
    }
    if (ui3::NetColCfgKey("netmon_events", 7) != L"netcol_netmon_events_7") {
        *err = L"键名形态错误：" + ui3::NetColCfgKey("netmon_events", 7);
        return false;
    }
    // 登记清单：7 张表，列数与 UI 中 BeginTable 一致。
    int count = 0;
    const ui3::NetColTableSpec* specs = ui3::NetColTableSpecs(&count);
    if (count != 7) { *err = L"netcol 表登记数量应为 7"; return false; }
    int expectCols[7] = {8, 5, 3, 5, 6, 4, 4};  // events/top/dns/pcap/netconn/gpuA/gpuP
    const char* expectIds[7] = {"netmon_events", "netmon_top", "netmon_dns", "pcap_pkts",
                                "netconn", "gpuadapters", "gpuprocs"};
    int total = 0;
    for (int t = 0; t < count; ++t) {
        if (std::string(specs[t].id) != expectIds[t] || specs[t].cols != expectCols[t]) {
            *err = L"netcol 表登记与 UI 不一致";
            return false;
        }
        total += specs[t].cols;
    }
    const std::vector<std::wstring> keys = ui3::NetColCfgKeys();
    if (static_cast<int>(keys.size()) != total) {
        *err = L"键清单数量与登记列数不一致";
        return false;
    }
    if (keys.front() != L"netcol_netmon_events_0" ||
        keys.back() != L"netcol_gpuprocs_3") {
        *err = L"键清单首尾不符（覆盖顺序错乱）";
        return false;
    }
    return true;
}

STM_TEST(p1_netcol_strip_from_file) {
    // 「重置布局」剔除：colW_*/netcol_* 前缀 + layoutScale/colOrder/perfZoom
    // 精确键全部消失，无关键原样保留。
    const std::wstring p = TempFile(L"stm_selftest_netcol.json");
    {
        Config c;
        c.SetDouble(L"netcol_netconn_1", 260.0);
        c.SetDouble(L"netcol_gpuadapters_3", 90.0);
        c.SetDouble(L"colW_pid", 80.0);
        c.SetDouble(L"layoutScale", 1.5);
        c.SetString(L"colOrder", L"1,0,2,3,4,5,6,7,8,9,10,11,12");
        c.SetInt(L"perfZoom", 3);
        c.SetInt(L"intervalMs", 1000);  // 非布局键：必须保留
        if (!c.Save(p)) { *err = L"Config::Save 失败"; return false; }
    }
    const int removed = ui3::StripLayoutKeysFromFile(p);
    Config r;
    bool ok = removed == 6 && r.Load(p) &&
              r.GetDouble(L"netcol_netconn_1", -1.0) == -1.0 &&
              r.GetDouble(L"netcol_gpuadapters_3", -1.0) == -1.0 &&
              r.GetDouble(L"colW_pid", -1.0) == -1.0 &&
              r.GetDouble(L"layoutScale", -1.0) == -1.0 &&
              r.GetString(L"colOrder").empty() && r.GetInt(L"perfZoom", -1) == -1 &&
              r.GetInt(L"intervalMs", 0) == 1000;
    DeleteFileW(p.c_str());
    if (!ok) {
        *err = L"布局键剔除结果不符（应剔 6 行且保留非布局键）";
        return false;
    }
    return true;
}

// ---- ④：布局缩放 -------------------------------------------------------------

STM_TEST(p1_layout_scale_from_env) {
    // 分辨率因子（1080p/1440p/4K 基准）。
    if (LayoutScaleFromEnv(1080.0f, 96.0f) != 1.0f) {
        *err = L"1080p@100% 缩放应为 1.0";
        return false;
    }
    if (LayoutScaleFromEnv(1440.0f, 96.0f) != 1.15f) {
        *err = L"1440p@100% 缩放应为 1.15";
        return false;
    }
    if (LayoutScaleFromEnv(2160.0f, 96.0f) != 1.30f) {
        *err = L"4K@100% 缩放应为 1.30";
        return false;
    }
    // DPI 因子（144 = 150%）取 max，不乘积。
    if (LayoutScaleFromEnv(1080.0f, 144.0f) != 1.5f) {
        *err = L"1080p@150% DPI 缩放应为 1.5";
        return false;
    }
    if (LayoutScaleFromEnv(1440.0f, 120.0f) != 1.25f) {
        *err = L"max(1.15, 1.25) 应为 1.25";
        return false;
    }
    // 钳制上限 2.0（4K@200%）与下限 1.0。
    if (LayoutScaleFromEnv(2160.0f, 192.0f) != 2.0f) {
        *err = L"4K@200% 应钳制到 2.0";
        return false;
    }
    if (LayoutScaleFromEnv(720.0f, 96.0f) != 1.0f) {
        *err = L"小窗口缩放应保持 1.0 下限";
        return false;
    }
    // 非法输入：NaN/0/负 → 回退 1.0（绝不放大也不传播 NaN）。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!(LayoutScaleFromEnv(nan, nan) == 1.0f) ||
        !(LayoutScaleFromEnv(0.0f, 0.0f) == 1.0f) ||
        !(LayoutScaleFromEnv(-100.0f, -96.0f) == 1.0f)) {
        *err = L"非法输入应回退 1.0";
        return false;
    }
    return true;
}

STM_TEST(p1_layout_scale_identity_and_slot) {
    // 缩放槽默认 1.0；Scaled 在 1.0 时与原常量逐位一致（回归断言）。
    SetLayoutScale(1.0f);
    if (LayoutScale() != 1.0f) { *err = L"缩放槽应可复位到 1.0"; return false; }
    if (Scaled(kAdapterRegionHeight) != kAdapterRegionHeight ||
        Scaled(kNetMonSectionHeight) != kNetMonSectionHeight ||
        Scaled(kSensorGroupsMaxHeight) != kSensorGroupsMaxHeight) {
        *err = L"缩放=1 时 Scaled 必须与原常量逐位一致";
        return false;
    }
    // 设置/钳制：超上限钳 2.0，非法回 1.0。
    if (SetLayoutScale(1.5f) != 1.5f || LayoutScale() != 1.5f) {
        *err = L"SetLayoutScale(1.5) 应生效";
        return false;
    }
    if (Scaled(200.0f) != 300.0f) { *err = L"缩放 1.5 下 200 应为 300"; return false; }
    if (SetLayoutScale(9.0f) != 2.0f) { *err = L"缩放应钳制到 2.0"; return false; }
    if (SetLayoutScale(-1.0f) != 1.0f) { *err = L"负缩放应回 1.0"; return false; }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (SetLayoutScale(nan) != 1.0f) { *err = L"NaN 缩放应回 1.0"; return false; }
    return true;
}

STM_TEST(p1_layout_scaled_regions_regression) {
    // 缩放=1：缩放变体与原函数逐位一致（验收要求的布局回归断言）。
    if (AdapterRegionHeightScaled(900.0f, 1.0f) != AdapterRegionHeight(900.0f) ||
        AdapterRegionHeightScaled(900.0f, 1.0f) != kAdapterRegionHeight) {
        *err = L"缩放=1 时适配器区高度回归失败";
        return false;
    }
    if (SensorGroupsRegionHeightScaled(1440.0f, 1.0f) !=
            SensorGroupsRegionHeight(1440.0f) ||
        SensorGroupsRegionHeightScaled(100.0f, 1.0f) !=
            SensorGroupsRegionHeight(100.0f)) {
        *err = L"缩放=1 时传感器分组区高度回归失败";
        return false;
    }
    // 缩放=1.5：适配器区 200→300，退化语义保持（可用高 < 上限时取可用高）。
    if (AdapterRegionHeightScaled(900.0f, 1.5f) != 300.0f) {
        *err = L"缩放 1.5 下适配器区应为 300";
        return false;
    }
    if (AdapterRegionHeightScaled(150.0f, 1.5f) != 150.0f) {
        *err = L"极矮窗口适配器区应退化为可用高度";
        return false;
    }
    // 传感器分组区：600→900；缩放 >1 时下限越过可用高度则钳回。
    if (SensorGroupsRegionHeightScaled(1440.0f, 1.5f) != 900.0f) {
        *err = L"缩放 1.5 下分组区上限应为 900";
        return false;
    }
    if (SensorGroupsRegionHeightScaled(300.0f, 1.5f) != 300.0f) {
        *err = L"缩放后越界应钳回可用高度";
        return false;
    }
    // 非法缩放回退原函数。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (AdapterRegionHeightScaled(900.0f, nan) != AdapterRegionHeight(900.0f) ||
        SensorGroupsRegionHeightScaled(900.0f, 0.0f) !=
            SensorGroupsRegionHeight(900.0f)) {
        *err = L"非法缩放应回退原函数";
        return false;
    }
    return true;
}

// ---- ⑤：定高区内容不足收缩 ---------------------------------------------------

STM_TEST(p1_fixed_region_shrink_to_content) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // 内容不足：收缩到内容高（空白底消失）。
    if (FixedRegionShrinkToContent(600.0f, 220.0f) != 220.0f) {
        *err = L"内容 220 应收缩到 220";
        return false;
    }
    // 内容超出：维持定高（M2 滚动语义）。
    if (FixedRegionShrinkToContent(600.0f, 1200.0f) != 600.0f) {
        *err = L"内容超出应维持定高";
        return false;
    }
    // 恰好等于定高：定高。
    if (FixedRegionShrinkToContent(600.0f, 600.0f) != 600.0f) {
        *err = L"内容等于定高应维持定高";
        return false;
    }
    // 内容未知（首帧 0 / NaN）：维持定高。
    if (FixedRegionShrinkToContent(600.0f, 0.0f) != 600.0f ||
        !(FixedRegionShrinkToContent(600.0f, nan) == 600.0f)) {
        *err = L"内容未知应维持定高";
        return false;
    }
    // 定高非法：回正的内容高或 0。
    if (FixedRegionShrinkToContent(0.0f, 150.0f) != 150.0f ||
        !(FixedRegionShrinkToContent(nan, 0.0f) == 0.0f)) {
        *err = L"定高非法时的退化值错误";
        return false;
    }
    return true;
}

// ---- ③ 补：布局重置键清单覆盖 ------------------------------------------------

STM_TEST(p1_layout_reset_keys_registry) {
    const std::vector<std::wstring> exact = ui3::LayoutResetExactKeys();
    bool hasScale = false, hasOrder = false, hasZoom = false;
    bool hasAdapterH = false, hasNetmonH = false;
    for (const std::wstring& k : exact) {
        if (k == L"layoutScale") hasScale = true;
        if (k == L"colOrder") hasOrder = true;
        if (k == L"perfZoom") hasZoom = true;
        if (k == L"netAdapterH") hasAdapterH = true;   // V34-P1-N1：拖拽分栏高度
        if (k == L"netmonH") hasNetmonH = true;
    }
    if (!hasScale || !hasOrder || !hasZoom || !hasAdapterH || !hasNetmonH) {
        *err = L"布局重置精确键清单缺少 layoutScale/colOrder/perfZoom/netAdapterH/netmonH";
        return false;
    }
    // 清单数量校验防漏登记：colW_*（Count+2）+ netcol_* + 5 个精确键。
    const size_t expect = static_cast<size_t>(SortColumn::Count) + 2 +
                          ui3::NetColCfgKeys().size() + 5;
    Config c;
    ui3::SoftDeleteLayoutKeys(c);  // 冒烟：空 cfg 上软删除不得崩溃
    const std::vector<std::wstring> colW = ui3::ColWidthCfgKeys();
    if (colW.size() + ui3::NetColCfgKeys().size() + 5 != expect) {
        *err = L"布局键清单数量不符（可能重复/漏登记）";
        return false;
    }
    return true;
}

// Wave-2 UI 逻辑测试：传感器页分组可见性配置（传感器页
// 重设计）、LHM 标签归类、每核标签解析，以及 F4
// 模块区决策辅助。全部为仅头文件纯逻辑——无 GUI。
#include "selftest/TestFramework.h"
#include "app/ui/ModulesUi.h"
#include "app/ui3/PageHelpers.h"
#include "core/Cfg.h"
#include <windows.h>
#include <string>
#include <vector>

using namespace stm;
using namespace stm::ui;
using namespace stm::ui3;

// 每个传感器组都有独立 cfg 键，且文档化的产品默认值
// (all visible, 风扇 hidden by default: it can only report 需要驱动支持 honestly).
STM_TEST(sensor_group_keys_defaults) {
    bool seen[static_cast<int>(SensorGroup::Count)] = {};
    for (int i = 0; i < static_cast<int>(SensorGroup::Count); ++i) {
        const SensorGroup g = static_cast<SensorGroup>(i);
        const wchar_t* key = SensorGroupCfgKey(g);
        if (key == nullptr || *key == L'\0') {
            *err = L"存在空的传感器组配置键";
            return false;
        }
        const int idx = static_cast<int>(g);
        seen[idx] = true;
        if (SensorGroupDefaultVisible(g) != (g != SensorGroup::Fan)) {
            *err = L"默认可见性错误：仅风扇应默认关闭";
            return false;
        }
    }
    for (int i = 0; i < static_cast<int>(SensorGroup::Count); ++i) {
        if (!seen[i]) { *err = L"传感器组枚举未遍历完整"; return false; }
    }
    // 标题也必须非空（渲染为组头）
    for (int i = 0; i < static_cast<int>(SensorGroup::Count); ++i) {
        if (*SensorGroupTitle(static_cast<SensorGroup>(i)) == L'\0') {
            *err = L"存在空的传感器组标题";
            return false;
        }
    }
    return true;
}

// 分组可见性 + LHM 选项在配置文件中往返（中文安全）。
STM_TEST(sensors_group_cfg_roundtrip) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring p = std::wstring(temp) + L"stm_selftest_senscfg.json";

    Config c1;
    // 键缺失时遵循默认
    for (int i = 0; i < static_cast<int>(SensorGroup::Count); ++i) {
        const SensorGroup g = static_cast<SensorGroup>(i);
        if (SensorGroupVisible(c1, g) != SensorGroupDefaultVisible(g)) {
            *err = L"缺省键时未返回产品默认可见性";
            return false;
        }
    }
    // 翻转所有组 + LHM 选项，保存，重载
    for (int i = 0; i < static_cast<int>(SensorGroup::Count); ++i) {
        const SensorGroup g = static_cast<SensorGroup>(i);
        c1.SetBool(SensorGroupCfgKey(g), !SensorGroupDefaultVisible(g));
    }
    c1.SetBool(L"lhmEnabled", true);
    c1.SetInt(L"lhmPort", 12345);
    if (!c1.Save(p)) { *err = L"传感器配置保存失败"; return false; }

    Config c2;
    if (!c2.Load(p)) { *err = L"传感器配置加载失败"; return false; }
    for (int i = 0; i < static_cast<int>(SensorGroup::Count); ++i) {
        const SensorGroup g = static_cast<SensorGroup>(i);
        if (SensorGroupVisible(c2, g) == SensorGroupDefaultVisible(g)) {
            *err = L"传感器组可见性往返失败";
            return false;
        }
    }
    if (!c2.GetBool(L"lhmEnabled", false)) { *err = L"lhmEnabled 往返失败"; return false; }
    if (c2.GetInt(L"lhmPort", 0) != 12345) { *err = L"lhmPort 往返失败"; return false; }
    DeleteFileW(p.c_str());
    return true;
}

// LHM 节点路径标签必须并入正确的传感器组（关键字扫描，
// 无歧义关键字优先）。“Other”只落到诚实的未分类行，
// 不到任何可见位置。
STM_TEST(lhm_group_classifier) {
    struct Case {
        const wchar_t* label;
        LhmGroup expect;
    };
    const Case cases[] = {
        {L"/lhm/my-pc/CPU/Core #1/Temperature", LhmGroup::Cpu},
        {L"/lhm/my-pc/CPU/Clock/核心 1", LhmGroup::Cpu},
        {L"/lhm/my-pc/GPU RTX 3080/GPU Core", LhmGroup::Gpu},
        {L"/lhm/my-pc/GPU/ temperatures/HOT spot", LhmGroup::Gpu},
        {L"/lhm/my-pc/RAM/Used", LhmGroup::Mem},
        {L"/lhm/my-pc/Memory/Physical Used", LhmGroup::Mem},
        {L"/lhm/my-pc/lpc/nct6779d/fan/1", LhmGroup::Fan},
        {L"/lhm/my-pc/Fan Control/FAN #2", LhmGroup::Fan},
        {L"/lhm/my-pc/nvme/SSD1/Remaining Life", LhmGroup::Disk},
        {L"/lhm/my-pc/HDD/Toshiba/Temp", LhmGroup::Disk},
        {L"/lhm/my-pc/Smart/Status", LhmGroup::Disk},
        {L"/lhm/my-pc/Battery/Charge Level", LhmGroup::Battery},
        {L"/lhm/my-pc/nic/ethernet/Throughput", LhmGroup::Net},
        {L"/lhm/my-pc/WiFi/Throughput", LhmGroup::Net},
        {L"/lhm/my-pc/motherboard/voltage/vcore", LhmGroup::Other},
    };
    for (const Case& c : cases) {
        if (LhmGroupOf(c.label) != c.expect) {
            *err = L"LHM 分组错误：" + std::wstring(c.label);
            return false;
        }
    }
    return true;
}

// 每核标签解析：索引提取 + 频率/利用率分类；聚合兜底标签
// 必须返回 -1，使其按普通读数渲染。
STM_TEST(sensor_core_index_parse) {
    bool isFreq = false;
    if (SensorCoreIndex(L"CPU 核 0 频率", &isFreq) != 0 || !isFreq) {
        *err = L"核心频率标签解析失败";
        return false;
    }
    if (SensorCoreIndex(L"CPU 核 11 占用率", &isFreq) != 11 || isFreq) {
        *err = L"核心占用率标签解析失败";
        return false;
    }
    if (SensorCoreIndex(L"CPU 核 #7 占用率", &isFreq) != 7) {
        *err = L"带 # 的核心标签解析失败";
        return false;
    }
    if (SensorCoreIndex(L"CPU 频率", &isFreq) != -1) {
        *err = L"聚合频率标签应返回 -1";
        return false;
    }
    if (SensorCoreIndex(L"CPU 每核占用率", &isFreq) != -1) {
        *err = L"聚合占用率标签应返回 -1";
        return false;
    }
    return true;
}

// F4：模块区状态机——详情任务运行期间为 pending、
// 列表不可读时诚实闸门，以及每模块签名验证结果的
// 徽标映射。
STM_TEST(module_section_and_badges) {
    if (DecideModuleSection(false, false, false, 0) != ModuleSectionState::Pending) {
        *err = L"详情未就绪应为 Pending";
        return false;
    }
    if (DecideModuleSection(true, false, false, 0) != ModuleSectionState::NeedAdmin) {
        *err = L"非提权拿不到模块应显示 NeedAdmin";
        return false;
    }
    if (DecideModuleSection(true, false, true, 0) != ModuleSectionState::Unavailable) {
        *err = L"提权仍拿不到模块应显示 Unavailable";
        return false;
    }
    if (DecideModuleSection(true, true, true, 3) != ModuleSectionState::Ready) {
        *err = L"模块已解析应为 Ready";
        return false;
    }
    for (ModuleSectionState s : {ModuleSectionState::Pending, ModuleSectionState::NeedAdmin,
                                 ModuleSectionState::Unavailable}) {
        if (*ModuleSectionText(s) == L'\0') { *err = L"章节文案为空"; return false; }
    }
    if (ModuleBadgeForSig(-1) != ModuleBadge::Pending) { *err = L"进行中校验应为 Pending"; return false; }
    if (ModuleBadgeForSig(static_cast<int>(ops::SigState::Valid)) != ModuleBadge::Valid) {
        *err = L"Valid 映射失败";
        return false;
    }
    if (ModuleBadgeForSig(static_cast<int>(ops::SigState::Unsigned)) != ModuleBadge::Unsigned) {
        *err = L"Unsigned 映射失败";
        return false;
    }
    if (ModuleBadgeForSig(static_cast<int>(ops::SigState::Invalid)) != ModuleBadge::Invalid) {
        *err = L"Invalid 映射失败";
        return false;
    }
    for (ModuleBadge b : {ModuleBadge::Pending, ModuleBadge::Valid, ModuleBadge::Unsigned,
                          ModuleBadge::Invalid, ModuleBadge::Unknown}) {
        if (*ModuleBadgeLabel(b) == L'\0') { *err = L"徽标文案为空"; return false; }
    }
    return true;
}

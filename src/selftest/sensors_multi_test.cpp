// sensors_multi_test：G-B "同类传感器多值展示" 用例（契约 Sensors.h，G-B
// 扩展）。重点：同一传感器类别有多个实例/来源时，每实例产出
// 一条读数——绝不聚合。断言遵循本机的实测行为
//（非管理员机器、root\WMI 可达、无 nvml.dll、一块物理盘），
// 在其他环境退化为诚实的兜底分支。
//
// 本机实测基线 (2026-09-18, non-admin):
//   MSAcpi_ThermalZoneTemperature  -> 拒绝访问  => NeedAdmin 条目集合
//   nvml.dll                       -> absent    => snap.gpu 空 + notes 提及 NVML
//   MSFT_PhysicalDisk              -> 1 块 (HYS512, SATA)
#include "selftest/TestFramework.h"
#include "collect/Sensors.h"
#include "core/Str.h"
#include <objbase.h>
#include <wbemidl.h>
#include <windows.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "oleaut32")  // 仅 selftest：DPTF 探测用 SysAllocString

namespace {

using State = stm::SensorReading::State;

bool IsKnownState(State s) {
    return s == State::Ok || s == State::NeedAdmin || s == State::NeedDriver ||
           s == State::NoHardware;
}

// 每盘属性的独立判据：OS 当前暴露多少块物理盘
//（与 ReadDisks 相同的探测、相同的权限）。
unsigned CountPhysicalDrives() {
    unsigned n = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        const std::wstring path = stm::Fmt(L"\\\\.\\PhysicalDrive{}", i);
        const HANDLE h = ::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
            ++n;
        }
    }
    return n;
}

// P2 DPTF 探测的独立判据：厂商命名空间是否安装。
// 镜像 collect/Sensors.cpp 的本地 GUID 模式（不链接 wbemuuid.lib）；
// 纯可达性问题，这里不读任何数据。
bool WmiNamespaceReachable(const wchar_t* ns) {
    constexpr GUID kProbeCLSID_WbemLocator = {
        0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
    constexpr GUID kProbeIID_IWbemLocator = {
        0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
    const HRESULT ci = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) return false;
    struct CoGuard {
        HRESULT hr;
        ~CoGuard() {
            if (SUCCEEDED(hr)) ::CoUninitialize();
        }
    } guard{ci};
    IWbemLocator* loc = nullptr;
    const HRESULT hr = ::CoCreateInstance(kProbeCLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                          kProbeIID_IWbemLocator,
                                          reinterpret_cast<void**>(&loc));
    if (FAILED(hr) || loc == nullptr) return false;
    BSTR bns = ::SysAllocString(ns);
    IWbemServices* svc = nullptr;
    const HRESULT cs = loc->ConnectServer(bns, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                                          &svc);
    if (bns != nullptr) ::SysFreeString(bns);
    loc->Release();
    if (svc != nullptr) svc->Release();
    return cs == S_OK;  // 其他（含 WBEM_E_INVALID_NAMESPACE）= 缺失
}

}  // namespace

// ---------------------------------------------------------------------------
// sensors_zones_multi_label：MSAcpi_ThermalZoneTemperature 是实例集——
// 每热区一条、标签两两不同。非管理员（本机）：集合
// 是 NeedAdmin 条目集且非空；若枚举本身失败，
// 也必须存在一条 NeedAdmin 条目（绝不静默缺位）。
// ---------------------------------------------------------------------------
STM_TEST(sensors_zones_multi_label) {
    std::wstring serr;
    const stm::SensorSnapshot snap = stm::ReadSensors(&serr);

    std::vector<const stm::SensorReading*> zones;
    for (const stm::SensorReading& r : snap.cpu) {
        if (r.label.find(L"ACPI 热区") != std::wstring::npos) zones.push_back(&r);
    }
    if (zones.empty()) {
        *err = L"无任何 ACPI 热区条目（存在热区实例或需管理员权限，均应有条目）";
        return false;
    }
    for (const stm::SensorReading* r : zones) {
        if (!IsKnownState(r->state)) {
            *err = L"ACPI 热区条目出现未知状态";
            return false;
        }
    }
    // 各条目 label 两两不同——"同类多值逐项展示"的最小可展示性要求。
    for (size_t i = 0; i < zones.size(); ++i) {
        for (size_t j = i + 1; j < zones.size(); ++j) {
            if (zones[i]->label == zones[j]->label) {
                *err = stm::Fmt(L"ACPI 热区 label 重复：{}", zones[i]->label);
                return false;
            }
        }
    }
    // 任何 Ok 读数必须是合理温度，绝不是假 0。
    bool anyOk = false;
    for (const stm::SensorReading* r : zones) {
        if (r->state == State::Ok) {
            anyOk = true;
            if (r->unit != L"°C" || r->value < -60.0 || r->value > 250.0) {
                *err = stm::Fmt(L"ACPI 热区 Ok 读数非法：{} {}{}°C", r->label, r->value, r->unit);
                return false;
            }
        }
    }
    if (!anyOk) {
        // 本机实测（非管理员，root\WMI 可达）：整组为 NeedAdmin；若本机枚举
        // 失败（命名空间拒绝），至少要有一条 NeedAdmin，而不是没有条目。
        bool needAdmin = false;
        for (const stm::SensorReading* r : zones) {
            if (r->state == State::NeedAdmin) needAdmin = true;
        }
        if (!needAdmin) {
            *err = L"热区未读到 Ok 值且无 NeedAdmin 条目（非管理员下应如实呈现 NeedAdmin）";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// sensors_gpu_expanded：NVML 路径每卡展开为每传感器读数
//（温度/减速阈值/功耗/GPU+显存利用率/风扇，source = L"NVML"）。
// 无 NVML 环境（本机）：NVML 组为空且 notes 仍诚实提及
// NVML——测试在两种环境中都不得失败。
// ---------------------------------------------------------------------------
STM_TEST(sensors_gpu_expanded) {
    std::wstring serr;
    const stm::SensorSnapshot snap = stm::ReadSensors(&serr);

    std::vector<const stm::SensorReading*> nvml;
    for (const std::vector<stm::SensorReading>* v : {&snap.gpu, &snap.gpus}) {
        for (const stm::SensorReading& r : *v) {
            if (r.source == L"NVML") nvml.push_back(&r);
        }
    }

    if (nvml.empty()) {
        // 无 NVML 环境（本机实测）：NVML 读数组为空 + notes 诚实提及 NVML。
        if (!snap.gpu.empty()) {
            *err = L"无 NVML 时 gpu 组应为空";
            return false;
        }
        if (snap.notes.find(L"NVML") == std::wstring::npos) {
            *err = L"无 NVML 时 notes 未提及 NVML（缺少诚实说明）";
            return false;
        }
        for (const stm::SensorReading& r : snap.gpus) {
            if (!IsKnownState(r.state)) {
                *err = L"gpus 出现未知状态";
                return false;
            }
        }
        return true;
    }

    // NVML 环境：标签唯一、数值合理、无 Ok+0 伪造。
    std::set<int> cards;                 // "GPU <n> ..." 中见到的每卡索引
    std::set<std::wstring> labelsSeen;
    bool cardHasTemp = false;
    for (const stm::SensorReading* r : nvml) {
        if (r->state != State::Ok) {
            *err = L"NVML 读数只应产生 Ok（读不到就不生成条目）";
            return false;
        }
        if (!labelsSeen.insert(r->label).second) {
            *err = stm::Fmt(L"NVML label 重复：{}", r->label);
            return false;
        }
        const size_t digitPos = r->label.find(L"GPU ") + 4;
        if (digitPos < r->label.size() && r->label[digitPos] >= L'0' &&
            r->label[digitPos] <= L'9') {
            cards.insert(r->label[digitPos] - L'0');
            if (r->label.find(L"温度") != std::wstring::npos &&
                r->label.find(L"阈值") == std::wstring::npos) {
                cardHasTemp = true;
                if (r->unit != L"°C" || r->value <= 0.0 || r->value > 150.0) {
                    *err = stm::Fmt(L"GPU 温度非法：{} = {}{}", r->label, r->value, r->unit);
                    return false;
                }
            }
        }
        if (r->unit == L"%" && (r->value < 0.0 || r->value > 100.0)) {
            *err = stm::Fmt(L"GPU 百分比超界：{} = {}", r->label, r->value);
            return false;
        }
        if (r->unit == L"W" && r->value <= 0.0) {
            *err = stm::Fmt(L"GPU 功耗非法：{} = {} W", r->label, r->value);
            return false;
        }
    }
    // 每张枚举到的卡至少报告其温度传感器。
    if (cardHasTemp != (cards.size() > 0) || nvml.size() < cards.size()) {
        *err = L"NVML 每卡展开不完整（每卡至少应有温度读数）";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// sensors_disks_per_disk：磁盘每物理盘一行（绝不聚合）；
// 每个型号非空；多盘 -> 型号两两不同。
// 行数与独立的 OS 探测核对。
// ---------------------------------------------------------------------------
STM_TEST(sensors_disks_per_disk) {
    std::wstring serr;
    const stm::SensorSnapshot snap = stm::ReadSensors(&serr);

    const unsigned drives = CountPhysicalDrives();
    if (drives == 0) {
        *err = L"探针未发现物理盘（测试机应至少有一块）";
        return false;
    }
    if (snap.disks.size() != drives) {
        *err = stm::Fmt(L"磁盘行数 {} != 物理盘数 {}（存在聚合或遗漏）", snap.disks.size(),
                        drives);
        return false;
    }
    for (const stm::DiskHealth& d : snap.disks) {
        if (d.model.empty()) {
            *err = L"磁盘条目 model 为空";
            return false;
        }
        if (d.tempState != State::Ok && d.tempState != State::NeedAdmin &&
            d.tempState != State::NoHardware) {
            *err = L"磁盘温度状态超出诚实三态";
            return false;
        }
        if (d.tempState == State::Ok && d.tempC <= 0.0) {
            *err = L"磁盘温度出现 Ok 且 <=0（假数据）";
            return false;
        }
        if (d.tempState != State::Ok && d.tempC != 0.0) {
            *err = L"磁盘温度非 Ok 却带非 0 数值";
            return false;
        }
    }
    if (snap.disks.size() > 1) {
        for (size_t i = 0; i < snap.disks.size(); ++i) {
            for (size_t j = i + 1; j < snap.disks.size(); ++j) {
                if (snap.disks[i].model == snap.disks[j].model) {
                    *err = stm::Fmt(L"多盘时 model 重复：{}", snap.disks[i].model);
                    return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// sensors_temp_sources_labeled（P2，"CPU 温度信息增强"）：每个用户态
// 热源每实例产出一条读数，且每条温度标签
// carries its provenance prefix — "ACPI 热区" (cpu group), "WMI 温度" /
// "WMI 热区计数器" / "…（WMI SMART）" / "DPTF 温度" (extra). Labels are
// 在并集上两两不同，两个提供程序绝不可能合并成一行。
// 非管理员机器（实测基线）：没有任何 Ok 时，至少必须存在
// 一条 NeedAdmin 条目——绝不静默缺位。
// D6 扩展（2026-09-20）：认可新的只读内核来源前缀——"DTS"/"PawnIO"
//（每核 DTS 温度经官方 PawnIO 模块，label "CPU 核心 N（DTS）"，见
// collect/PawnIoLink.h）；不变量本身（来源前缀 + 两两不同）不变。
// ---------------------------------------------------------------------------
STM_TEST(sensors_temp_sources_labeled) {
    std::wstring serr;
    const stm::SensorSnapshot snap = stm::ReadSensors(&serr);

    auto hasProvenance = [](const std::wstring& label) {
        for (const wchar_t* mark : {L"ACPI 热区", L"WMI", L"DPTF", L"DTS", L"PawnIO"}) {
            if (label.find(mark) != std::wstring::npos) return true;
        }
        return false;
    };

    std::vector<const stm::SensorReading*> temps;
    for (const std::vector<stm::SensorReading>* group : {&snap.cpu, &snap.extra}) {
        for (const stm::SensorReading& r : *group) {
            if (r.unit != L"°C") continue;  // 只看温度读数
            if (!hasProvenance(r.label)) {
                *err = stm::Fmt(L"温度条目缺少来源前缀：{}", r.label);
                return false;
            }
            if (!IsKnownState(r.state)) {
                *err = stm::Fmt(L"温度条目状态未知：{}", r.label);
                return false;
            }
            temps.push_back(&r);
        }
    }
    for (size_t i = 0; i < temps.size(); ++i) {
        for (size_t j = i + 1; j < temps.size(); ++j) {
            if (temps[i]->label == temps[j]->label) {
                *err = stm::Fmt(L"温度条目 label 跨来源重复：{}", temps[i]->label);
                return false;
            }
        }
    }
    bool anyOk = false, anyNeedAdmin = false;
    for (const stm::SensorReading* r : temps) {
        if (r->state == State::Ok) {
            anyOk = true;
            if (r->value < -60.0 || r->value > 250.0) {
                *err = stm::Fmt(L"温度 Ok 读数越界：{} = {}°C", r->label, r->value);
                return false;
            }
        }
        if (r->state == State::NeedAdmin) anyNeedAdmin = true;
    }
    if (!anyOk && !anyNeedAdmin) {
        *err = L"无 Ok 温度读数（非管理员常态）且无 NeedAdmin 条目（诚实回退缺失）";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// sensors_dptf_silent_when_absent（P2）：Intel DPTF 依赖驱动。当其
// WMI 命名空间未安装（常见的台式机情形）时，快照必须保持
// 沉默——任何地方都不得出现 DPTF 标签的读数，
// 因为缺失不作任何承诺。命名空间已安装时，每条 DPTF 读数
// 必须为 Ok + 合理 + 带 source=DPTF 标记（采集器只推送这种行；
// 被拒时不产出任何内容，而不是占位符）。
// ---------------------------------------------------------------------------
STM_TEST(sensors_dptf_silent_when_absent) {
    std::wstring serr;
    const stm::SensorSnapshot snap = stm::ReadSensors(&serr);

    std::vector<const stm::SensorReading*> dptf;
    for (const std::vector<stm::SensorReading>* group : {&snap.cpu, &snap.extra}) {
        for (const stm::SensorReading& r : *group) {
            if (r.label.find(L"DPTF") != std::wstring::npos) dptf.push_back(&r);
        }
    }

    const bool nsPresent = WmiNamespaceReachable(L"ROOT\\Intel_DPTF") ||
                           WmiNamespaceReachable(L"ROOT\\Intel(DPTF)");
    if (!nsPresent && !dptf.empty()) {
        *err = stm::Fmt(L"DPTF 命名空间不存在却出现 {} 条 DPTF 读数（违反静默跳过）",
                        dptf.size());
        return false;
    }
    for (const stm::SensorReading* r : dptf) {
        if (r->state != State::Ok || r->unit != L"°C" || r->value < -60.0 ||
            r->value > 250.0) {
            *err = stm::Fmt(L"DPTF 读数只应 Ok 且温度合理：{} = {}{}（状态 {}）", r->label,
                            r->value, r->unit, static_cast<int>(r->state));
            return false;
        }
        if (r->source != L"DPTF") {
            *err = stm::Fmt(L"DPTF 读数缺少 source 标记：{}", r->label);
            return false;
        }
    }
    return true;
}

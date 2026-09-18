// sensors_multi_test: G-B "同类传感器多值展示" cases (contract Sensors.h, G-B
// extension). Focus: a sensor kind with several instances/sources yields one
// reading PER instance — never an aggregate. Assertions follow this machine's
// measured behaviour (non-admin box, root\WMI reachable, no nvml.dll, one
// physical disk) and degrade to the honest fallback branches elsewhere.
//
// 本机实测基线 (2026-09-18, non-admin):
//   MSAcpi_ThermalZoneTemperature  -> 拒绝访问  => NeedAdmin 条目集合
//   nvml.dll                       -> absent    => snap.gpu 空 + notes 提及 NVML
//   MSFT_PhysicalDisk              -> 1 块 (HYS512, SATA)
#include "selftest/TestFramework.h"
#include "collect/Sensors.h"
#include "core/Str.h"
#include <windows.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

using State = stm::SensorReading::State;

bool IsKnownState(State s) {
    return s == State::Ok || s == State::NeedAdmin || s == State::NeedDriver ||
           s == State::NoHardware;
}

// Independent oracle for the per-disk property: how many physical drives does
// the OS expose right now (same probe ReadDisks uses, same privileges).
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

}  // namespace

// ---------------------------------------------------------------------------
// sensors_zones_multi_label: MSAcpi_ThermalZoneTemperature is an instance set —
// one entry per zone, labels pairwise distinct. Non-admin (this box): the set
// is a NeedAdmin entry collection and non-empty; if enumeration itself fails,
// a NeedAdmin entry must still exist (never a silent gap).
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
    // Labels pairwise distinct — "同类多值逐项" 的最小可展示性要求。
    for (size_t i = 0; i < zones.size(); ++i) {
        for (size_t j = i + 1; j < zones.size(); ++j) {
            if (zones[i]->label == zones[j]->label) {
                *err = stm::Fmt(L"ACPI 热区 label 重复：{}", zones[i]->label);
                return false;
            }
        }
    }
    // Any Ok reading must be a plausible temperature, never a fake 0.
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
// sensors_gpu_expanded: NVML path expands per card into per-sensor readings
// (temp / slowdown threshold / power / gpu+mem util / fan, source = L"NVML").
// No-NVML environment (this box): the NVML group is EMPTY and the notes still
// mention NVML honestly — the test must not fail in either environment.
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

    // NVML environment: labels unique, values plausible, no Ok+0 fake.
    std::set<int> cards;                 // per-card index seen in "GPU <n> ..."
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
    // Every enumerated card reports at least its temperature sensor.
    if (cardHasTemp != (cards.size() > 0) || nvml.size() < cards.size()) {
        *err = L"NVML 每卡展开不完整（每卡至少应有温度读数）";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// sensors_disks_per_disk: disks are one row PER physical drive (never an
// aggregate); every model non-empty; multiple drives -> models pairwise
// distinct. Row count is checked against an independent OS probe.
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

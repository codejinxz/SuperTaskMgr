#pragma once
// Header-only pure-logic helpers for the phase-3 pages (network / startup /
// services / drivers / sensors). Shared between the UI layer (Pages3.cpp) and
// the selftest (ui3_test.cpp); intentionally free of ImGui and app objects so
// stm_selftest can include it while linking only core+collect+ops.
// All user-facing strings are Chinese and returned as wide strings; the UI
// converts them through ui::U8() at the call site.
#include <string>
#include <algorithm>
#include <cwctype>
#include "collect/NetTables.h"
#include "core/Cfg.h"
#include "core/Str.h"
#include "ops/ServiceOps.h"
#include "ops/StartupOps.h"

namespace stm {
namespace ui3 {

// ---------------------------------------------------------------------------
// Services: SERVICE_STATE (winsvc.h values 1..7) -> Chinese short label.
// ---------------------------------------------------------------------------
inline std::wstring ServiceStateLabel(uint32_t state) {
    switch (state) {
        case 1: return L"已停止";    // SERVICE_STOPPED
        case 2: return L"正在启动";  // SERVICE_START_PENDING
        case 3: return L"正在停止";  // SERVICE_STOP_PENDING
        case 4: return L"正在运行";  // SERVICE_RUNNING
        case 5: return L"正在恢复";  // SERVICE_CONTINUE_PENDING
        case 6: return L"正在暂停";  // SERVICE_PAUSE_PENDING
        case 7: return L"已暂停";    // SERVICE_PAUSED
        default: return L"未知";
    }
}

// ---------------------------------------------------------------------------
// Services: start type (SERVICE_*_START constants 0..4) -> Chinese label.
// ---------------------------------------------------------------------------
inline std::wstring ServiceStartTypeLabel(uint32_t startType) {
    switch (startType) {
        case 0: return L"引导启动";  // SERVICE_BOOT_START
        case 1: return L"系统启动";  // SERVICE_SYSTEM_START
        case 2: return L"自动";      // SERVICE_AUTO_START
        case 3: return L"手动";      // SERVICE_DEMAND_START
        case 4: return L"禁用";      // SERVICE_DISABLED
        default: return L"未知";
    }
}

// ---------------------------------------------------------------------------
// Startup items: source enum -> Chinese label (table column 来源).
// ---------------------------------------------------------------------------
inline std::wstring StartupSourceLabel(ops::StartupSource source) {
    switch (source) {
        case ops::StartupSource::RegRun:        return L"注册表";
        case ops::StartupSource::RegRun32:      return L"注册表（32 位）";
        case ops::StartupSource::StartupFolder: return L"启动文件夹";
        case ops::StartupSource::ScheduledTask: return L"计划任务";
        case ops::StartupSource::UwpStartupTask: return L"UWP";
        default: return L"未知";
    }
}

// A startup item may only be toggled without elevation when the enumerator
// marked it canToggle (HKCU writable entries); everything else needs admin.
inline bool StartupNeedsElevation(const ops::StartupItem& item, bool elevated) {
    return !elevated && !item.canToggle;
}

// ---------------------------------------------------------------------------
// Connections: thin wrapper over the contract's TcpStateLabel. Guarantees a
// non-empty display value: UDP rows carry state 0 and the contract maps
// unknown states to hex, so an empty result degrades to the em dash.
// ---------------------------------------------------------------------------
inline std::wstring UiTcpStateLabel(uint32_t state) {
    std::wstring label = TcpStateLabel(state);
    return label.empty() ? std::wstring(L"—") : label;
}

// "addr:port"; an empty address (UDP remote) renders as the em dash.
inline std::wstring ConnEndpoint(const std::wstring& addr, uint16_t port) {
    if (addr.empty()) return L"—";
    return addr + L":" + std::to_wstring(port);
}

// ---------------------------------------------------------------------------
// Drivers: the contract returns the literal error 需要管理员权限 on 24H2+
// without elevation; the page degrades to a full-page notice in that case.
// ---------------------------------------------------------------------------
inline bool DriverErrNeedsAdmin(const std::wstring& err) {
    return err.find(L"需要管理员权限") != std::wstring::npos;
}

// ---------------------------------------------------------------------------
// Sensor page (W2 redesign): per-group visibility, persisted in the config.
// Pure helpers so stm_selftest can cover keys/defaults without a GUI.
// ---------------------------------------------------------------------------
enum class SensorGroup { Cpu, Gpu, Mem, Disk, Net, Battery, Fan, Extra, Count };

inline const wchar_t* SensorGroupCfgKey(SensorGroup g) {
    switch (g) {
        case SensorGroup::Cpu: return L"sensShowCpu";
        case SensorGroup::Gpu: return L"sensShowGpu";
        case SensorGroup::Mem: return L"sensShowMem";
        case SensorGroup::Disk: return L"sensShowDisk";
        case SensorGroup::Net: return L"sensShowNet";
        case SensorGroup::Battery: return L"sensShowBattery";
        case SensorGroup::Fan: return L"sensShowFan";
        case SensorGroup::Extra: return L"sensShowExtra";
        default: return L"";
    }
}

// Product default: every group visible except 风扇 (honest NeedDriver-only data).
inline bool SensorGroupDefaultVisible(SensorGroup g) {
    return g != SensorGroup::Fan;
}

inline bool SensorGroupVisible(const Config& cfg, SensorGroup g) {
    return cfg.GetBool(SensorGroupCfgKey(g), SensorGroupDefaultVisible(g));
}

inline const wchar_t* SensorGroupTitle(SensorGroup g) {
    switch (g) {
        case SensorGroup::Cpu: return L"CPU";
        case SensorGroup::Gpu: return L"GPU";
        case SensorGroup::Mem: return L"内存";
        case SensorGroup::Disk: return L"磁盘";
        case SensorGroup::Net: return L"网络";
        case SensorGroup::Battery: return L"电池";
        case SensorGroup::Fan: return L"风扇";
        case SensorGroup::Extra: return L"其他";
        default: return L"";
    }
}

// Classifies a LibreHardwareMonitor reading (label = its node path, already
// suffixed with ［LHM］) into the sensor group it merges into. Case-insensitive
// keyword scan, ordered so unambiguous keywords win (fan before disk etc.).
enum class LhmGroup { Other, Cpu, Gpu, Mem, Disk, Net, Battery, Fan };
inline LhmGroup LhmGroupOf(const std::wstring& lhmLabel) {
    std::wstring s(lhmLabel.size(), L'\0');
    std::transform(lhmLabel.begin(), lhmLabel.end(), s.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    auto has = [&s](const wchar_t* kw) { return s.find(kw) != std::wstring::npos; };
    if (has(L"fan") || has(L"control")) return LhmGroup::Fan;
    if (has(L"battery")) return LhmGroup::Battery;
    if (has(L"gpu")) return LhmGroup::Gpu;
    if (has(L"cpu")) return LhmGroup::Cpu;
    if (has(L"ram") || has(L"memory") || has(L"mem")) return LhmGroup::Mem;
    if (has(L"hdd") || has(L"ssd") || has(L"nvme") || has(L"harddisk") || has(L"disk") ||
        has(L"storage") || has(L"smart")) {
        return LhmGroup::Disk;
    }
    if (has(L"nic") || has(L"ethernet") || has(L"network") || has(L"wifi")) return LhmGroup::Net;
    return LhmGroup::Other;
}

// Extracts the core index from per-core CPU sensor labels produced by the
// collector ("CPU 核 N 频率" / "CPU 核 N 占用率"); -1 = aggregate/other row.
// isFreqOut reports whether the row is a frequency (MHz) or an utilization (%).
inline int SensorCoreIndex(const std::wstring& label, bool* isFreqOut) {
    if (isFreqOut) *isFreqOut = label.find(L"频率") != std::wstring::npos;
    const size_t at = label.find(L"核");
    if (at == std::wstring::npos) return -1;
    size_t i = at + 1;
    while (i < label.size() && (label[i] == L' ' || label[i] == L'#')) ++i;
    if (i >= label.size() || label[i] < L'0' || label[i] > L'9') return -1;
    int n = 0;
    while (i < label.size() && label[i] >= L'0' && label[i] <= L'9') {
        n = n * 10 + (label[i] - L'0');
        ++i;
    }
    return n;
}

}  // namespace ui3
}  // namespace stm

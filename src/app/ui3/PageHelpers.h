#pragma once
// Header-only pure-logic helpers for the phase-3 pages (network / startup /
// services / drivers / sensors). Shared between the UI layer (Pages3.cpp) and
// the selftest (ui3_test.cpp); intentionally free of ImGui and app objects so
// stm_selftest can include it while linking only core+collect+ops.
// All user-facing strings are Chinese and returned as wide strings; the UI
// converts them through ui::U8() at the call site.
#include <string>
#include "collect/NetTables.h"
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

}  // namespace ui3
}  // namespace stm

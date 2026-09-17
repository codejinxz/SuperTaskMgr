// Phase-3 UI helper tests. PageHelpers.h is header-only and depends only on
// core/collect/ops headers, so these run inside stm_selftest which links
// core+collect+ops (no app objects / no ImGui).
#include "selftest/TestFramework.h"
#include "app/ui3/PageHelpers.h"

using stm::ops::StartupItem;
using stm::ops::StartupSource;

// SERVICE_STATE (1..7) maps to the documented Chinese labels; unknown stays honest.
STM_TEST(ui3_service_state_labels) {
    if (stm::ui3::ServiceStateLabel(1) != L"已停止") {
        *err = L"SERVICE_STOPPED 应映射为 已停止";
        return false;
    }
    if (stm::ui3::ServiceStateLabel(4) != L"正在运行") {
        *err = L"SERVICE_RUNNING 应映射为 正在运行";
        return false;
    }
    if (stm::ui3::ServiceStateLabel(2) != L"正在启动" ||
        stm::ui3::ServiceStateLabel(7) != L"已暂停") {
        *err = L"SERVICE_START_PENDING/PAUSED 映射错误";
        return false;
    }
    if (stm::ui3::ServiceStateLabel(99) != L"未知" || stm::ui3::ServiceStateLabel(0) != L"未知") {
        *err = L"未知状态应映射为 未知";
        return false;
    }
    return true;
}

// Start type (SERVICE_*_START) mapping used by the services page column.
STM_TEST(ui3_service_starttype_labels) {
    if (stm::ui3::ServiceStartTypeLabel(2) != L"自动" ||
        stm::ui3::ServiceStartTypeLabel(3) != L"手动") {
        *err = L"AUTO/DEMAND 启动类型映射错误";
        return false;
    }
    if (stm::ui3::ServiceStartTypeLabel(4) != L"禁用") {
        *err = L"SERVICE_DISABLED 应映射为 禁用";
        return false;
    }
    return true;
}

// Startup item source -> Chinese label (table column 来源).
STM_TEST(ui3_startup_source_labels) {
    if (stm::ui3::StartupSourceLabel(StartupSource::RegRun) != L"注册表") {
        *err = L"RegRun 应映射为 注册表";
        return false;
    }
    if (stm::ui3::StartupSourceLabel(StartupSource::StartupFolder) != L"启动文件夹") {
        *err = L"StartupFolder 应映射为 启动文件夹";
        return false;
    }
    if (stm::ui3::StartupSourceLabel(StartupSource::ScheduledTask) != L"计划任务") {
        *err = L"ScheduledTask 应映射为 计划任务";
        return false;
    }
    if (stm::ui3::StartupSourceLabel(StartupSource::UwpStartupTask) != L"UWP") {
        *err = L"UwpStartupTask 应映射为 UWP";
        return false;
    }
    return true;
}

// The TcpStateLabel wrapper must delegate to the contract function verbatim
// (contract: unknown states -> hex, so state 0 is NOT empty) and must never
// return an empty string for a known value. UDP rows render "—" in the page
// itself without consulting the wrapper.
STM_TEST(ui3_tcp_state_wrapper) {
    if (stm::ui3::UiTcpStateLabel(2) != stm::TcpStateLabel(2) ||
        stm::ui3::UiTcpStateLabel(2).empty()) {
        *err = L"包装器未正确透传 TcpStateLabel(2)";
        return false;
    }
    if (stm::ui3::UiTcpStateLabel(5) != stm::TcpStateLabel(5) ||
        stm::ui3::UiTcpStateLabel(5).empty()) {
        *err = L"包装器未正确透传 TcpStateLabel(5)";
        return false;
    }
    if (stm::ui3::UiTcpStateLabel(0) != stm::TcpStateLabel(0) ||
        stm::ui3::UiTcpStateLabel(0).empty()) {
        *err = L"未知状态应按契约透传（十六进制回退），不得为空";
        return false;
    }
    return true;
}

// Non-elevated users may only toggle items the enumerator marked canToggle.
STM_TEST(ui3_startup_elevation_gate) {
    StartupItem item;
    item.canToggle = false;
    if (!stm::ui3::StartupNeedsElevation(item, false)) {
        *err = L"canToggle=false 且未提权时应提示需提权";
        return false;
    }
    if (stm::ui3::StartupNeedsElevation(item, true)) {
        *err = L"已提权时不应提示需提权";
        return false;
    }
    item.canToggle = true;
    if (stm::ui3::StartupNeedsElevation(item, false)) {
        *err = L"canToggle=true 的项目（HKCU）未提权也应可操作";
        return false;
    }
    return true;
}

// Driver page degrade trigger: only the documented contract error unlocks the
// full-page elevate notice.
STM_TEST(ui3_driver_err_needs_admin) {
    if (!stm::ui3::DriverErrNeedsAdmin(L"需要管理员权限")) {
        *err = L"契约错误串应触发降级页";
        return false;
    }
    if (stm::ui3::DriverErrNeedsAdmin(L"") || stm::ui3::DriverErrNeedsAdmin(L"其他错误")) {
        *err = L"普通错误不应触发驱动页降级";
        return false;
    }
    return true;
}

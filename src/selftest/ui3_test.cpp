// 第 3 阶段 UI 辅助测试。PageHelpers.h 仅头文件且只依赖
// core/collect/ops 头，因此这些测试在 stm_selftest 内运行
//（链接 core+collect+ops，无应用对象/无 ImGui）。
#include "selftest/TestFramework.h"
#include "app/ui3/PageHelpers.h"

using stm::ops::StartupItem;
using stm::ops::StartupSource;

// SERVICE_STATE（1..7）映射到有文档的中文标签；未知保持诚实。
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

// 服务页列使用的启动类型（SERVICE_*_START）映射。
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

// 启动项来源 → 中文标签（表格"来源"列）。
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

// TcpStateLabel 包装必须原样委托给契约函数
//（契约：未知状态 -> 十六进制，因此状态 0 不是空），
// 且对已知值绝不返回空字符串。UDP 行在页面里渲染 "—"
// 而不经由该包装。
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

// 未提权用户只能切换枚举器标记 canToggle 的条目。
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

// 驱动页降级触发：只有有文档的契约错误才解锁整页
// 提权提示。
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

#pragma once
// 第 3 阶段页面（网络/启动项/服务/驱动/传感器）的仅头文件
// 纯逻辑辅助。UI 层（Pages3.cpp）与 selftest（ui3_test.cpp）共享；
// 刻意不依赖 ImGui 与应用对象，使 stm_selftest 只链接
// core+collect+ops 即可包含它。
// 所有面向用户的字符串都是中文并以宽字符串返回；UI 在调用点
// 经 ui::U8() 转换。
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
// 服务：SERVICE_STATE（winsvc.h 值 1..7）-> 中文短标签。
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
// 服务：启动类型（SERVICE_*_START 常量 0..4）-> 中文标签。
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
// 启动项：来源枚举 -> 中文标签（表格列“来源”）。
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

// 只有枚举器标记 canToggle（HKCU 可写项）的启动项才能在未提权时
// 切换；其余一律需要管理员。
inline bool StartupNeedsElevation(const ops::StartupItem& item, bool elevated) {
    return !elevated && !item.canToggle;
}

// ---------------------------------------------------------------------------
// 连接：契约 TcpStateLabel 的薄包装。保证展示值非空：
// UDP 行状态为 0，契约把未知状态映射为十六进制，
// 空结果退化为破折号。
// ---------------------------------------------------------------------------
inline std::wstring UiTcpStateLabel(uint32_t state) {
    std::wstring label = TcpStateLabel(state);
    return label.empty() ? std::wstring(L"—") : label;
}

// "addr:port"；空地址（UDP 远端）渲染为破折号。
inline std::wstring ConnEndpoint(const std::wstring& addr, uint16_t port) {
    if (addr.empty()) return L"—";
    return addr + L":" + std::to_wstring(port);
}

// ---------------------------------------------------------------------------
// Drivers: the contract returns the literal error 需要管理员权限 on 24H2+
// 未提权时；该情形下页面退化为整页提示。
// ---------------------------------------------------------------------------
inline bool DriverErrNeedsAdmin(const std::wstring& err) {
    return err.find(L"需要管理员权限") != std::wstring::npos;
}

// ---------------------------------------------------------------------------
// 传感器页（W2 重设计）：分组可见性，持久化于配置。
// 纯辅助函数，stm_selftest 无 GUI 即可覆盖键/默认值。
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

// 把 LibreHardwareMonitor 读数（label = 其节点路径，已带
// ［LHM］ 后缀）归类到它应并入的传感器组。大小写不敏感
// 关键字扫描，顺序保证无歧义关键字优先（风扇先于磁盘等）。
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

// 从采集器产出的每核 CPU 传感器标签中提取核心索引
// collector ("CPU 核 N 频率" / "CPU 核 N 占用率"); -1 = aggregate/other row.
// isFreqOut 报告该行是频率（MHz）还是利用率（%）。
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

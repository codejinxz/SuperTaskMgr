#pragma once
// 第 3 阶段契约：硬件传感器，每条读数诚实三分类（架构 §8 / R6）：
//   State::Ok         — 来自有文档的用户态来源的真实值
//   State::NeedAdmin  — 来源存在但需要提权（ACPI 热区、SMART）
//   State::NeedDriver — 用户态完全不可达（风扇转速、每核 CPU 温度）
//   State::NoHardware — 适配器/磁盘存在但该传感器缺失
// 绝不用 0 顶替缺失数据。本应用不附带任何内核驱动。
//
// F3 扩展（2026-09-18，架构师批准）：仅做增量。新增成员为
// cpuCores/gpus/network/battery/memory 向量、double uptimeSec，以及下方
// DiskHealth 的新字段（sparePct..critWarnValid）。既有成员保持
// 语义不变；四态诚实模型同样约束每条新读数
//（Ok = 有文档的用户态来源：PDH / GetIfTable2 / CallNtPowerInformation /
// GlobalMemoryStatusEx / GetPerformanceInfo / NVML；可选的外部
// LibreHardwareMonitor 源位于 collect/LhmSource.h，默认关闭）。
//
// G-B 扩展（2026-09-18，"同类传感器多值展示"）：同样仅做增量。新增
// 成员为 SensorReading::source 与 SensorSnapshot::extra。所有既有成员
// 语义不变；四态模型同样约束新读数。多值规则：同一传感器类别若有多个
// 实例/来源（ACPI 热区、NVML GPU、按引擎的 GPU 引擎、磁盘、网卡），
// 则每个实例产出一条读数——绝不以一个聚合值顶替整组。
//
//
// P2 扩展（2026-09-18，"CPU 温度信息增强"；仅增量，不改结构）：
// SensorSnapshot::extra 还承载其余用户态热源，每个实例一条读数，
// 每条标签带来源前缀——
//   "WMI 温度 N/（实例）"      Win32_Temperature (ROOT\CIMV2)
//   "WMI 热区计数器 N/（实例）" Win32_PerfFormattedData_Counters_ThermalZoneInformation
//   "DPTF 温度（参与者）"      Intel DPTF TEMPERATURE set (root\Intel_DPTF, fallback
//                              root\Intel(DPTF))；命名空间缺失 -> 静默跳过
//（ACPI 热区在 `cpu` 中仍用 "ACPI 热区 N/（InstanceName）" 标签。）所有
// 行只出 Ok 且经合理性闸门——失败、需提权或输出垃圾数据的来源
// 一律不产出任何内容。每核 CPU DTS 温度
//（MSR 0x19C/0x1A2/0x1B1）仍属 State::NeedDriver 范畴：需要内核驱动，
// 而本应用绝不附带内核驱动（红线）；传感器页已注明这一点，
// 并改为提供可选的 LibreHardwareMonitor 桥接。
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

struct SensorReading {
    std::wstring label;      // e.g. L"CPU 整包温度（ACPI 热区）"
    double value = 0.0;      // 以 `unit` 为单位
    std::wstring unit;       // L"°C", L"MHz", L"rpm", L"%"
    enum class State { Ok, NeedAdmin, NeedDriver, NoHardware } state = State::Ok;
    // --- G-B 新增（2026-09-18）：数据来源，如 L"NVML"。空 =
    // 本模块内置来源。纯元数据：现有生产者均不设置它，
    // 因此默认构造的读数行为与从前完全一致。
    std::wstring source;
};

struct DiskHealth {
    std::wstring model;        // 友好名称
    std::wstring serial;
    std::wstring busType;      // SATA / NVMe / USB / Unknown（未知）
    std::wstring health;       // "良好" / "警告" / "未知" (honest; MSFT_PhysicalDisk + IOCTL detail)
    double tempC = 0.0;        // NVMe 综合温度 / ATA 温度；NeedAdmin => state
    SensorReading::State tempState = SensorReading::State::NeedAdmin;
    uint64_t powerOnHours = UINT64_MAX;  // UINT64_MAX = 不可得
    // --- F3 新增（2026-09-18）：NVMe SMART/健康日志细节。所有百分比字段
    // 以 UINT32_MAX = 不可得（ATA/USB 盘，或日志不可读——绝不
    // 给假 0）。critWarnBits 仅在 critWarnValid 为 true 时有意义。
    uint32_t sparePct = UINT32_MAX;        // NVMe "Available Spare" 百分比
    uint32_t spareThreshPct = UINT32_MAX;  // NVMe "Available Spare Threshold" 百分比
    uint32_t wearPct = UINT32_MAX;         // NVMe "Percentage Used"（寿命磨损）百分比
    uint8_t critWarnBits = 0;              // NVMe 严重警告位图（log[0]）
    bool critWarnValid = false;            // 仅在 NVMe 日志读取成功后为 true
};

struct SensorSnapshot {
    std::vector<SensorReading> cpu;     // 整包温度（ACPI）、每核频率、使用率
    std::vector<SensorReading> gpu;     // 厂商运行时存在时经 IGCL/NVML 取温度/利用率
    std::vector<DiskHealth> disks;      // 来自 SMART
    std::vector<SensorReading> fans;    // 预期：只有 NeedDriver 条目（诚实）
    std::wstring notes;                 // 页头聚合的诚实性备注
    // --- F3 新增（2026-09-18），全部受四态模型约束 ---
    std::vector<SensorReading> cpuCores;  // 每核频率（MHz，CallNtPowerInformation）
                                          // + 使用率（%，PDH Processor Information）
    std::vector<SensorReading> gpus;      // 每传感器 GPU 扩展（G-B）：NVML 每卡——
                                          // 温度 / 减速阈值 / 功耗 / GPU+显存利用率 /
                                          // 风扇——外加引擎级利用率 3D/Copy/VideoDecode/
                                          // Encode（PDH GPU Engine）与专用/共享 VRAM
                                          //（PDH GPU Adapter Memory）。无 NVML -> 仅 PDH。
    std::vector<SensorReading> network;   // 每适配器收/发 B/s + 链路速度（GetIfTable2）
    std::vector<SensorReading> battery;   // AC/DC、电量百分比、剩余时间（CallNtPowerInformation
                                          // SystemBatteryState）；无电池时为 NoHardware 条目
    std::vector<SensorReading> memory;    // 物理/提交/分页池/非分页池
                                          //（GlobalMemoryStatusEx + GetPerformanceInfo）
    // --- G-B 新增（2026-09-18）：已检测到但不属于上述任何分组的读数
    //（尽力而为：MSAcpi_ThermalZone 之外的 WMI 温度类，如
    // Win32_Temperature / MSStorageDriver_FailurePredictData 磁盘温度）。
    // 适用四态规则；为空表示"未找到"——直接不展示。
    std::vector<SensorReading> extra;
    double uptimeSec = 0.0;               // 系统运行秒数（GetTickCount64）
};

// 阻塞式读取（在 ops 任务队列上运行）。绝不抛异常；允许部分结果。
SensorSnapshot ReadSensors(std::wstring* err);

}  // namespace stm

#pragma once
// Phase-3 contract: hardware sensors, honest trichotomy per reading (arch §8 / R6):
//   State::Ok         — real value from a documented user-mode source
//   State::NeedAdmin  — source exists but requires elevation (ACPI thermal, SMART)
//   State::NeedDriver — not reachable in user mode at all (fan speeds, per-core CPU temp)
//   State::NoHardware — adapter/disk present but this sensor absent
// NEVER report 0 in place of missing data. No kernel driver ships with this app.
//
// F3 extension (2026-09-18, architect-approved): additive only. New members are
// the vectors cpuCores/gpus/network/battery/memory, the double uptimeSec and the
// DiskHealth fields below (sparePct..critWarnValid). Existing members keep their
// exact semantics; the four-state honesty model governs every new reading too
// (Ok = documented user-mode source: PDH / GetIfTable2 / CallNtPowerInformation /
// GlobalMemoryStatusEx / GetPerformanceInfo / NVML; an optional external
// LibreHardwareMonitor source lives in collect/LhmSource.h, OFF by default).
//
// G-B extension (2026-09-18, "同类传感器多值展示"): additive only, again. New
// members are SensorReading::source and SensorSnapshot::extra. Semantics of all
// pre-existing members are unchanged; the four-state model governs the new
// readings too. Multi-value rule: a sensor kind with several instances/sources
// (ACPI thermal zones, NVML GPUs, per-engtype GPU engines, disks, NICs) yields
// one reading PER instance — never an aggregate in place of the set.
//
// P2 extension (2026-09-18, "CPU 温度信息增强"; additive, no struct change):
// SensorSnapshot::extra additionally carries the remaining USER-MODE thermal
// sources, one reading per instance, each label prefixed with its provenance —
//   "WMI 温度 N/（实例）"      Win32_Temperature (ROOT\CIMV2)
//   "WMI 热区计数器 N/（实例）" Win32_PerfFormattedData_Counters_ThermalZoneInformation
//   "DPTF 温度（参与者）"      Intel DPTF TEMPERATURE set (root\Intel_DPTF, fallback
//                              root\Intel(DPTF)); namespace absent -> silent skip
// (ACPI zones keep their "ACPI 热区 N/（InstanceName）" labels in `cpu`.) All
// rows are Ok-only and plausibility-gated — a source that fails, is elevation-
// gated or reports garbage simply yields nothing. Per-core CPU DTS temperatures
// (MSR 0x19C/0x1A2/0x1B1) remain State::NeedDriver territory: they need a
// kernel driver, which this app never ships (red line); the sensor page states
// this and surfaces the optional LibreHardwareMonitor bridge instead.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

struct SensorReading {
    std::wstring label;      // e.g. L"CPU 整包温度（ACPI 热区）"
    double value = 0.0;      // in `unit` units
    std::wstring unit;       // L"°C", L"MHz", L"rpm", L"%"
    enum class State { Ok, NeedAdmin, NeedDriver, NoHardware } state = State::Ok;
    // --- G-B addition (2026-09-18): data provenance, e.g. L"NVML". Empty =
    // this module's built-in source. Pure metadata: no existing producer sets
    // it, so default-constructed readings behave exactly as before.
    std::wstring source;
};

struct DiskHealth {
    std::wstring model;        // friendly name
    std::wstring serial;
    std::wstring busType;      // SATA / NVMe / USB / Unknown
    std::wstring health;       // "良好" / "警告" / "未知" (honest; MSFT_PhysicalDisk + IOCTL detail)
    double tempC = 0.0;        // NVMe composite temp / ATA temperature; NeedAdmin => state
    SensorReading::State tempState = SensorReading::State::NeedAdmin;
    uint64_t powerOnHours = UINT64_MAX;  // UINT64_MAX = unavailable
    // --- F3 addition (2026-09-18): NVMe SMART/health log detail. All pct fields
    // use UINT32_MAX = unavailable (ATA/USB drives, or log not readable — never
    // a fake 0). critWarnBits is meaningful only when critWarnValid is true.
    uint32_t sparePct = UINT32_MAX;        // NVMe "Available Spare" %
    uint32_t spareThreshPct = UINT32_MAX;  // NVMe "Available Spare Threshold" %
    uint32_t wearPct = UINT32_MAX;         // NVMe "Percentage Used" (endurance wear) %
    uint8_t critWarnBits = 0;              // NVMe critical warning bitmap (log[0])
    bool critWarnValid = false;            // true only after a successful NVMe log read
};

struct SensorSnapshot {
    std::vector<SensorReading> cpu;     // package temp (ACPI), per-core freq, usage
    std::vector<SensorReading> gpu;     // temp/util via IGCL/NVML when vendor runtime present
    std::vector<DiskHealth> disks;      // SMART-derived
    std::vector<SensorReading> fans;    // expected: NeedDriver entries only (honest)
    std::wstring notes;                 // aggregate honesty notes for the page header
    // --- F3 additions (2026-09-18), all governed by the four-state model ---
    std::vector<SensorReading> cpuCores;  // per-core freq (MHz, CallNtPowerInformation)
                                          // + utilization (%, PDH Processor Information)
    std::vector<SensorReading> gpus;      // per-sensor GPU expansion (G-B): NVML per card —
                                          // temp / slowdown threshold / power / gpu+mem util /
                                          // fan — plus engine-level util 3D/Copy/VideoDecode/
                                          // Encode (PDH GPU Engine) and VRAM dedicated/shared
                                          // (PDH GPU Adapter Memory). No NVML -> PDH-only.
    std::vector<SensorReading> network;   // per-adapter recv/send B/s + link speed (GetIfTable2)
    std::vector<SensorReading> battery;   // AC/DC, charge %, remaining time (CallNtPowerInformation
                                          // SystemBatteryState); NoHardware entry when absent
    std::vector<SensorReading> memory;    // physical/committed/paged pool/nonpaged pool
                                          // (GlobalMemoryStatusEx + GetPerformanceInfo)
    // --- G-B addition (2026-09-18): readings detected but not fitting any group
    // above (best effort: WMI temperature classes outside MSAcpi_ThermalZone, e.g.
    // Win32_Temperature / MSStorageDriver_FailurePredictData disk temps). Four-
    // state rules apply; EMPTY means "nothing found" — simply not displayed.
    std::vector<SensorReading> extra;
    double uptimeSec = 0.0;               // system uptime in seconds (GetTickCount64)
};

// Blocking read (run on ops job queue). Never throws; partial results allowed.
SensorSnapshot ReadSensors(std::wstring* err);

}  // namespace stm

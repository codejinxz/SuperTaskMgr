#pragma once
// Phase-3 contract: hardware sensors, honest trichotomy per reading (arch §8 / R6):
//   State::Ok         — real value from a documented user-mode source
//   State::NeedAdmin  — source exists but requires elevation (ACPI thermal, SMART)
//   State::NeedDriver — not reachable in user mode at all (fan speeds, per-core CPU temp)
//   State::NoHardware — adapter/disk present but this sensor absent
// NEVER report 0 in place of missing data. No kernel driver ships with this app.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {

struct SensorReading {
    std::wstring label;      // e.g. L"CPU 整包温度（ACPI 热区）"
    double value = 0.0;      // in `unit` units
    std::wstring unit;       // L"°C", L"MHz", L"rpm", L"%"
    enum class State { Ok, NeedAdmin, NeedDriver, NoHardware } state = State::Ok;
};

struct DiskHealth {
    std::wstring model;        // friendly name
    std::wstring serial;
    std::wstring busType;      // SATA / NVMe / USB / Unknown
    std::wstring health;       // "良好" / "警告" / "未知" (honest; MSFT_PhysicalDisk + IOCTL detail)
    double tempC = 0.0;        // NVMe composite temp / ATA temperature; NeedAdmin => state
    SensorReading::State tempState = SensorReading::State::NeedAdmin;
    uint64_t powerOnHours = UINT64_MAX;  // UINT64_MAX = unavailable
};

struct SensorSnapshot {
    std::vector<SensorReading> cpu;     // package temp (ACPI), per-core freq, usage
    std::vector<SensorReading> gpu;     // temp/util via IGCL/NVML when vendor runtime present
    std::vector<DiskHealth> disks;      // SMART-derived
    std::vector<SensorReading> fans;    // expected: NeedDriver entries only (honest)
    std::wstring notes;                 // aggregate honesty notes for the page header
};

// Blocking read (run on ops job queue). Never throws; partial results allowed.
SensorSnapshot ReadSensors(std::wstring* err);

}  // namespace stm

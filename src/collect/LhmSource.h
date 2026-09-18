#pragma once
// F3 contract addition (2026-09-18, registered with the architect): OPTIONAL
// LibreHardwareMonitor (LHM) bridge, OFF by default. LHM ships its own kernel
// driver stack; we never distribute or load one (red line). When the USER runs
// LHM and enables its remote web server (default http://127.0.0.1:8085/data.json),
// this module polls it as a localhost-only HTTP CLIENT:
//   - WinHTTP is bound dynamically (no new link dependency);
//   - loopback addresses only, 1 s timeouts, synchronous one-shot calls;
//   - NO persistent thread is ever spawned (poll happens on caller demand);
//   - every mapped reading is tagged ［LHM］ and reuses the Sensors.h
//     four-state honesty model (unparseable value => NoHardware, never 0).
// Connection failure => false + *err (UI shows "未检测到 LibreHardwareMonitor
// 数据源"); options are process-global and thread-safe.
#include <cstdint>
#include <string>
#include <vector>

#include "collect/Sensors.h"  // SensorReading

namespace stm {

struct LhmOptions {
    bool enabled = false;              // default OFF: no network traffic unless the user opts in
    uint16_t port = 8085;              // LHM remote web server default port
    std::wstring host = L"127.0.0.1";  // loopback only; enforced again inside PollLhm
};

void SetLhmOptions(const LhmOptions& options);  // thread-safe
LhmOptions GetLhmOptions();                     // thread-safe

// One synchronous poll of <host>:<port>/data.json. Never spawns threads and
// never blocks longer than the ~1 s HTTP timeouts. On success fills *out with
// SensorReading items (label = LHM node path, value/unit parsed from the Value
// text, every label suffixed with ［LHM］). On failure returns false, fills
// *err and leaves *out empty.
bool PollLhm(std::vector<SensorReading>* out, std::wstring* err);

// Parser only (no network): minimal recursive JSON for LHM data.json trees
// (nested Text/Value/Sensor/Hardware nodes; strings with escapes, numbers,
// booleans, null). Exposed for the selftest; returns false on malformed input.
bool ParseLhmJson(const std::string& utf8, std::vector<SensorReading>* out);

}  // namespace stm

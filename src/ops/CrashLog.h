#pragma once
// Phase-6 contract: crash/hang history from the classic event log (documented
// Windows Event Log API; Application+System channels are readable without admin).
// Architect-owned, frozen.
#include <cstdint>
#include <string>
#include <vector>

namespace stm {
namespace ops {

struct CrashEvent {
    int64_t unixTime = 0;
    std::wstring provider;   // e.g. "Application Error"
    uint32_t eventId = 0;    // 1000 = app error, 1001 = WER report, 1002 = app hang
    uint16_t level = 0;      // 2 = error, 3 = warning, ...
    std::wstring app;        // faulting application (from EventData, best effort)
    std::wstring module;     // faulting module (best effort, may be empty)
    std::wstring summary;    // short human text (Chinese label + key fields)
};

// Newest-first, at most maxCount events across Application+System for IDs
// 1000/1001/1002. Empty vector with empty err = no recent crashes (honest).
std::vector<CrashEvent> QueryCrashEvents(uint32_t maxCount, std::wstring* err);

}  // namespace ops
}  // namespace stm

#pragma once
// Elevation helpers (arch section 7). Contract header — architect-owned.
#include <string>

namespace stm {
namespace ops {

bool IsElevated();        // forwards to core::IsProcessElevated
bool CanElevate();        // UAC-capable system and current token may request elevation

// Relaunch self with lpVerb=L"runas". ERROR_CANCELLED is swallowed (user declined => false, no exit).
// Returns TRUE only when a new process was launched — caller must exit immediately.
// Session handoff (SaveSession before calling) is the caller's duty, per arch section 7.
bool RelaunchAsAdmin(const std::wstring& args);

}  // namespace ops
}  // namespace stm

#pragma once
// Elevation-relaunch session handoff (arch section 7). Contract header — architect-owned.
// Old instance: SaveSession -> exit (mutex released by process teardown).
// New instance: CreateMutex (≤1000ms wait) -> LoadSession (ts<60s guard).
#include <string>
#include "core/ProcData.h"

namespace stm {
namespace ops {

struct SessionState {
    int page = 0;
    ProcKey selected;
    std::wstring sortKey = L"name";   // column id
    int sortDir = 0;                  // 0 asc, 1 desc
    long winX = 0, winY = 0, winW = 0, winH = 0;
    uint32_t intervalMs = 1000;
    int64_t ts = 0;                   // unix seconds, written by SaveSession
};

// Atomic write (temp + MoveFileExW REPLACE_EXISTING). Sets ts internally.
bool SaveSession(const SessionState& s);
// Returns false when missing / unparsable / older than 60s (fresh start in that case).
bool LoadSession(SessionState* out);

}  // namespace ops
}  // namespace stm

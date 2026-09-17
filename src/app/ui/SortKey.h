#pragma once
// Header-only sorting primitives for the process table (arch section 8).
// Shared between the UI (Pages.cpp) and selftest (ui_test.cpp); intentionally
// free of ImGui/OS dependencies so stm_selftest can include it without app objects.
//
// Semantics (frozen by the architecture doc):
//  - raw values compare, no locale collation for the name column
//  - unavailable metrics (NaN / kUnavailU64) always sort LAST, both directions
//  - pid ascending breaks ties, both directions
#include <cstdint>
#include "core/ProcData.h"

namespace stm {
namespace ui {

// Table columns 0..Count-1 are sortable; badges/description are not.
enum class SortColumn : int {
    Name = 0, Pid, Cpu, MemPrivate, Commit, Disk, Net, HardFaults, Handles, Threads,
    CtxSwitches,
    Count
};

// Stable identifier for config/session persistence ("sortKey" / "sortDir").
inline const wchar_t* SortColumnId(SortColumn c) {
    switch (c) {
        case SortColumn::Name:        return L"name";
        case SortColumn::Pid:         return L"pid";
        case SortColumn::Cpu:         return L"cpu";
        case SortColumn::MemPrivate:  return L"mem";
        case SortColumn::Commit:      return L"commit";
        case SortColumn::Disk:        return L"disk";
        case SortColumn::Net:         return L"net";
        case SortColumn::HardFaults:  return L"hardfaults";
        case SortColumn::Handles:     return L"handles";
        case SortColumn::Threads:     return L"threads";
        case SortColumn::CtxSwitches: return L"ctxswitches";
        default:                      return L"name";
    }
}

// Inverse of SortColumnId; keeps the current value when id is unknown.
inline bool ParseSortColumn(const wchar_t* id, SortColumn* out) {
    if (!id || !out) return false;
    struct Entry { const wchar_t* id; SortColumn col; };
    static const Entry kTable[] = {
        {L"name", SortColumn::Name},         {L"pid", SortColumn::Pid},
        {L"cpu", SortColumn::Cpu},           {L"mem", SortColumn::MemPrivate},
        {L"commit", SortColumn::Commit},     {L"disk", SortColumn::Disk},
        {L"net", SortColumn::Net},           {L"hardfaults", SortColumn::HardFaults},
        {L"handles", SortColumn::Handles},   {L"threads", SortColumn::Threads},
        {L"ctxswitches", SortColumn::CtxSwitches},
    };
    for (const Entry& e : kTable) {
        const wchar_t* a = e.id;
        const wchar_t* b = id;
        while (*a != L'\0' && *a == *b) { ++a; ++b; }
        if (*a == L'\0' && *b == L'\0') { *out = e.col; return true; }
    }
    return false;
}

// True when this column's value is "unavailable" for the row (rendered as em dash).
inline bool IsColumnUnavail(const ProcInfo& p, SortColumn c) {
    switch (c) {
        case SortColumn::Cpu:         return p.cpuPercent != p.cpuPercent;              // NaN
        case SortColumn::MemPrivate:  return p.privateWorkingSet == kUnavailU64;
        case SortColumn::Disk:        return p.diskBytesPerSec != p.diskBytesPerSec;
        case SortColumn::Net:         return p.netBytesPerSec != p.netBytesPerSec;
        case SortColumn::HardFaults:  return p.pageFaultsPerSec != p.pageFaultsPerSec;
        case SortColumn::CtxSwitches: return p.contextSwitchesPerSec != p.contextSwitchesPerSec;
        default:                      return false;  // name/pid/commit/handles/threads: never
    }
}

namespace sort_detail {
inline int CmpU64(uint64_t a, uint64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }
inline int CmpU32(uint32_t a, uint32_t b) { return a < b ? -1 : (a > b ? 1 : 0); }
inline int CmpDouble(double a, double b) { return a < b ? -1 : (a > b ? 1 : 0); }  // no NaN here

inline int CmpNameLower(const std::wstring& a, const std::wstring& b) {
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const wchar_t x = static_cast<wchar_t>(towlower(a[i]));
        const wchar_t y = static_cast<wchar_t>(towlower(b[i]));
        if (x != y) return x < y ? -1 : 1;
    }
    return CmpU64(static_cast<uint64_t>(a.size()), static_cast<uint64_t>(b.size()));
}
}  // namespace sort_detail

// Ascending raw 3-way compare WITHOUT unavailable handling or tie-break.
// Returns -1 when a first, +1 when b first, 0 when equal.
inline int CompareColumnRaw(const ProcInfo& a, const ProcInfo& b, SortColumn c) {
    switch (c) {
        case SortColumn::Name:        return sort_detail::CmpNameLower(a.name, b.name);
        case SortColumn::Pid:         return sort_detail::CmpU32(a.key.pid, b.key.pid);
        case SortColumn::Cpu:         return sort_detail::CmpDouble(a.cpuPercent, b.cpuPercent);
        case SortColumn::MemPrivate:  return sort_detail::CmpU64(a.privateWorkingSet, b.privateWorkingSet);
        case SortColumn::Commit:      return sort_detail::CmpU64(a.commitBytes, b.commitBytes);
        case SortColumn::Disk:        return sort_detail::CmpDouble(a.diskBytesPerSec, b.diskBytesPerSec);
        case SortColumn::Net:         return sort_detail::CmpDouble(a.netBytesPerSec, b.netBytesPerSec);
        case SortColumn::HardFaults:  return sort_detail::CmpDouble(a.pageFaultsPerSec, b.pageFaultsPerSec);
        case SortColumn::Handles:     return sort_detail::CmpU32(a.handles, b.handles);
        case SortColumn::Threads:     return sort_detail::CmpU32(a.threads, b.threads);
        case SortColumn::CtxSwitches: return sort_detail::CmpDouble(a.contextSwitchesPerSec, b.contextSwitchesPerSec);
        default:                      return 0;
    }
}

// Full ascending 3-way compare: unavailable last, pid ascending tie-break.
// The single entry point the selftest exercises.
inline int CompareColumn(const ProcInfo& a, const ProcInfo& b, SortColumn c) {
    const bool au = IsColumnUnavail(a, c);
    const bool bu = IsColumnUnavail(b, c);
    if (au != bu) return au ? 1 : -1;  // unavailable rows always last
    if (au) return 0;                  // both unavailable: equal bucket
    int r = CompareColumnRaw(a, b, c);
    if (r == 0) {
        if (a.key.pid < b.key.pid) r = -1;
        else if (a.key.pid > b.key.pid) r = 1;
    }
    return r;
}

// std::sort predicate used by the UI table. desc flips value order only;
// unavailable placement (last) and the pid tie-break (ascending) stay fixed.
inline bool SortLess(const ProcInfo& a, const ProcInfo& b, SortColumn c, bool desc) {
    const bool au = IsColumnUnavail(a, c);
    const bool bu = IsColumnUnavail(b, c);
    if (au != bu) return bu;   // a first iff b is the unavailable one
    if (au) return false;      // same bucket: not less (stable_sort keeps arrival order)
    const int r = CompareColumnRaw(a, b, c);
    if (r != 0) return desc ? r > 0 : r < 0;
    return a.key.pid < b.key.pid;
}

}  // namespace ui
}  // namespace stm

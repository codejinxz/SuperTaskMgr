#pragma once
// 进程表的仅头文件排序原语（架构第 8 节）。
// 由 UI（Pages.cpp）与 selftest（ui_test.cpp）共享；刻意不依赖
// ImGui/OS，使 stm_selftest 无需应用对象即可包含。
//
// 语义（架构文档冻结）：
//  - 原始值比较，名称列不做区域设置排序
//  - 不可得指标（NaN / kUnavailU64）在两个方向上都恒排最后
//  - 两个方向都以 pid 升序破平
#include <cstdint>
#include "core/ProcData.h"

namespace stm {
namespace ui {

// 表列 0..Count-1 可排序；徽标/描述不可排序。
enum class SortColumn : int {
    Name = 0, Pid, Cpu, MemPrivate, Commit, Disk, Net, HardFaults, Handles, Threads,
    CtxSwitches,
    Count
};

// 供配置/会话持久化的稳定标识（"sortKey" / "sortDir"）。
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

// SortColumnId 的逆变换；id 未知时保持当前值。
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

// 该列对这一行的值是否"不可得"（渲染为破折号）。
inline bool IsColumnUnavail(const ProcInfo& p, SortColumn c) {
    switch (c) {
        case SortColumn::Cpu:         return p.cpuPercent != p.cpuPercent;              // NaN
        case SortColumn::MemPrivate:  return p.privateWorkingSet == kUnavailU64;
        case SortColumn::Disk:        return p.diskBytesPerSec != p.diskBytesPerSec;
        case SortColumn::Net:         return p.netBytesPerSec != p.netBytesPerSec;
        case SortColumn::HardFaults:  return p.pageFaultsPerSec != p.pageFaultsPerSec;
        case SortColumn::CtxSwitches: return p.contextSwitchesPerSec != p.contextSwitchesPerSec;
        default:                      return false;  // name/pid/commit/handles/threads：永不为不可得
    }
}

namespace sort_detail {
inline int CmpU64(uint64_t a, uint64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }
inline int CmpU32(uint32_t a, uint32_t b) { return a < b ? -1 : (a > b ? 1 : 0); }
inline int CmpDouble(double a, double b) { return a < b ? -1 : (a > b ? 1 : 0); }  // 此处无 NaN

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

// 升序原始三路比较，不含不可得处理与破平。
// a 在前返回 -1，b 在前返回 +1，相等返回 0。
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

// 完整升序三路比较：不可得在最后，pid 升序破平。
// selftest 测试的唯一入口。
inline int CompareColumn(const ProcInfo& a, const ProcInfo& b, SortColumn c) {
    const bool au = IsColumnUnavail(a, c);
    const bool bu = IsColumnUnavail(b, c);
    if (au != bu) return au ? 1 : -1;  // 不可得行恒在最后
    if (au) return 0;                  // 都不可得：同一桶
    int r = CompareColumnRaw(a, b, c);
    if (r == 0) {
        if (a.key.pid < b.key.pid) r = -1;
        else if (a.key.pid > b.key.pid) r = 1;
    }
    return r;
}

// UI 表格使用的 std::sort 谓词。desc 只翻转值顺序；
// 不可得位置（最后）与 pid 破平（升序）保持不变。
inline bool SortLess(const ProcInfo& a, const ProcInfo& b, SortColumn c, bool desc) {
    const bool au = IsColumnUnavail(a, c);
    const bool bu = IsColumnUnavail(b, c);
    if (au != bu) return bu;   // 当且仅当 b 是不可得方时 a 在前
    if (au) return false;      // 同一桶：不小于（stable_sort 保持到达顺序）
    const int r = CompareColumnRaw(a, b, c);
    if (r != 0) return desc ? r > 0 : r < 0;
    return a.key.pid < b.key.pid;
}

}  // namespace ui
}  // namespace stm

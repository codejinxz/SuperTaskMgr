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
#include <string>
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

// ---------------------------------------------------------------------------
// P1②：进程表列 UserID / 显示顺序持久化（纯函数，selftest 共用）。
// 背景：进程表加 ImGuiTableFlags_Reorderable 后，表头可拖动重排 —— 排序
// 回调（SortSpecs）与列宽/顺序持久化绝不能按**显示序**映射，否则拖动后
// 全部错位。契约：
//   - TableSetupColumn 一律按**槽位序**（0..kProcColSlots-1）提交，UserID
//     即槽位值；widths_[slot] / "colW_"+SortColumnId(slot) 键槽与 UserID
//     一一对应，与显示顺序解耦（ImGui 的 Columns[i] 也按提交序索引）。
//   - 排序回调只认 specs->Specs[0].ColumnUserID（ProcColumnFromUserId）。
//   - 显示顺序（拖动结果）持久化在 cfg "colOrder"：UserID 逗号串，按显示
//     位置排列；启动时经 ProcColumnOrderFromCfg 校验为合法排列后恢复。
// ---------------------------------------------------------------------------
inline constexpr int kProcColUserIdBadges = static_cast<int>(SortColumn::Count);      // 11
inline constexpr int kProcColUserIdDesc = static_cast<int>(SortColumn::Count) + 1;    // 12
inline constexpr int kProcColSlots = static_cast<int>(SortColumn::Count) + 2;         // 13

// 槽位 -> UserID（恒等；显式函数使映射关系在调用点可读）。
inline int ProcColumnUserId(int slot) { return slot; }

// UserID 是否为可排序列（徽标/描述不可排序，表头 NoSort）。
inline bool ProcColumnIsSortableUserId(int userId) {
    return userId >= 0 && userId < static_cast<int>(SortColumn::Count);
}

// UserID -> SortColumn；徽标/描述/越界返回 false（排序回调据此忽略）。
inline bool ProcColumnFromUserId(int userId, SortColumn* out) {
    if (!ProcColumnIsSortableUserId(userId)) return false;
    if (out != nullptr) *out = static_cast<SortColumn>(userId);
    return true;
}

// colOrder 值 → cfg 文本（UserID 逗号串，按显示位置排列）。
inline std::wstring ProcColumnOrderToCfg(const int* userIds, int count) {
    std::wstring s;
    if (userIds == nullptr || count != kProcColSlots) return s;
    for (int i = 0; i < count; ++i) {
        if (i > 0) s += L',';
        s += std::to_wstring(userIds[i]);
    }
    return s;
}

// 解析 colOrder 文本 → 按显示位置的 UserID 数组。
// 返回写入的个数（= kProcColSlots）；任何形态错误（个数/越界/重复/非排列）
// 返回 -1，调用方保持默认顺序（绝不应用半途而废的顺序）。
inline int ProcColumnOrderFromCfg(const wchar_t* s, int* outUserIds, int cap) {
    if (s == nullptr || outUserIds == nullptr || cap < kProcColSlots) return -1;
    int seen[kProcColSlots] = {};
    int n = 0;
    const wchar_t* p = s;
    while (*p != L'\0') {
        if (*p == L',') return -1;          // 空字段
        long v = 0;
        int digits = 0;
        while (*p >= L'0' && *p <= L'9') {
            v = v * 10 + (*p - L'0');
            if (v > kProcColSlots) return -1;  // 早停：必越界
            ++digits;
            ++p;
        }
        if (digits == 0) return -1;
        if (n >= kProcColSlots) return -1;      // 字段过多
        if (v >= kProcColSlots || seen[v] != 0) return -1;  // 越界/重复
        seen[v] = 1;
        outUserIds[n++] = static_cast<int>(v);
        if (*p == L',') {
            ++p;
            if (*p == L'\0') return -1;         // 尾逗号
        } else if (*p != L'\0') {
            return -1;                          // 非数字非逗号
        }
    }
    if (n != kProcColSlots) return -1;          // 字段不足
    return n;
}

// 默认显示顺序（恒等排列）写入 out，返回个数。
inline int ProcColumnDefaultOrder(int* outUserIds, int cap) {
    if (outUserIds == nullptr || cap < kProcColSlots) return -1;
    for (int i = 0; i < kProcColSlots; ++i) outUserIds[i] = ProcColumnUserId(i);
    return kProcColSlots;
}

}  // namespace ui
}  // namespace stm

// UI 排序原语测试。SortKey.h 仅头文件（无应用对象），
// 因此这些测试在只链接 core+collect+ops 的 stm_selftest 内运行。
#include "selftest/TestFramework.h"
#include "app/ui/SortKey.h"

using stm::ProcInfo;
using stm::ui::SortColumn;
using stm::ui::SortLess;

namespace {
ProcInfo MakeProc(uint32_t pid, const wchar_t* name, double cpu, uint64_t privWs) {
    ProcInfo p;
    p.key.pid = pid;
    p.key.createTime = 1000 + pid;  // 可区分的身份
    p.name = name;
    p.cpuPercent = cpu;
    p.privateWorkingSet = privWs;
    return p;
}
}  // namespace

// 不可得 CPU（NaN）在两个方向上都必须排在最后。
STM_TEST(ui_sort_cpu_unavail_last) {
    const ProcInfo live = MakeProc(10, L"a.exe", 5.0, 100);
    const ProcInfo dead = MakeProc(20, L"b.exe", stm::kUnavail, 100);
    if (stm::ui::CompareColumn(live, dead, SortColumn::Cpu) >= 0) {
        *err = L"升序时 NaN 未排在最后";
        return false;
    }
    if (!SortLess(live, dead, SortColumn::Cpu, true) || SortLess(dead, live, SortColumn::Cpu, true)) {
        *err = L"降序时 NaN 未保持在最后";
        return false;
    }
    return true;
}

// 大小写不敏感的同名以 pid 升序破平（两个方向）。
STM_TEST(ui_sort_name_tiebreak_pid) {
    const ProcInfo hi = MakeProc(30, L"NOTEPAD.EXE", 1.0, 100);
    const ProcInfo lo = MakeProc(20, L"notepad.exe", 1.0, 100);
    if (stm::ui::CompareColumn(lo, hi, SortColumn::Name) >= 0) {
        *err = L"同名并列未按 pid 升序";
        return false;
    }
    if (!SortLess(lo, hi, SortColumn::Name, true) || SortLess(hi, lo, SortColumn::Name, true)) {
        *err = L"降序时 pid 平局规则失效";
        return false;
    }
    return true;
}

// kUnavailU64 私有工作集排在最后；desc 也救不回来。
STM_TEST(ui_sort_mem_unavail_last) {
    const ProcInfo small = MakeProc(5, L"a.exe", 0.0, 100);
    const ProcInfo unknownHi = MakeProc(9, L"b.exe", 0.0, stm::kUnavailU64);
    const ProcInfo unknownLo = MakeProc(2, L"c.exe", 0.0, stm::kUnavailU64);
    if (stm::ui::CompareColumn(small, unknownHi, SortColumn::MemPrivate) >= 0) {
        *err = L"kUnavailU64 未排在已知值之后";
        return false;
    }
    if (!SortLess(small, unknownHi, SortColumn::MemPrivate, true)) {
        *err = L"降序时 kUnavailU64 未保持在最后";
        return false;
    }
    if (stm::ui::CompareColumn(unknownLo, unknownHi, SortColumn::MemPrivate) != 0) {
        *err = L"两个不可用值应视为并列";
        return false;
    }
    return true;
}

// 列 id 在配置/会话持久化层中往返一致。
STM_TEST(ui_sort_column_ids_roundtrip) {
    for (int i = 0; i < static_cast<int>(SortColumn::Count); ++i) {
        const SortColumn c = static_cast<SortColumn>(i);
        SortColumn back = SortColumn::Name;
        if (!stm::ui::ParseSortColumn(stm::ui::SortColumnId(c), &back) || back != c) {
            *err = L"列标识往返失败";
            return false;
        }
    }
    SortColumn bogus = SortColumn::Name;
    if (stm::ui::ParseSortColumn(L"不存在", &bogus)) {
        *err = L"非法列标识不应解析成功";
        return false;
    }
    return true;
}

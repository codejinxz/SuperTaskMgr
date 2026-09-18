#pragma once
// P3 任务一：一键内存优化 —— 纯逻辑（header-only，无 ImGui / 无 app 对象依赖，
// stm_selftest 仅链 core+collect+ops 即可覆盖）。
//
// 职责：
//  - SelectTopCleanupCandidates：按「私有工作集」降序取 TopN 候选；系统进程经
//    ui3::ClassifyProc（「区分系统进程」分类器，Critical/Windows/ServiceHost）
//    标注为不可选 —— ops 侧 ProtectedList/身份复核仍是执行时硬门禁，这里只是
//    把明显不该动的进程从默认勾选中排除。
//  - DefaultCleanupSelection / AnySelected / SelectedBytes：默认勾选前 5 个可选
//    项、空选择判定、涉及字节合计（模态里如实展示）。
//  - CleanupOutcome + RecordTrimResult + FormatCleanupDoneText：批量执行结果
//    聚合与 toast 文案（「释放约 X MiB」为估计值：以成功项的私有工作集合计）。
//
// 实际执行（单个 job 批量 TrimWorkingSet + 可选 PurgeStandbyList）在
// app/ui/ConfirmAction.h::MakeMemCleanupJob —— 本头文件保持零副作用。
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "app/ui3/ProcKind.h"
#include "core/ProcData.h"
#include "core/Str.h"

namespace stm {
namespace ui3 {

inline constexpr uint64_t kMiBBytes = 1024ull * 1024ull;

// 一个候选行：模态里展示 名称 + 当前私有工作集，并按类别决定能否勾选。
struct CleanupCandidate {
    ProcKey key;
    std::wstring name;
    uint64_t privateWorkingSet = kUnavailU64;  // kUnavailU64 = 未知（排最后、不可选）
    ProcKind kind = ProcKind::Unknown;
    bool selectable = false;
};

// 与进程页「只看用户进程」同一套排除规则：Critical（保护名单）/Windows/
// ServiceHost 不可选；UWP 属用户应用保留。私有工作集未知时也不可选（诚实）。
inline bool CleanupSelectable(ProcKind kind, uint64_t privateWorkingSet) {
    return ProcKindVisibleInUserFilter(kind) && privateWorkingSet != kUnavailU64;
}

// TopN 选取（systemRoot 显式传入以便纯函数自测）。稳定排序：同值保持快照顺序。
inline std::vector<CleanupCandidate> SelectTopCleanupCandidates(
    const Snapshot& snap, size_t topN, const std::wstring& systemRoot) {
    std::vector<CleanupCandidate> out;
    out.reserve(snap.procs.size());
    for (const ProcInfo& p : snap.procs) {
        CleanupCandidate c;
        c.key = p.key;
        c.name = p.name;
        c.privateWorkingSet = p.privateWorkingSet;
        c.kind = ClassifyProc(p.key.pid, p.name, p.path, p.flags, systemRoot);
        c.selectable = CleanupSelectable(c.kind, c.privateWorkingSet);
        out.push_back(std::move(c));
    }
    const auto sizeKey = [](const CleanupCandidate& c) -> uint64_t {
        return c.privateWorkingSet == kUnavailU64 ? 0 : c.privateWorkingSet;
    };
    std::stable_sort(out.begin(), out.end(),
                     [&sizeKey](const CleanupCandidate& a, const CleanupCandidate& b) {
                         return sizeKey(a) > sizeKey(b);
                     });
    if (out.size() > topN) out.resize(topN);
    return out;
}

// 运行时入口：systemRoot 取自 GetWindowsDirectoryW（%SystemRoot%）。
inline std::vector<CleanupCandidate> SelectTopCleanupCandidates(const Snapshot& snap,
                                                                size_t topN) {
    wchar_t root[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(root, MAX_PATH);
    const std::wstring systemRoot = len > 0 && len < MAX_PATH ? std::wstring(root) : std::wstring();
    return SelectTopCleanupCandidates(snap, topN, systemRoot);
}

// 默认勾选：可选条目中按列表顺序（即占用降序）勾前 defaultCount 个。
inline std::vector<bool> DefaultCleanupSelection(const std::vector<CleanupCandidate>& items,
                                                 size_t defaultCount = 5) {
    std::vector<bool> sel(items.size(), false);
    size_t picked = 0;
    for (size_t i = 0; i < items.size() && picked < defaultCount; ++i) {
        if (items[i].selectable) {
            sel[i] = true;
            ++picked;
        }
    }
    return sel;
}

inline bool AnySelected(const std::vector<bool>& sel) {
    for (bool b : sel) {
        if (b) return true;
    }
    return false;
}

// 已勾选个数（模态汇总行/toast 的「已处理 N 个」口径由执行结果另计）。
inline size_t CountSelected(const std::vector<bool>& sel) {
    size_t n = 0;
    for (bool b : sel) {
        if (b) ++n;
    }
    return n;
}

// 勾选项的私有工作集合计（估计值；模态提示行用，未知项按 0 计）。
inline uint64_t SelectedBytes(const std::vector<CleanupCandidate>& items,
                              const std::vector<bool>& sel) {
    uint64_t sum = 0;
    const size_t n = std::min(items.size(), sel.size());
    for (size_t i = 0; i < n; ++i) {
        if (sel[i] && items[i].privateWorkingSet != kUnavailU64) {
            sum += items[i].privateWorkingSet;
        }
    }
    return sum;
}

// 批量执行结果聚合：attempted 含失败项（「已处理 N 个 …（失败 M 个）」口径）。
struct CleanupOutcome {
    int attempted = 0;        // 实际尝试 TrimWorkingSet 的个数
    int failed = 0;           // 其中失败个数（逐项失败只计数，不中断批次）
    std::wstring firstError;  // 首个失败的原始 err（V15-P2-2：toast 如实回显）
    uint64_t freedBytes = 0;  // 成功项的私有工作集合计（释放量估计）
    bool purgeAttempted = false;  // 是否附加了待机列表清理
    bool purgeOk = true;          // 附加清理是否成功（失败另有单独 note）
};

inline void RecordTrimResult(CleanupOutcome& o, bool ok, uint64_t candidateBytes) {
    ++o.attempted;
    if (ok) {
        o.freedBytes += candidateBytes;
    } else {
        ++o.failed;
    }
}

// toast 文案（诚实表述：释放的是「工作集」估计值，不是进程真实占用）。
inline std::wstring FormatCleanupDoneText(const CleanupOutcome& o) {
    const double mib = static_cast<double>(o.freedBytes) / static_cast<double>(kMiBBytes);
    std::wstring text = Fmt(L"一键优化完成：已处理 {} 个进程，释放约 {:.1f} MiB 工作集",
                            o.attempted, mib);
    if (o.failed > 0) text += Fmt(L"（失败 {} 个）", o.failed);
    return text;
}

}  // namespace ui3
}  // namespace stm

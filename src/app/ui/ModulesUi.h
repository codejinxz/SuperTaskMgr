#pragma once
// F4 quick-win #1: detail-panel module list — pure decision helpers, header-only
// and ImGui-free so stm_selftest can exercise them (same pattern as ConfirmAction.h).
// The UI (Pages.cpp DetailPanel) requests ops::DetailKind::Modules, renders the
// list, and verifies per-module signatures through the ops job queue with a
// per-path cache.
#include <cstddef>
#include "ops/DetailsProvider.h"  // ops::ProcessDetails, ops::SigState

namespace stm {
namespace ui {

// What the module section should display for the currently selected process.
enum class ModuleSectionState {
    Pending,    // details job still in flight => "查询中…"
    NeedAdmin,  // fetch attempted but list unreadable without elevation (honest gate)
    Unavailable,// fetch attempted, unreadable even elevated (dead race / protected)
    Ready,      // list available (count may be 0: a process can legitimately have none)
};
inline ModuleSectionState DecideModuleSection(bool entryDone, bool modulesResolved,
                                              bool elevated, size_t count) {
    (void)count;
    if (!entryDone) return ModuleSectionState::Pending;
    if (!modulesResolved) return elevated ? ModuleSectionState::Unavailable
                                          : ModuleSectionState::NeedAdmin;
    return ModuleSectionState::Ready;
}
inline const wchar_t* ModuleSectionText(ModuleSectionState s) {
    switch (s) {
        case ModuleSectionState::Pending: return L"模块列表查询中…";
        case ModuleSectionState::NeedAdmin: return L"模块列表需要管理员权限（以管理员身份重启后可查看）";
        case ModuleSectionState::Unavailable: return L"无法读取模块列表（进程已退出或受保护）";
        case ModuleSectionState::Ready: return L"";
    }
    return L"";
}

// Per-module signature badge (value comes from ops::VerifyFileSignature run on the
// ops worker; -1 = job still in flight, DriverPage slot pattern).
enum class ModuleBadge { Pending, Valid, Unsigned, Invalid, Unknown };
inline ModuleBadge ModuleBadgeForSig(int sigStateInt) {
    if (sigStateInt < 0) return ModuleBadge::Pending;
    switch (static_cast<ops::SigState>(sigStateInt)) {
        case ops::SigState::Valid: return ModuleBadge::Valid;
        case ops::SigState::Unsigned: return ModuleBadge::Unsigned;
        case ops::SigState::Invalid: return ModuleBadge::Invalid;
        case ops::SigState::NoCheck:
        case ops::SigState::Unknown:
        default: return ModuleBadge::Unknown;
    }
}
inline const wchar_t* ModuleBadgeLabel(ModuleBadge b) {
    switch (b) {
        case ModuleBadge::Pending: return L"校验中";
        case ModuleBadge::Valid: return L"有效签名";
        case ModuleBadge::Unsigned: return L"未签名";
        case ModuleBadge::Invalid: return L"签名无效";
        case ModuleBadge::Unknown: return L"未知";
    }
    return L"";
}

}  // namespace ui
}  // namespace stm

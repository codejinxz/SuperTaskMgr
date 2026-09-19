#pragma once
// F4 快赢 #1：详情面板模块列表——纯决策辅助，仅头文件、
// 不依赖 ImGui，便于 stm_selftest 测试（与 ConfirmAction.h 同一模式）。
// UI（Pages.cpp 的 DetailPanel）请求 ops::DetailKind::Modules，渲染列表，
// 并经 ops 任务队列配合按路径缓存逐模块验证签名。
//
#include <cstddef>
#include "ops/DetailsProvider.h"  // ops::ProcessDetails, ops::SigState

namespace stm {
namespace ui {

// 当前选中进程的模块区应当展示什么。
enum class ModuleSectionState {
    Pending,    // details job still in flight => "查询中…"
    NeedAdmin,  // 已尝试抓取但未提权时列表不可读（诚实的闸门）
    Unavailable,// 已尝试抓取，提权后仍不可读（竞态失效 / 受保护）
    Ready,      // 列表可用（数量可为 0：进程确实可能没有模块）
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

// 每模块签名徽标（值来自在 ops 工作线程上运行的 ops::VerifyFileSignature；
// -1 = 任务仍在途，DriverPage 槽位模式）。
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

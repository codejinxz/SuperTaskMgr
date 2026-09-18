#pragma once
// Confirm-dialog execution layer (bug F1 fix, 2026-09): the action performed when
// the user presses the confirm button of any destructive-op dialog, extracted from
// the ImGui translation units into this header-only, ImGui-free function so
// stm_selftest can exercise the whole "confirm -> jobs.Submit -> ops -> notes/toast"
// chain without a GUI.
//
// Contract:
//  - ONLY the UI confirm button calls ExecuteConfirmedAction(); it never runs ops
//    inline — everything goes through ctx->jobs (serial ops worker, arch section 5).
//  - EVERY path produces a user-visible notification: the job posts success/failure
//    via ctx->notes (drained into toasts by the shell), and a submission failure
//    (queue not running / context gone) posts a JobFailed note immediately instead
//    of failing silently (the "没有任何反应" class of bug).
//  - Depends only on core/ + ops/ (+ the AppContext aggregate), never on ImGui,
//    so stm_selftest (core+collect+ops link) can include it.
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "app/AppContext.h"
#include "app/ui3/MemCleanup.h"     // P3 任务一：一键优化候选/聚合（纯逻辑）
#include "app/ui3/ProcControlUi.h"  // ui3::PriorityLabel（优先级文案单一来源）
#include "core/ProcData.h"
#include "core/Str.h"
#include "ops/ProcessControl.h"
#include "ops/ProcessOps.h"
#include "ops/StartupOps.h"

namespace stm {
namespace ui {

// Kind of the pending confirm dialog (shared by the process-page dialogs and the
// phase-3 startup dialog).
struct ConfirmKind {
    enum E {
        None = 0,
        Kill,            // 终止进程
        KillTree,        // 终止进程树
        TrimWorkingSet,  // 释放工作集
        PurgeStandby,    // 清理待机列表
        StartupToggle,   // 启用/禁用启动项 (startupEnable + startupItem)
        // --- F4#2 进程控制（挂起/恢复/优先级/亲和性）---
        Suspend,         // 挂起进程（比终止更危险：保护名单进程菜单项直接禁用）
        Resume,          // 恢复进程（低风险，可直接提交、不开确认框）
        SetPriority,     // 设置优先级（设置前经 ConfirmDialog 确认；priority 字段）
        SetAffinity,     // 设置亲和性（亲和性模态本身即确认；affinityMask 字段）
        // --- P3 新增 ---
        MemCleanup,      // 一键内存优化（cleanupItems/cleanupSelected/cleanupPurgeStandby）
        CloseAsk,        // 关闭行为询问（任务三：非 ops job，由 UI 直接处理，不走执行表）
    };
};

// Pending confirmation request. Identity fields (ProcKey/name/path/startup item)
// are locked when the dialog opens; ops re-verifies identity at execution time.
struct ConfirmRequest {
    ConfirmKind::E kind = ConfirmKind::None;
    ProcKey key;
    uint32_t pid = 0;
    std::wstring name;
    std::wstring path;
    bool serviceHost = false;
    // Tree planning: filled by an ops job, polled by the UI until ready.
    // -1 pending, -2 planning failed, >= 0 planned count.
    std::shared_ptr<std::atomic<int>> planCount;
    // StartupToggle only.
    bool startupEnable = false;
    ops::StartupItem startupItem;
    // F4#2: SetPriority -> priority; SetAffinity -> affinityMask (nonzero).
    ops::ProcPriority priority = ops::ProcPriority::Normal;
    uint64_t affinityMask = 0;
    // P3 任务三 CloseAsk only: 「记住我的选择」复选框（勾选时才写 cfg closeAction）。
    bool rememberChoice = false;
    // P3 任务一 MemCleanup only: 打开模态时冻结的 Top10 候选与逐项勾选状态；
    // ops 执行时照常做身份复核（createTime）与保护名单硬门禁。
    std::vector<ui3::CleanupCandidate> cleanupItems;
    std::vector<bool> cleanupSelected;
    bool cleanupPurgeStandby = false;
};

// ---------------------------------------------------------------------------
// P3 任务三：标题栏关闭行为（cfg closeAction，持久化到 config.json）。
//   0 = 每次询问（默认，弹三选一模态）  1 = 直接退出  2 = 最小化到托盘。
// 非法值一律归一为 0（宁多问一次，不做危险假设）。
// ---------------------------------------------------------------------------
inline constexpr wchar_t kCloseActionCfgKey[] = L"closeAction";
inline int NormalizeCloseAction(int64_t v) { return v == 1 ? 1 : v == 2 ? 2 : 0; }

// Non-empty suffix appended to failure notes when not elevated (H5 requirement:
// the user must see why an op may have failed and what to try next).
inline std::wstring AdminHintSuffix(bool elevated) {
    return elevated ? std::wstring() : std::wstring(L"（可能需要管理员权限，可尝试提权重启）");
}

inline void PostConfirmNote(AppContext& app, Notification::Kind kind, const std::wstring& text) {
    Notification n;
    n.kind = kind;
    n.text = text;
    app.notes.Push(std::move(n));
}

// Every job lambda captures the shared AppContext BY VALUE (same lifetime pattern
// as the rest of the app: an in-flight op must keep notes/jobs alive past teardown,
// V7-P1-3) and reports its outcome through app->notes -> shell toasts.

inline std::function<void()> MakeKillJob(std::shared_ptr<AppContext> app, const ConfirmRequest& req) {
    const bool elevated = app->elevated;
    return [app, elevated, req] {
        std::wstring err;
        if (ops::TerminateProcessById(req.key, &err)) {
            PostConfirmNote(*app, Notification::Kind::JobDone,
                            Fmt(L"已终止进程 {} ({})", req.name, req.key.pid));
        } else {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"终止 {} ({}) 失败：{}{}", req.name, req.key.pid,
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
        }
    };
}

// The dialog's plan (planCount) is only a preview; execution re-plans under the
// ops worker at act-time like the ProcessOps contract requires.
inline std::function<void()> MakeKillTreeJob(std::shared_ptr<AppContext> app,
                                             const ConfirmRequest& req) {
    const bool elevated = app->elevated;
    return [app, elevated, req] {
        ops::TreeResult r;
        std::wstring err;
        if (!ops::TerminateTree(req.key, &r, &err)) {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"终止进程树 {} ({}) 失败：{}{}", req.name, req.key.pid,
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
            return;
        }
        std::wstring text = Fmt(L"已终止进程树 {} ({})：终止 {} 个", req.name, req.key.pid,
                                r.terminated);
        if (r.skippedProtected > 0) text += Fmt(L"，跳过保护进程 {} 个", r.skippedProtected);
        if (r.failed > 0) text += Fmt(L"，失败 {} 个", r.failed);
        PostConfirmNote(*app, r.failed > 0 ? Notification::Kind::Warn : Notification::Kind::JobDone,
                        text);
    };
}

inline std::function<void()> MakeTrimWorkingSetJob(std::shared_ptr<AppContext> app,
                                                   const ConfirmRequest& req) {
    const bool elevated = app->elevated;
    return [app, elevated, req] {
        std::wstring err;
        if (ops::TrimWorkingSet(req.key, &err)) {
            PostConfirmNote(*app, Notification::Kind::JobDone,
                            Fmt(L"已请求释放 {} ({}) 的工作集内存", req.name, req.key.pid));
        } else {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"释放 {} ({}) 工作集失败：{}{}", req.name, req.key.pid,
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
        }
    };
}

inline std::function<void()> MakePurgeStandbyJob(std::shared_ptr<AppContext> app) {
    const bool elevated = app->elevated;
    return [app, elevated] {
        std::wstring err;
        if (ops::PurgeStandbyList(&err)) {
            PostConfirmNote(*app, Notification::Kind::JobDone, L"已清理系统待机列表");
        } else {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"清理待机列表失败：{}{}",
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
        }
    };
}

inline std::function<void()> MakeStartupToggleJob(std::shared_ptr<AppContext> app,
                                                  const ConfirmRequest& req) {
    const bool elevated = app->elevated;
    return [app, elevated, req] {
        std::wstring err;
        if (ops::SetStartupEnabled(req.startupItem, req.startupEnable, &err)) {
            PostConfirmNote(*app, Notification::Kind::JobDone,
                            Fmt(L"已{}启动项「{}」", req.startupEnable ? L"启用" : L"禁用",
                                req.startupItem.name));
        } else {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"{}启动项「{}」失败：{}{}", req.startupEnable ? L"启用" : L"禁用",
                                req.startupItem.name,
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
        }
    };
}

// ---------------------------------------------------------------------------
// F4#2: process control jobs (suspend/resume/priority/affinity). Same protocol:
// ops re-verifies (pid, createTime) identity and refuses protected processes.
// ---------------------------------------------------------------------------

inline std::wstring ControlTargetLabel(const ConfirmRequest& req) {
    return Fmt(L"{} ({})", req.name, req.key.pid);
}

inline std::function<void()> MakeSuspendJob(std::shared_ptr<AppContext> app,
                                            const ConfirmRequest& req) {
    const bool elevated = app->elevated;
    return [app, elevated, req] {
        std::wstring err;
        if (ops::SuspendProcess(req.key, &err)) {
            PostConfirmNote(*app, Notification::Kind::JobDone,
                            Fmt(L"已挂起进程 {}", ControlTargetLabel(req)));
        } else {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"挂起 {} 失败：{}{}", ControlTargetLabel(req),
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
        }
    };
}

inline std::function<void()> MakeResumeJob(std::shared_ptr<AppContext> app,
                                           const ConfirmRequest& req) {
    const bool elevated = app->elevated;
    return [app, elevated, req] {
        std::wstring err;
        if (ops::ResumeProcess(req.key, &err)) {
            PostConfirmNote(*app, Notification::Kind::JobDone,
                            Fmt(L"已恢复进程 {}", ControlTargetLabel(req)));
        } else {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"恢复 {} 失败：{}{}", ControlTargetLabel(req),
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
        }
    };
}

inline std::function<void()> MakeSetPriorityJob(std::shared_ptr<AppContext> app,
                                                const ConfirmRequest& req) {
    const bool elevated = app->elevated;
    return [app, elevated, req] {
        std::wstring err;
        if (ops::SetProcPriority(req.key, req.priority, &err)) {
            PostConfirmNote(*app, Notification::Kind::JobDone,
                            Fmt(L"已将 {} 优先级设为「{}」", ControlTargetLabel(req),
                                ui3::PriorityLabel(req.priority)));
        } else {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"设置 {} 优先级失败：{}{}", ControlTargetLabel(req),
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
        }
    };
}

inline std::function<void()> MakeSetAffinityJob(std::shared_ptr<AppContext> app,
                                                const ConfirmRequest& req) {
    const bool elevated = app->elevated;
    return [app, elevated, req] {
        std::wstring err;
        if (ops::SetProcAffinity(req.key, req.affinityMask, &err)) {
            PostConfirmNote(*app, Notification::Kind::JobDone,
                            Fmt(L"已将 {} 的处理器亲和性设为 0x{:X}", ControlTargetLabel(req),
                                req.affinityMask));
        } else {
            PostConfirmNote(*app, Notification::Kind::JobFailed,
                            Fmt(L"设置 {} 亲和性失败：{}{}", ControlTargetLabel(req),
                                err.empty() ? std::wstring(L"未知错误") : err,
                                AdminHintSuffix(elevated)));
        }
    };
}

// ---------------------------------------------------------------------------
// P3 任务一：一键内存优化 —— 确认后以「单个 job」批量执行：逐项 TrimWorkingSet
// （ops 内照常做 createTime 身份复核 + 保护名单硬门禁；单项失败只计数不中断），
// 可选附加一次 PurgeStandbyList（失败如实补发单独 note）。结果聚合成一条 toast。
// ---------------------------------------------------------------------------

inline std::function<void()> MakeMemCleanupJob(std::shared_ptr<AppContext> app,
                                               std::vector<ui3::CleanupCandidate> items,
                                               std::vector<bool> selected,
                                               bool purgeStandby) {
    const bool elevated = app->elevated;
    return [app, elevated, items = std::move(items), selected = std::move(selected),
            purgeStandby] {
        ui3::CleanupOutcome out;
        const size_t n = std::min(items.size(), selected.size());
        for (size_t i = 0; i < n; ++i) {
            if (!selected[i]) continue;
            std::wstring err;
            const bool ok = ops::TrimWorkingSet(items[i].key, &err);
            const uint64_t bytes =
                items[i].privateWorkingSet == kUnavailU64 ? 0 : items[i].privateWorkingSet;
            ui3::RecordTrimResult(out, ok, bytes);
            if (!ok && out.firstError.empty()) out.firstError = err;  // V15-P2-2
        }
        if (purgeStandby) {
            out.purgeAttempted = true;
            std::wstring err;
            out.purgeOk = ops::PurgeStandbyList(&err);
            if (!out.purgeOk) {
                PostConfirmNote(*app, Notification::Kind::JobFailed,
                                Fmt(L"清理系统待机缓存失败：{}{}",
                                    err.empty() ? std::wstring(L"未知错误") : err,
                                    AdminHintSuffix(elevated)));
            }
        }
        PostConfirmNote(*app, out.failed > 0 ? Notification::Kind::Warn
                                             : Notification::Kind::JobDone,
                        ui3::FormatCleanupDoneText(out) +
                            (out.firstError.empty()
                                 ? std::wstring()
                                 : Fmt(L"（首个失败：{}{}）", out.firstError,
                                       AdminHintSuffix(elevated))));
    };
}

// Execute the confirmed action: the ONLY entry point a confirm-dialog confirm
// button should call. Returns true when the op job was queued; a false return is
// always accompanied by an immediately-posted JobFailed notification (never silent).
inline bool ExecuteConfirmedAction(std::shared_ptr<AppContext> ctx, const ConfirmRequest& req) {
    if (!ctx) return false;  // app already torn down; no notes queue left to inform
    std::function<void()> job;
    switch (req.kind) {
        case ConfirmKind::Kill: job = MakeKillJob(ctx, req); break;
        case ConfirmKind::KillTree: job = MakeKillTreeJob(ctx, req); break;
        case ConfirmKind::TrimWorkingSet: job = MakeTrimWorkingSetJob(ctx, req); break;
        case ConfirmKind::PurgeStandby: job = MakePurgeStandbyJob(ctx); break;
        case ConfirmKind::StartupToggle: job = MakeStartupToggleJob(ctx, req); break;
        case ConfirmKind::Suspend: job = MakeSuspendJob(ctx, req); break;
        case ConfirmKind::Resume: job = MakeResumeJob(ctx, req); break;
        case ConfirmKind::SetPriority: job = MakeSetPriorityJob(ctx, req); break;
        case ConfirmKind::SetAffinity: job = MakeSetAffinityJob(ctx, req); break;
        case ConfirmKind::MemCleanup:
            job = MakeMemCleanupJob(ctx, req.cleanupItems, req.cleanupSelected,
                                    req.cleanupPurgeStandby);
            break;
        case ConfirmKind::CloseAsk:
            // 关闭行为不是 ops job：三个按钮由 DrawConfirmDialogs 直接处理
            // （wantExit / ShowWindow(SW_HIDE)），不经本执行表。
            return false;
        case ConfirmKind::None:
        default: return false;
    }
    // Submit returns 0 when the queue is not running (teardown): never silent.
    if (ctx->jobs.Submit(std::move(job)) == 0) {
        PostConfirmNote(*ctx, Notification::Kind::JobFailed,
                        L"操作队列未运行，指令未能提交（应用可能正在退出）");
        return false;
    }
    return true;
}

}  // namespace ui
}  // namespace stm

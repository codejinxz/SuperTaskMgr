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
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include "app/AppContext.h"
#include "core/ProcData.h"
#include "core/Str.h"
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
};

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

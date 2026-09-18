#pragma once
// Page registry + app shell chrome (toolbar / tab bar / status bar).
// NOTE: Pages.cpp is owned by the UI developer agent from phase 2 onward;
// the shell only guarantees RegisterPages() + DrawShell() signatures.
#include "ops/SessionState.h"
#include <memory>
#include <windows.h>

namespace stm {

class AppContext;

void RegisterPages(AppContext& ctx);   // fill ctx.pages (tab order)
void DrawShell(AppContext& ctx);       // toolbar + tabs + status bar + active page
// main hands its owning handle to the UI layer: ops job lambdas capture this
// shared_ptr so in-flight jobs keep notes/jobs/details alive past teardown.
void BindAppContext(std::shared_ptr<AppContext> ctx);
// Session handoff helpers (arch section 7): restore after LoadSession / snapshot before exit.
void ApplySession(AppContext& ctx, const ops::SessionState& s);
void SaveSessionFromCtx(const AppContext& ctx, HWND mainWnd);

// --- --autotest dialogclick (V14): real-UI-pipeline regression for the confirm
// dialog. ArmKillConfirmForAutotest opens the kill confirm through the exact same
// RequestConfirmKill() path the row context menu uses; the dialogclick driver then
// injects synthetic io mouse events to actually click the rendered action button.
// The state exposes the last-rendered modal/button geometry so the driver can aim
// without touching the real cursor, plus a single-frame-regression sentinel
// (framesOpen counts consecutive frames the modal was submitted & interactive).
struct DialogAutotestState {
    bool modalOpen = false;    // BeginPopupModal returned true on the last frame
    int framesOpen = 0;        // consecutive submitted frames (sentinel: must be >= N)
    bool requestActive = false;// a confirm request is armed (kind != None)
    float actionMinX = 0, actionMinY = 0, actionMaxX = 0, actionMaxY = 0;
};
void ArmKillConfirmForAutotest(uint32_t pid, uint64_t createTime, const wchar_t* name);
const DialogAutotestState& DialogAutotestStateForAutotest();

}  // namespace stm

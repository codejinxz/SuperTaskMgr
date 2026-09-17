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

}  // namespace stm

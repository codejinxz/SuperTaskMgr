#pragma once
// Phase-3 UI extension: network / startup / services / drivers / sensors tabs
// plus the optional threshold alert watcher. Registered on top of the phase-2
// pages by appending to RegisterPages (app/ui/Pages.cpp).
#include <functional>
#include <memory>
#include <string>
#include "app/AppContext.h"

namespace stm {
namespace ui3 {

// Page registration (called from RegisterPages; appends after perf).
void RegisterPhase3Pages(AppContext& ctx);

// Shared context handle for ops jobs created by the phase-3 pages (lifetime
// pattern identical to BindAppContext). main.cpp calls this once at startup.
void BindPhase3Context(std::shared_ptr<AppContext> ctx);
std::shared_ptr<AppContext> LiveP3Ctx();

// Shell hook, called once per frame from DrawShell: threshold alert watcher
// (default off; cfg keys alertOn / alertCpu / alertMem).
void AlertTick(AppContext& ctx);

// Perf-page hook: compact alert on/off + threshold controls (appended at the
// bottom of the performance page; purely additive).
void DrawAlertControls(AppContext& ctx);

// Smoke support: when enabled (--smoke), DrawSmokeAllPages draws every page
// once per frame into an offscreen window so the empty/error states of the
// new tabs are exercised by SuperTaskMgr.exe --smoke.
void SetSmokeDrawAll(bool on);
void DrawSmokeAllPages(AppContext& ctx);

// Tray balloon sink for alerts (wired by main.cpp to Tray::ShowBalloon; unset
// under --smoke, where alerts degrade to toasts only).
using BalloonSink = std::function<void(const std::wstring& title, const std::wstring& text)>;
void SetBalloonSink(BalloonSink sink);

}  // namespace ui3
}  // namespace stm

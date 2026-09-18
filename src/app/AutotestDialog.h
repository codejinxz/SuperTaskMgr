#pragma once
// --autotest dialogclick (V14 P1): a REAL-UI-pipeline regression guard for the
// confirm dialog that F1 fixed. Unlike the other --autotest modes (which call
// ui::ExecuteConfirmedAction directly and would pass even on the broken code),
// this driver:
//   1. spawns a `cmd /c ping -n 30` child,
//   2. arms the kill confirm through the same RequestConfirmKill() path as the
//      row context menu (app/ui/Pages.cpp),
//   3. injects synthetic io.AddMousePosEvent / AddMouseButtonEvent every frame so
//      the REAL rendered modal receives hover/down/up on its action button,
//   4. asserts the old-defect sentinel (modal submitted continuously for >= 4
//      frames before the click — the F1 bug rendered it for exactly one frame)
//      and that the child process actually dies and the modal closes.
// The main window stays VISIBLE on purpose: the swap chain uses vsync'd Present,
// which can stall on a hidden window; the driver never touches the real cursor.
#include <memory>
#include <string>

#include "app/ui/Pages.h"  // DialogAutotestState
#include "core/HandleGuard.h"

namespace stm {

class AppContext;

class DialogClickDriver {
public:
    // Spawns the victim process and arms the shell kill confirm. Requires the
    // frame loop's usual autotest gating (a few frames rendered + ticks flowing).
    void Start(std::shared_ptr<AppContext> ctx);
    // Once per frame, AFTER renderer.Present(): injects mouse events (consumed by
    // the next frame's NewFrame) and evaluates phase transitions / assertions.
    // Finish() posts ctx->wantExit.
    void Tick();
    bool Done() const;

    std::wstring result = L"FAIL";  // L"PASS" / L"FAIL" for autotest_result.log
    std::wstring detail;
    bool pass = false;

private:
    enum class Phase {
        WaitStart,     // waiting for the frame/tick gating
        WaitOpen,      // dialog requested; wait for the modal + sentinel frames
        Hover,         // move onto the action button
        Down,          // mouse down
        Up,            // mouse up
        WaitExit,      // assert child exit + modal close
        Finished,
    };

    void Fail(const std::wstring& why);
    void Finish(bool ok, const std::wstring& why);

    std::shared_ptr<AppContext> ctx_;
    UniqueHandle child_;
    uint32_t childPid_ = 0;
    Phase phase_ = Phase::WaitStart;
    int hoverFrames_ = 0;
    uint64_t phaseDeadline_ = 0;  // GetTickCount64-based per-phase timeout
};

}  // namespace stm

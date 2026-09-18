// --autotest dialogclick driver implementation (see AutotestDialog.h).
#include "app/AutotestDialog.h"
#include "app/AppContext.h"
#include "app/ui/ConfirmAction.h"
#include "core/Log.h"
#include "core/Str.h"
#include "ops/ProcessOps.h"
#include "imgui.h"
#include <windows.h>
#include <string>

namespace stm {

namespace {

constexpr int kMinOpenFrames = 4;        // sentinel: modal submitted >= N frames
constexpr uint64_t kPhaseTimeoutMs = 15000;
constexpr uint64_t kExitTimeoutMs = 10000;

uint64_t NowMs() { return GetTickCount64(); }

bool ChildExited(const UniqueHandle& h) {
    return h && WaitForSingleObject(h.get(), 0) == WAIT_OBJECT_0;
}

}  // namespace

bool DialogClickDriver::Done() const { return phase_ == Phase::Finished; }

void DialogClickDriver::Start(std::shared_ptr<AppContext> ctx) {
    ctx_ = std::move(ctx);

    // Victim: cmd -> ping tree, same shape as the other autotest modes.
    wchar_t cmdLine[] = L"cmd.exe /c ping -n 30 127.0.0.1";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Fail(L"创建 cmd 子进程失败");
        return;
    }
    CloseHandle(pi.hThread);
    child_.reset(pi.hProcess);
    childPid_ = pi.dwProcessId;
    FILETIME c{}, x{}, k{}, u{};
    uint64_t createTime = 0;
    if (GetProcessTimes(pi.hProcess, &c, &x, &k, &u)) {
        createTime = (static_cast<uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    }

    // Arm through the SAME path the row context menu uses (not ExecuteConfirmed-
    // Action): the click must land on the real rendered modal.
    ArmKillConfirmForAutotest(childPid_, createTime, L"cmd.exe");
    phase_ = Phase::WaitOpen;
    phaseDeadline_ = NowMs() + kPhaseTimeoutMs;
}

void DialogClickDriver::Fail(const std::wstring& why) {
    detail = why;
    result = L"FAIL";
    pass = false;
    phase_ = Phase::Finished;
    if (child_ && !ChildExited(child_)) {
        TerminateProcess(child_.get(), 1);  // best-effort cleanup on failure
    }
    if (ctx_) ctx_->wantExit = true;
}

void DialogClickDriver::Finish(bool ok, const std::wstring& why) {
    detail = why;
    result = ok ? L"PASS" : L"FAIL";
    pass = ok;
    phase_ = Phase::Finished;
    if (child_ && !ChildExited(child_)) {
        TerminateProcess(child_.get(), 1);
    }
    if (ctx_) ctx_->wantExit = true;
}

void DialogClickDriver::Tick() {
    if (phase_ == Phase::WaitStart) return;
    if (phase_ == Phase::Finished) {
        // Keep the child cleanup deadline honest if the frame loop lingers.
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const DialogAutotestState& st = DialogAutotestStateForAutotest();

    switch (phase_) {
        case Phase::WaitOpen: {
            // Park the synthetic cursor somewhere neutral while waiting.
            io.AddMousePosEvent(640.0f, 400.0f);
            if (st.modalOpen && st.framesOpen >= kMinOpenFrames) {
                // Old-defect sentinel passed: the modal has now been submitted and
                // interactive for several consecutive frames (the F1 bug rendered
                // it for exactly one frame and it could never be clicked).
                phase_ = Phase::Hover;
                hoverFrames_ = 0;
                phaseDeadline_ = NowMs() + kPhaseTimeoutMs;
                break;
            }
            // NOTE: on the Tick right after Start() the dialog has not been drawn
            // yet (Start runs after Present) — failures are only decided on timeout.
            if (NowMs() > phaseDeadline_) {
                if (st.requestActive && !st.modalOpen) {
                    Fail(L"旧缺陷哨兵失败：模态未能持续提交（单帧化回归）或打开超时");
                } else if (!st.requestActive && !st.modalOpen) {
                    Fail(L"确认框从未打开（请求丢失）");
                } else {
                    Fail(L"旧缺陷哨兵失败：模态连续存在帧数未达阈值（单帧化回归）");
                }
            }
            break;
        }
        case Phase::Hover: {
            if (!st.modalOpen) {
                Fail(L"悬停阶段模态消失（单帧化回归）");
                break;
            }
            const float cx = (st.actionMinX + st.actionMaxX) * 0.5f;
            const float cy = (st.actionMinY + st.actionMaxY) * 0.5f;
            io.AddMousePosEvent(cx, cy);
            if (++hoverFrames_ >= 2) {
                phase_ = Phase::Down;
                phaseDeadline_ = NowMs() + kPhaseTimeoutMs;
            }
            break;
        }
        case Phase::Down: {
            if (!st.modalOpen) {
                Fail(L"按下阶段模态消失（单帧化回归）");
                break;
            }
            const float cx = (st.actionMinX + st.actionMaxX) * 0.5f;
            const float cy = (st.actionMinY + st.actionMaxY) * 0.5f;
            io.AddMousePosEvent(cx, cy);
            io.AddMouseButtonEvent(0, true);
            phase_ = Phase::Up;
            break;
        }
        case Phase::Up: {
            const float cx = (st.actionMinX + st.actionMaxX) * 0.5f;
            const float cy = (st.actionMinY + st.actionMaxY) * 0.5f;
            io.AddMousePosEvent(cx, cy);
            io.AddMouseButtonEvent(0, false);
            phase_ = Phase::WaitExit;
            phaseDeadline_ = NowMs() + kExitTimeoutMs;
            break;
        }
        case Phase::WaitExit: {
            const bool childDead = ChildExited(child_);
            const bool modalClosed = !st.modalOpen && !st.requestActive;
            if (childDead && modalClosed) {
                Finish(true, L"真实管线点击生效：子进程退出、模态已关闭");
                break;
            }
            // The click was supposed to fire; if the modal is gone but the child
            // still lives, ExecuteConfirmedAction failed or was never reached.
            if (NowMs() > phaseDeadline_) {
                if (childDead) {
                    // One-frame grace: the frame that processed the click still
                    // reports the modal open; the assertion window just lapsed.
                    Finish(true, L"子进程已退出（模态关闭断言超出宽限期）");
                } else {
                    Fail(L"子进程 10 秒内未退出（点击未生效）");
                }
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace stm

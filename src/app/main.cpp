// Application entry: single-instance, session handoff, service wiring, message loop.
#include "app/AppContext.h"
#include "app/AutotestDialog.h"
#include "app/D3DRenderer.h"
#include "app/ImGuiLayer.h"
#include "app/Theme.h"
#include "app/Win32Window.h"
#include "app/ui/ConfirmAction.h"
#include "app/ui/Pages.h"
#include "app/ui/Tray.h"
#include "app/ui3/GcPages.h"  // F4: 热键绑定 + 新页注册依赖的共享入口
#include "app/ui3/Pages3.h"
#include "core/FsUtil.h"
#include "core/HandleGuard.h"
#include "core/Log.h"
#include "core/Str.h"
#include "ops/Elevate.h"
#include "ops/SessionState.h"
#include "ops/SingleInstance.h"
#include "ops/StartupOps.h"
#include "core/Privilege.h"
#include <memory>
#include <shellapi.h>
#include <string>
#include <tlhelp32.h>

namespace stm {
namespace {

int ParseSmokeFrames() {
    int nArgs = 0;
    int smoke = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (args) {
        for (int i = 1; i + 1 < nArgs; ++i) {
            if (wcscmp(args[i], L"--smoke") == 0) smoke = _wtoi(args[i + 1]);
        }
        LocalFree(args);
    }
    return smoke;
}

// --autotest kill|tree|startup (bug F1 verification aid): after the UI is up, the
// frame loop executes the SAME ui::ExecuteConfirmedAction() call a confirm button
// makes, writes a PASS/FAIL log next to the app log, and exits. For quick manual
// re-verification on a machine without a selftest binary.
std::wstring ParseAutotestMode() {
    int nArgs = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    std::wstring mode;
    if (args) {
        for (int i = 1; i + 1 < nArgs; ++i) {
            if (wcscmp(args[i], L"--autotest") == 0) mode = args[i + 1];
        }
        LocalFree(args);
    }
    return mode;
}

uint32_t ClampInterval(int64_t v) {
    if (v < 500) return 500;
    if (v > 5000) return 5000;
    return static_cast<uint32_t>(v);
}

// F4#10: 托盘左键与全局热键共用的主窗显隐三分支（最小化=>还原并置前；
// 可见=>隐藏；否则显示并置前，P1-2）。
void ToggleMainWindowVisible(HWND hwnd) {
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    } else if (IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
}

// ---------------- autotest helpers (mirrors src/selftest/ui_fix_test.cpp) ----

uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

struct SpawnedProc {
    UniqueHandle proc;
    uint32_t pid = 0;
    uint64_t createTime = 0;
    bool Exited() const { return proc && WaitForSingleObject(proc.get(), 0) == WAIT_OBJECT_0; }
};

bool SpawnPingCmd(int n, SpawnedProc* out) {
    wchar_t cmdLine[128];
    swprintf_s(cmdLine, L"cmd.exe /c ping -n %d 127.0.0.1", n);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    out->proc.reset(pi.hProcess);
    out->pid = pi.dwProcessId;
    FILETIME c{}, x{}, k{}, u{};
    if (GetProcessTimes(pi.hProcess, &c, &x, &k, &u)) out->createTime = FileTimeToU64(c);
    return true;
}

uint32_t FindChildPid(uint32_t parentPid, const wchar_t* name) {
    HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) return 0;
    UniqueHandle snap(raw);
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap.get(), &pe)) return 0;
    do {
        if (pe.th32ParentProcessID == parentPid && _wcsicmp(pe.szExeFile, name) == 0) {
            return pe.th32ProcessID;
        }
    } while (Process32NextW(snap.get(), &pe));
    return 0;
}

template <typename Pred>
bool PollUntil(Pred pred, uint32_t timeoutMs) {
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (pred()) return true;
        if (GetTickCount64() >= deadline) return pred();
        Sleep(100);
    }
}

void DrainNotesInto(std::wstring* text, AppContext& ctx) {
    std::vector<Notification> out;
    ctx.notes.Drain(&out);
    for (const Notification& n : out) {
        *text += Fmt(L"\n  [{}]", n.kind == Notification::Kind::JobFailed ? L"失败"
                          : n.kind == Notification::Kind::Warn ? L"警告" : L"完成") +
                 L" " + n.text;
    }
}

void WriteAutotestLine(const std::wstring& mode, const std::wstring& result,
                       const std::wstring& detail);

// Runs the requested autotest and writes a human-readable log line. Returns 0 on PASS.
// Takes the shared context by value: the ops job lambdas capture it like every other
// submit path (V7-P1-3).
int RunAutotest(std::shared_ptr<AppContext> ctx, const std::wstring& mode) {
    std::wstring result = L"FAIL";
    std::wstring detail;

    if (mode == L"kill" || mode == L"tree") {
        SpawnedProc p;
        if (!SpawnPingCmd(30, &p)) {
            detail = L"创建 cmd 子进程失败";
        } else {
            ui::ConfirmRequest req;
            req.kind = mode == L"kill" ? ui::ConfirmKind::Kill : ui::ConfirmKind::KillTree;
            req.key = ProcKey{p.pid, p.createTime};
            req.pid = p.pid;
            req.name = L"cmd.exe";
            UniqueHandle ping;
            if (mode == L"tree") {
                uint32_t pingPid = 0;
                PollUntil([&] { return (pingPid = FindChildPid(p.pid, L"ping.exe")) != 0; }, 3000);
                if (pingPid) ping.reset(OpenProcess(SYNCHRONIZE, FALSE, pingPid));
            }
            const bool accepted = ui::ExecuteConfirmedAction(ctx, req);
            if (!accepted) {
                detail = L"ExecuteConfirmedAction 拒绝请求";
            } else {
                const bool rootOk = PollUntil([&] { return p.Exited(); }, 8000);
                bool treeOk = true;
                if (mode == L"tree" && ping) {
                    treeOk = WaitForSingleObject(ping.get(), 8000) == WAIT_OBJECT_0;
                }
                result = rootOk && treeOk ? L"PASS" : L"FAIL";
                if (!rootOk) detail = L"8 秒内根进程未退出";
                if (!treeOk) detail += rootOk ? L"8 秒内 ping 子进程未退出" : L"，ping 子进程未退出";
            }
            TerminateProcess(p.proc.get(), 1);  // best-effort cleanup on failure
        }
    } else if (mode == L"startup") {
        constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        constexpr wchar_t kValue[] = L"stm_f1_autotest";
        constexpr wchar_t kApproved[] =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        HKEY raw = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_SET_VALUE, nullptr, &raw, nullptr) != ERROR_SUCCESS) {
            detail = L"无法打开 HKCU Run 键";
        } else {
            const std::wstring cmd = L"C:\\Windows\\System32\\notepad.exe";
            RegSetValueExW(raw, kValue, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(cmd.c_str()),
                           static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(raw);
            ui::ConfirmRequest req;
            req.kind = ui::ConfirmKind::StartupToggle;
            req.startupEnable = false;
            req.startupItem.source = ops::StartupSource::RegRun;
            req.startupItem.id = std::wstring(L"HKCU\\") + kRunKey + L"\\" + kValue;
            req.startupItem.name = kValue;
            req.startupItem.command = cmd;
            req.startupItem.location = std::wstring(L"HKCU\\") + kRunKey;
            req.startupItem.enabled = true;
            req.startupItem.canToggle = true;
            if (!ui::ExecuteConfirmedAction(ctx, req)) {
                detail = L"ExecuteConfirmedAction 拒绝请求";
            } else {
                BYTE low = 0xFF;
                const bool disabled = PollUntil([&] {
                    HKEY k = nullptr;
                    BYTE buf[16]{};  // 12-byte REG_BINARY: buffer must not be smaller
                    DWORD size = sizeof(buf);
                    const bool got =
                        RegOpenKeyExW(HKEY_CURRENT_USER, kApproved, 0, KEY_QUERY_VALUE, &k) ==
                            ERROR_SUCCESS &&
                        RegQueryValueExW(k, kValue, nullptr, nullptr, buf, &size) == ERROR_SUCCESS;
                    if (k) RegCloseKey(k);
                    if (!got) return false;
                    low = buf[0];
                    return (low & 1) == 1;
                }, 8000);
                result = disabled ? L"PASS" : L"FAIL";
                if (!disabled) detail = L"确认禁用后 StartupApproved 值未变为禁用编码";
            }
            // cleanup both values regardless of outcome
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &raw) == ERROR_SUCCESS) {
                RegDeleteValueW(raw, kValue);
                RegCloseKey(raw);
            }
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kApproved, 0, KEY_SET_VALUE, &raw) == ERROR_SUCCESS) {
                RegDeleteValueW(raw, kValue);
                RegCloseKey(raw);
            }
        }
    } else {
        detail = L"未知 --autotest 模式（支持 kill|tree|startup）";
    }

    DrainNotesInto(&detail, *ctx);  // log the exact toast text the UI would show
    WriteAutotestLine(mode, result, detail);
    return result == L"PASS" ? 0 : 1;
}

// Appends one flushed line to %LOCALAPPDATA%\SuperTaskMgr\logs\autotest_result.log
// and mirrors it into stm.log. Shared by all --autotest modes.
void WriteAutotestLine(const std::wstring& mode, const std::wstring& result,
                       const std::wstring& detail) {
    const std::wstring line =
        Fmt(L"[autotest:{}] {}{}", mode, result, detail.empty() ? L"" : L" - " + detail);
    STM_LOG_INFO("app", L"{}", line);
    const std::wstring logPath = LogDir() + L"\\autotest_result.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, logPath.c_str(), L"ab") == 0 && f) {
        const std::string u8 = WideToUtf8(line);
        fwrite(u8.data(), 1, u8.size(), f);
        fwrite("\n", 1, 1, f);
        fclose(f);
    }
}

}  // namespace
}  // namespace stm

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int cmdShow) {
    using namespace stm;
    const int smokeFrames = ParseSmokeFrames();
    const std::wstring autotestMode = ParseAutotestMode();
    const bool headless = smokeFrames > 0 || !autotestMode.empty();

    LogInit(LogDir());
    STM_LOG_INFO("app", L"启动（phase2 UI, elevated={}, smoke={}, autotest={}）",
                 IsProcessElevated() ? L"1" : L"0", smokeFrames,
                 autotestMode.empty() ? L"-" : autotestMode);

    ops::SingleInstance si;
    // 3s wait: elevation relaunch hands the mutex over while the old instance tears
    // down; a slow teardown must not make the new instance bail (final review V11-P2-6).
    if (!si.TryAcquire(3000)) {
        if (headless) return 1;  // CI: never block on a MessageBox (V11-P2-7)
        MessageBoxW(nullptr, L"超级任务管理器已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    ops::SessionState session;
    const bool hasSession = ops::LoadSession(&session);

    // V7-P1-3: AppContext lives in a shared_ptr. Ops job lambdas capture this handle
    // by value (registered via BindAppContext), so a job still running after
    // JobQueue::Shutdown's wait timeout (worker detached) keeps notes/jobs/details
    // alive until it finishes — the queues' lifetime is extended with the context
    // instead of being freed under a detached worker. If the last reference drops
    // on the detached worker thread itself, ~JobQueue is still safe: after detach
    // the thread object is non-joinable and Shutdown(0) returns without joining.
    const std::shared_ptr<AppContext> ctx = std::make_shared<AppContext>();
    ctx->elevated = ops::IsElevated();
    ctx->details = std::make_unique<ops::DetailsProvider>(ctx->jobs, ctx->notes);
    if (!ctx->cfg.Load(ConfigPath())) {
        STM_LOG_INFO("app", L"配置缺失或损坏，使用默认设置");
    }

    RegisterPages(*ctx);
    if (hasSession) ApplySession(*ctx, session);
    BindAppContext(ctx);
    ui3::BindPhase3Context(ctx);  // phase-3 pages use the same lifetime pattern
    if (!ctx->collect.Start(hasSession ? ClampInterval(session.intervalMs)
                                       : ClampInterval(ctx->cfg.GetInt(L"intervalMs", 1000)))) {
        MessageBoxW(nullptr, L"采集服务启动失败。", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!ctx->jobs.Start()) {
        ctx->collect.Stop();
        MessageBoxW(nullptr, L"操作线程启动失败。", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }

    MainWindow::EnableDpiAwareness();
    D3DRenderer renderer;
    MainWindow win;
    WindowCallbacks cbs;
    cbs.quit = &ctx->wantExit;
    cbs.onResize = [](void* ud, int w, int h) { static_cast<D3DRenderer*>(ud)->Resize(w, h); };
    ui::Tray tray;
    // Tray messages ride the window-proc hook (Win32Window exposes no other way in).
    // F4#10: WM_HOTKEY（全局热键 Ctrl+Alt+M）也在此处理，复用托盘三分支显隐逻辑。
    cbs.onMessage = [&tray, &win](HWND h, UINT msg, WPARAM wParam, LPARAM lParam,
                                  bool* handled) -> LRESULT {
        if (msg == WM_HOTKEY && wParam == static_cast<WPARAM>(ui3::kGcHotkeyId)) {
            ToggleMainWindowVisible(win.Hwnd());
            *handled = true;
            return 0;
        }
        return tray.HandleMessage(h, msg, wParam, lParam, handled);
    };
    cbs.ud = &renderer;
    if (!win.Create(inst, L"超级任务管理器", 1280, 800, cmdShow, cbs)) {
        STM_LOG_ERROR("app", L"窗口创建失败");
        return 1;
    }
    if (!renderer.Init(win.Hwnd(), win.Width(), win.Height())) {
        MessageBoxW(nullptr, L"D3D11 初始化失败（含 WARP 兜底）。", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    ImGuiLayer ui;
    if (!ui.Init(win.Hwnd(), &renderer)) return 1;
    Theme::Apply();

    // Tray icon: skipped under --smoke / --autotest (CI renders headless-ish, no
    // shell icon churn).
    if (!headless && tray.Create(win.Hwnd(), ctx->elevated)) {
        const HWND hwnd = win.Hwnd();
        tray.onToggleWindow = [hwnd]() {
            // P1-2 (F2 review): a minimized window still reports IsWindowVisible()==TRUE,
            // so the old two-branch check hid the taskbar button on left click. Three
            // branches: minimized => restore+foreground; visible => hide; else show.
            ToggleMainWindowVisible(hwnd);
        };
        tray.onShowWindow = [hwnd]() {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        };
        // Capture the shared_ptr by value so these callbacks stay lifetime-safe
        // regardless of teardown ordering.
        tray.onRestartElevated = [ctx, &win]() {
            SaveSessionFromCtx(*ctx, win.Hwnd());
            if (ops::RelaunchAsAdmin(L"--relaunched")) ctx->wantExit = true;
        };
        tray.onExit = [ctx]() { ctx->wantExit = true; };
        // Phase-3 alerts fire tray balloons through the same icon (additive).
        ui3::SetBalloonSink([&tray](const std::wstring& title, const std::wstring& text) {
            tray.ShowBalloon(title, text);
        });
    }
    // Under --smoke every page's Draw is exercised in an offscreen window so
    // the new tabs' empty/error states are covered by the CI smoke run.
    ui3::SetSmokeDrawAll(smokeFrames > 0);

    // F4#10: 全局热键挂到主窗（UI 线程 == 窗口线程，RegisterHotKey 合法）；
    // headless（--smoke/--autotest）不注册，避免 CI 侧副作用。失败只记日志。
    ui3::GcHotkeyBindWindow(win.Hwnd());
    if (!headless && ctx->cfg.GetBool(L"hotkeyEnabled", false)) {
        std::wstring hkErr;
        if (!ui3::GcHotkeySetEnabled(true, &hkErr)) {
            ctx->cfg.SetBool(L"hotkeyEnabled", false);
            STM_LOG_WARN("app", L"全局热键注册失败：{}", hkErr);
        }
    }

    if (hasSession && session.winW > 100 && session.winH > 100) {
        MoveWindow(win.Hwnd(), session.winX, session.winY, session.winW, session.winH, FALSE);
    }

    // Frame loop: vsync-paced; collection happens on its own thread.
    // Minimized => relax the collection cadence to 2 s (phase-4 budget item).
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    int frames = 0;
    int autotestFrame = 0;
    int autotestResult = 0;
    bool autotestGateOpen = false;
    bool dialogClickStarted = false;
    bool dialogClickLogged = false;
    DialogClickDriver dialogClick;
    bool wasIconic = IsIconic(win.Hwnd()) != FALSE;
    while (!ctx->wantExit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (ctx->wantExit) break;

        const bool iconic = IsIconic(win.Hwnd()) != FALSE;
        if (iconic != wasIconic) {
            // Restore reads cfg live so toolbar interval changes made while running
            // survive a minimize/restore cycle (final review V11-P1-2).
            ctx->collect.SetInterval(iconic ? 2000 : ClampInterval(ctx->cfg.GetInt(L"intervalMs", 1000)));
            wasIconic = iconic;
        }

        QueryPerformanceCounter(&t0);
        renderer.BeginFrame();
        ui.NewFrame();
        DrawShell(*ctx);  // drains notifications into toasts; one snapshot read per frame
        ui.Render();
        renderer.Present();
        QueryPerformanceCounter(&t1);
        ctx->frameMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;

        if (smokeFrames > 0 && ++frames >= smokeFrames) break;
        // --autotest: let a few frames run (shell + toasts live), then execute the
        // confirmed action / drive the dialog click, log, and exit.
        // Gate on the first completed collect ticks: quitting while the very first
        // tick is still in flight would block teardown's collect.Stop() join on a
        // tick that can take a long time on slow/VM GPUs — and the action itself
        // wants a real snapshot to exist anyway.
        if (!autotestMode.empty()) {
            if (!autotestGateOpen && ++autotestFrame >= 8 && ctx->collect.TickCount() >= 2) {
                autotestGateOpen = true;
            }
            if (autotestMode == L"dialogclick") {
                // V14 P1: drive the REAL rendered confirm dialog with synthetic
                // mouse events (events queued here are consumed by next NewFrame).
                if (autotestGateOpen && !dialogClickStarted) {
                    dialogClick.Start(ctx);
                    dialogClickStarted = true;
                }
                dialogClick.Tick();
                if (dialogClick.Done() && !dialogClickLogged) {
                    autotestResult = dialogClick.pass ? 0 : 1;
                    WriteAutotestLine(L"dialogclick", dialogClick.result, dialogClick.detail);
                    dialogClickLogged = true;
                }
            } else if (autotestGateOpen) {
                autotestResult = RunAutotest(ctx, autotestMode);
                ctx->wantExit = true;
            }
        }
    }

    if (smokeFrames == 0 && autotestMode.empty()) {
        SaveSessionFromCtx(*ctx, win.Hwnd());  // window rect / page / selection handoff
        ctx->cfg.Save(ConfigPath());
    }
    tray.Remove();
    ui3::GcHotkeyUnbindWindow();   // F4#10: 退出反注册热键
    ui3::SetBalloonSink(nullptr);  // P2-7: sink captured &tray; drop before teardown
    // Stops collection and gives in-flight ops jobs up to 2 s; anything still
    // running afterwards survives on ctx's shared_ptr (see comment above).
    ctx->collect.Stop();
    ctx->jobs.Shutdown(2000);
    ui.Shutdown();
    renderer.Shutdown();
    win.Destroy();
    LogShutdown();
    return autotestResult;
}

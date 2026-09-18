// Application entry: single-instance, session handoff, service wiring, message loop.
#include "app/AppContext.h"
#include "app/D3DRenderer.h"
#include "app/ImGuiLayer.h"
#include "app/Theme.h"
#include "app/Win32Window.h"
#include "app/ui/Pages.h"
#include "app/ui/Tray.h"
#include "app/ui3/Pages3.h"
#include "core/FsUtil.h"
#include "core/Log.h"
#include "core/Str.h"
#include "ops/Elevate.h"
#include "ops/SessionState.h"
#include "ops/SingleInstance.h"
#include "core/Privilege.h"
#include <memory>
#include <shellapi.h>
#include <string>

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

uint32_t ClampInterval(int64_t v) {
    if (v < 500) return 500;
    if (v > 5000) return 5000;
    return static_cast<uint32_t>(v);
}

}  // namespace
}  // namespace stm

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int cmdShow) {
    using namespace stm;
    const int smokeFrames = ParseSmokeFrames();

    LogInit(LogDir());
    STM_LOG_INFO("app", L"启动（phase2 UI, elevated={}, smoke={}）",
                 IsProcessElevated() ? L"1" : L"0", smokeFrames);

    ops::SingleInstance si;
    // 3s wait: elevation relaunch hands the mutex over while the old instance tears
    // down; a slow teardown must not make the new instance bail (final review V11-P2-6).
    if (!si.TryAcquire(3000)) {
        if (smokeFrames > 0) return 1;  // CI: never block on a MessageBox (V11-P2-7)
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
    cbs.onMessage = [&tray](HWND h, UINT msg, WPARAM wParam, LPARAM lParam,
                            bool* handled) -> LRESULT {
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

    // Tray icon: skipped under --smoke (CI renders headless-ish, no shell icon churn).
    if (smokeFrames == 0 && tray.Create(win.Hwnd(), ctx->elevated)) {
        const HWND hwnd = win.Hwnd();
        tray.onToggleWindow = [hwnd]() {
            if (IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, SW_HIDE);
            } else {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            }
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

    if (hasSession && session.winW > 100 && session.winH > 100) {
        MoveWindow(win.Hwnd(), session.winX, session.winY, session.winW, session.winH, FALSE);
    }

    // Frame loop: vsync-paced; collection happens on its own thread.
    // Minimized => relax the collection cadence to 2 s (phase-4 budget item).
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    int frames = 0;
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
    }

    if (smokeFrames == 0) {
        SaveSessionFromCtx(*ctx, win.Hwnd());  // window rect / page / selection handoff
        ctx->cfg.Save(ConfigPath());
    }
    tray.Remove();
    ui3::SetBalloonSink(nullptr);  // P2-7: sink captured &tray; drop before teardown
    // Stops collection and gives in-flight ops jobs up to 2 s; anything still
    // running afterwards survives on ctx's shared_ptr (see comment above).
    ctx->collect.Stop();
    ctx->jobs.Shutdown(2000);
    ui.Shutdown();
    renderer.Shutdown();
    win.Destroy();
    LogShutdown();
    return 0;
}

// 应用入口：单实例、会话交接、服务装配、消息循环。
#include "app/AboutInfo.h"        // H-A: 窗口标题带版本（发布信息单一来源）
#include "app/AppContext.h"
#include "app/AutotestDialog.h"
#include "app/D3DRenderer.h"
#include "app/ImGuiLayer.h"
#include "app/Theme.h"
#include "app/Win32Window.h"
#include "app/ui/AboutUi.h"
#include "app/ui/ConfirmAction.h"
#include "app/ui/Pages.h"
#include "app/ui/Tray.h"
#include "app/ui3/GcPages.h"      // F4: 热键绑定 + 新页注册依赖的共享入口
#include "app/ui3/Pages3.h"
#include "app/ui3/ThemeCfg.h"     // H-A: 退出时剔除「恢复默认列宽」的软删除残留
#include "app/ui3/Wallpaper.h"    // Phase-6 接线: AutoRestore/DrawBackground/ClampMask
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

// --autotest kill|tree|startup（bug F1 验证辅助）：UI 起来之后，帧循环
// 执行与确认按钮相同的 ui::ExecuteConfirmedAction() 调用，在应用日志旁
// 写一条 PASS/FAIL 日志并退出。用于在没有 selftest 二进制的机器上
// 快速人工复核。
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

// ---------------- autotest 辅助（镜像 src/selftest/ui_fix_test.cpp）----

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

// 运行请求的 autotest 并写一条人读日志行。PASS 返回 0。
// 按值取共享上下文：ops 任务 lambda 与其他提交路径一样按值捕获
//（V7-P1-3）。
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
            TerminateProcess(p.proc.get(), 1);  // 失败时尽力清理
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
                    BYTE buf[16]{};  // 12 字节 REG_BINARY：缓冲不能更小
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
            // 无论结果如何都清理两个值
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
        detail = L"未知 --autotest 模式（支持 kill|tree|startup|dialogclick|about|wallpaper）";
    }

    DrainNotesInto(&detail, *ctx);  // 记录 UI 本会显示的确切 toast 文本
    WriteAutotestLine(mode, result, detail);
    return result == L"PASS" ? 0 : 1;
}

// 追加一行（带 flush）到 %LOCALAPPDATA%\SuperTaskMgr\logs\autotest_result.log，
// 并镜像到 stm.log。所有 --autotest 模式共用。
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
    // 3s 等待：提权重启在旧实例拆除期间交接互斥体；
    // 缓慢的拆除不应让新实例放弃（终审 V11-P2-6）。
    if (!si.TryAcquire(3000)) {
        if (headless) return 1;  // CI：绝不被 MessageBox 阻塞（V11-P2-7）
        MessageBoxW(nullptr, L"超级任务管理器已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    ops::SessionState session;
    const bool hasSession = ops::LoadSession(&session);

    // V7-P1-3：AppContext 存于 shared_ptr。ops 任务 lambda 按值捕获该句柄
    //（经 BindAppContext 注册），因此 JobQueue::Shutdown 等待超时后
    // 仍在运行的任务（工作线程已脱离）会让 notes/jobs/details
    // 保持存活直到完成——队列的生命周期随上下文延长，
    // 而不是在脱离的工作线程下方被释放。如果最后一个引用恰好在
    // 脱离的工作线程上释放，~JobQueue 仍然安全：detach 之后
    // 线程对象不可 join，Shutdown(0) 不做 join 直接返回。
    const std::shared_ptr<AppContext> ctx = std::make_shared<AppContext>();
    ctx->elevated = ops::IsElevated();
    ctx->details = std::make_unique<ops::DetailsProvider>(ctx->jobs, ctx->notes);
    if (!ctx->cfg.Load(ConfigPath())) {
        STM_LOG_INFO("app", L"配置缺失或损坏，使用默认设置");
    }

    RegisterPages(*ctx);
    if (hasSession) ApplySession(*ctx, session);
    BindAppContext(ctx);
    ui3::BindPhase3Context(ctx);  // 第 3 阶段页面使用同样的生命周期模式
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
    bool uiThemeReady = false;  // H-A: ImGui 上下文就绪前不响应主题系统广播
    // 托盘消息走窗口过程钩子（Win32Window 没有暴露其他入口）。
    // F4#10: WM_HOTKEY（全局热键 Ctrl+Alt+M）也在此处理，复用托盘三分支显隐逻辑。
    cbs.onMessage = [&tray, &win, &ctx, &uiThemeReady](HWND h, UINT msg, WPARAM wParam,
                                                       LPARAM lParam,
                                                       bool* handled) -> LRESULT {
        // P3 任务三：标题栏 X -> 按 cfg closeAction 分派（持久化键 closeAction，
        // 默认 0）。拦截 WM_CLOSE（置 handled，不交 DefWindowProc => 不销毁窗口）：
        //   1 = 直接退出；2 = 最小化到托盘（SW_HIDE，托盘左键可恢复）；
        //   0 = 置 pending 标志，由 DrawConfirmDialogs 每帧弹出三选一模态。
        // 注意：--smoke / --autotest 的退出路径走 ctx->wantExit，从不经过 WM_CLOSE，
        // 拦截不影响 headless 退出；托盘菜单「退出」也始终直接置 wantExit 不询问。
        // V22-P1-1：限制最小窗口宽——工具条为流式布局（窄窗会裁剪「主题…/关于」
        // 且全应用无替代入口），760px 保证全部一级按钮可点。
        if (msg == WM_GETMINMAXINFO) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 760;
            mmi->ptMinTrackSize.y = 480;
            *handled = true;
            return 0;
        }
        if (msg == WM_CLOSE) {
            const int action =
                ui::NormalizeCloseAction(ctx->cfg.GetInt(ui::kCloseActionCfgKey, 0));
            if (action == 1) {
                // 透传（不处理）：DefWindowProc 销毁窗口 =>
                // WM_DESTROY => 退出。让外部优雅关闭（不带 /F 的 taskkill）
                // 在用户选择"总是退出"时仍然有效（V18-P2-2）。
            } else if (action == 2) {
                ShowWindow(h, SW_HIDE);
                *handled = true;
                return 0;
            } else {
                ctx->closeAskPending = true;
                *handled = true;
                return 0;
            }
        }
        // H-A: 跟随系统主题实时切换 —— Windows 深浅色切换广播
        // (WM_SETTINGCHANGE, lParam=="ImmersiveColorSet")。仅当 cfg 当前模式为
        // System 时重新 Apply（实时读注册表）；不置 handled，托盘钩子照常处理。
        if (msg == WM_SETTINGCHANGE && lParam != 0 && uiThemeReady) {
            const wchar_t* section = reinterpret_cast<LPCWSTR>(lParam);
            if (section != nullptr && lstrcmpiW(section, L"ImmersiveColorSet") == 0 &&
                ThemeModeFromInt(ctx->cfg.GetInt(L"themeMode", 0)) == ThemeMode::System) {
                Theme::Apply(ThemeMode::System);
            }
        }
        if (msg == WM_HOTKEY && wParam == static_cast<WPARAM>(ui3::kGcHotkeyId)) {
            ToggleMainWindowVisible(win.Hwnd());
            *handled = true;
            return 0;
        }
        // 系统关机/注销（V20-P2-4）：立即持久化会话与配置，避免末段设置静默丢失。
        if (msg == WM_ENDSESSION && wParam != 0) {
            SaveSessionFromCtx(*ctx, win.Hwnd());
            ctx->cfg.Save(ConfigPath());
            // 不置 handled：默认处理继续结束会话。
        }
        return tray.HandleMessage(h, msg, wParam, lParam, handled);
    };
    cbs.ud = &renderer;
    // H-A: 窗口标题带版本（AboutInfo.h::WindowTitleWithVersion，发布信息单一来源）。
    if (!win.Create(inst, WindowTitleWithVersion().c_str(), 1280, 800, cmdShow, cbs)) {
        STM_LOG_ERROR("app", L"窗口创建失败");
        return 1;
    }
    // P3 任务三：主窗句柄交给 AppContext，「最小化到托盘」的模态按钮据此 SW_HIDE。
    ctx->mainHwnd = win.Hwnd();
    // 应用 Logo（Logo 任务）：标题栏大/小图标（LR_SHARED 共享句柄，无需 DestroyIcon；
    // 注意变量名不可用 small——rpcndr.h 将其定义为 char 宏）。
    if (smokeFrames == 0) {
        const HICON iconBig = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        const HICON iconSmall = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        if (iconBig) {
            SendMessageW(win.Hwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(iconBig));
        }
        if (iconSmall) {
            SendMessageW(win.Hwnd(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(iconSmall));
        }
    }
    if (!renderer.Init(win.Hwnd(), win.Width(), win.Height())) {
        MessageBoxW(nullptr, L"D3D11 初始化失败（含 WARP 兜底）。", L"错误", MB_OK | MB_ICONERROR);
        return 1;
    }
    ImGuiLayer ui;
    if (!ui.Init(win.Hwnd(), &renderer)) return 1;
    ui::SetAboutGraphics(renderer.Device(), renderer.Context());
    // H-A: 启动即应用 cfg 持久的主题模式（themeMode，默认 0=Dark；System 在
    // Apply 内解析注册表）。随后的系统深浅色广播由 onMessage 钩子处理。
    Theme::Apply(ThemeModeFromInt(ctx->cfg.GetInt(L"themeMode", 0)));
    uiThemeReady = true;
    // 壁纸（Phase-6 接线）：D3D/ImGui 均就绪后，注册后端供外观菜单加载/清除用，
    // 并恢复 %LOCALAPPDATA% 持久的壁纸副本（无副本时为安静空操作）。
    RegisterWallpaperBackend(renderer.Device(), renderer.Context());
    ui::WallpaperAutoRestore(renderer.Device(), renderer.Context());

    // 托盘图标：--smoke / --autotest 下跳过（CI 近乎无头渲染，
    // 避免外壳图标折腾）。
    if (!headless && tray.Create(win.Hwnd(), ctx->elevated)) {
        const HWND hwnd = win.Hwnd();
        tray.onToggleWindow = [hwnd]() {
            // P1-2（F2 评审）：最小化窗口仍报告 IsWindowVisible()==TRUE，
            // 因此旧的两分支检查在左键点击时会把任务栏按钮藏掉。三个
            // 分支：最小化 => 恢复+前置；可见 => 隐藏；否则显示。
            ToggleMainWindowVisible(hwnd);
        };
        tray.onShowWindow = [hwnd]() {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        };
        // 按值捕获 shared_ptr，使这些回调无论拆除顺序如何
        // 都保持生命周期安全。
        tray.onRestartElevated = [ctx, &win]() {
            SaveSessionFromCtx(*ctx, win.Hwnd());
            if (ops::RelaunchAsAdmin(L"--relaunched")) ctx->wantExit = true;
        };
        tray.onExit = [ctx]() { ctx->wantExit = true; };
        // 第 3 阶段告警经同一图标发托盘气泡（增量式）。
        ui3::SetBalloonSink([&tray](const std::wstring& title, const std::wstring& text) {
            tray.ShowBalloon(title, text);
        });
    }
    // --smoke 下每个页面的 Draw 都在屏外窗口中执行一遍，
    // 让新标签页的空态/错误态被 CI 冒烟覆盖。
    ui3::SetSmokeDrawAll(smokeFrames > 0);
    // H-A: --smoke 同时把「外观→自定义壁纸」控件组画进离屏窗口（渲染路径覆盖）。
    SetAppearanceSmokePreview(smokeFrames > 0);

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

    // 帧循环：按垂直同步节拍；采集在独立线程进行。
    // 最小化 => 把采集节奏放宽到 2s（第 4 阶段预算项）。
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    int frames = 0;
    int autotestFrame = 0;
    int autotestResult = 0;
    bool autotestGateOpen = false;
    bool dialogClickStarted = false;
    bool dialogClickLogged = false;
    DialogClickDriver dialogClick;
    bool aboutClickStarted = false;
    bool aboutClickLogged = false;
    AboutClickDriver aboutClick;
    bool wallpaperTestStarted = false;
    bool wallpaperTestLogged = false;
    WallpaperTestDriver wallpaperTest;
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
            // 恢复时实时读取 cfg，使运行期间在工具栏改的间隔
            // 在最小化/恢复循环中得以保留（终审 V11-P1-2）。
            ctx->collect.SetInterval(iconic ? 2000 : ClampInterval(ctx->cfg.GetInt(L"intervalMs", 1000)));
            wasIconic = iconic;
        }

        QueryPerformanceCounter(&t0);
        renderer.BeginFrame();
        ui.NewFrame();
        // 壁纸（Phase-6 接线，V18-P0 修正）：必须发生在 NewFrame 之后——背景绘制
        // 列表只有在本帧内被追加过才会并入 DrawData（imgui 以 g.Time 戳判定），
        // NewFrame 之前调用永不渲染。背景列表天然位于所有普通窗口之下。
        ui::WallpaperDrawBackground(
            ui::ClampMask(static_cast<float>(ctx->cfg.GetDouble(L"wallpaperMask", 0.45))));
        DrawShell(*ctx);  // 把通知取空变成 toast；每帧一次快照读取
        ui.Render();
        renderer.Present();
        QueryPerformanceCounter(&t1);
        ctx->frameMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;

        if (smokeFrames > 0 && ++frames >= smokeFrames) break;
        // --autotest：先跑几帧（外壳 + toast 存活），然后执行确认动作 /
        // 驱动对话框点击，写日志并退出。
        // 以首批完成的采集 tick 为门控：若第一个 tick 仍在途就退出，
        // 拆除时的 collect.Stop() join 会卡在一个在慢速/虚拟 GPU 上
        // 可能很久的 tick 上——而且动作本身
        // 也需要一份真实快照存在。
        if (!autotestMode.empty()) {
            if (!autotestGateOpen && ++autotestFrame >= 8 && ctx->collect.TickCount() >= 2) {
                autotestGateOpen = true;
            }
            if (autotestMode == L"dialogclick") {
                // V14 P1：用合成鼠标事件驱动真实渲染的确认对话框
                //（这里排队的事件由下一次 NewFrame 消费）。
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
            } else if (autotestMode == L"about") {
                // R-Fix Bug3：对工具栏「?」关于按钮使用相同注入方法
                //（点击 -> 模态框打开并保持 -> Esc 关闭）。
                if (autotestGateOpen && !aboutClickStarted) {
                    aboutClick.Start(ctx);
                    aboutClickStarted = true;
                }
                aboutClick.Tick();
                if (aboutClick.Done() && !aboutClickLogged) {
                    autotestResult = aboutClick.pass ? 0 : 1;
                    WriteAutotestLine(L"about", aboutClick.result, aboutClick.detail);
                    aboutClickLogged = true;
                }
            } else if (autotestMode == L"wallpaper") {
                // R-Fix Bug2：把生成的 4x4 测试图像经真实 WallpaperLoad
                // 路径加载，然后断言该帧 DrawData 携带绑定壁纸的几何
                //（顶点数 > 0 + 非图集纹理的绘制命令），
                // 之后以 0 退出。
                if (autotestGateOpen && !wallpaperTestStarted) {
                    wallpaperTest.Start(ctx, renderer.Device(), renderer.Context());
                    wallpaperTestStarted = true;
                }
                wallpaperTest.Tick();
                if (wallpaperTest.Done() && !wallpaperTestLogged) {
                    autotestResult = wallpaperTest.pass ? 0 : 1;
                    WriteAutotestLine(L"wallpaper", wallpaperTest.result,
                                      wallpaperTest.detail);
                    wallpaperTestLogged = true;
                }
            } else if (autotestGateOpen) {
                autotestResult = RunAutotest(ctx, autotestMode);
                ctx->wantExit = true;
            }
        }
    }

    if (smokeFrames == 0 && autotestMode.empty()) {
        SaveSessionFromCtx(*ctx, win.Hwnd());  // 窗口矩形/页面/选择的交接
        ctx->cfg.Save(ConfigPath());
        // H-A: 「恢复默认列宽」把 colW_* 软删除为 ""，Save 会把空值写回 —— 这里
        // 按值过滤剔除，保证重启后配置文件不再残留这些键（重新调过的真实宽度保留）。
        ui3::StripColWidthKeysFromFile(ConfigPath(), /*onlyEmptyValues=*/true);
    }
    tray.Remove();
    ui3::GcHotkeyUnbindWindow();   // F4#10: 退出反注册热键
    ui3::SetBalloonSink(nullptr);  // P2-7：sink 捕获了 &tray；拆除前先摘除
    // 停止采集，给在途 ops 任务至多 2s；之后仍在运行的
    // 一切都靠 ctx 的 shared_ptr 存活（见上文注释）。
    ctx->collect.Stop();
    ctx->jobs.Shutdown(2000);
    // R-Fix Bug2: 退出只释放 GPU 纹理，保留 %LOCALAPPDATA% 持久化副本 ——
    // 旧的 WallpaperClear 在退出时删除副本，使「下次启动自动恢复」成为死代码
    // （选图后重启即回到纯色，属于用户报告的「壁纸不生效」的一半根因）。
    ui::WallpaperShutdown();
    ui::ShutdownAboutUi();  // V20-P2-3：先于设备销毁释放关于页徽标纹理
    ui.Shutdown();
    renderer.Shutdown();
    win.Destroy();
    LogShutdown();
    return autotestResult;
}

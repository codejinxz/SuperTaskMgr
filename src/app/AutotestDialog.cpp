// --autotest dialogclick 驱动实现（见 AutotestDialog.h）。
#include "app/AutotestDialog.h"
#include "app/AppContext.h"
#include "app/ui/AboutUi.h"
#include "app/ui/ConfirmAction.h"
#include "app/ui3/Wallpaper.h"
#include "core/FsUtil.h"
#include "core/Log.h"
#include "core/Str.h"
#include "ops/ProcessOps.h"
#include "imgui.h"
#include "imgui_internal.h"  // R-Fix 诊断：g.HoveredWindow 名字进 [about] 日志
#include <windows.h>
#include <string>

namespace stm {

namespace {

constexpr int kMinOpenFrames = 4;        // 哨兵：模态框提交 >= N 帧
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

    // 受害进程：cmd -> ping 树，与其他 autotest 模式同形。
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

    // 经行右键菜单所用的同一路径布设（而非 ExecuteConfirmed-
    // Action）：点击必须落在真实渲染的模态框上。
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
        TerminateProcess(child_.get(), 1);  // 失败时尽力清理
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
        // 若帧循环拖沓，保持子进程清理期限的诚实。
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const DialogAutotestState& st = DialogAutotestStateForAutotest();

    switch (phase_) {
        case Phase::WaitOpen: {
            // 等待期间把合成光标停在中性位置。
            io.AddMousePosEvent(640.0f, 400.0f);
            if (st.modalOpen && st.framesOpen >= kMinOpenFrames) {
                // 旧缺陷哨兵已通过：模态框现已连续多帧提交且
                // 可交互（F1 的 bug 只渲染一帧、
                // 永远无法点击）。
                phase_ = Phase::Hover;
                hoverFrames_ = 0;
                phaseDeadline_ = NowMs() + kPhaseTimeoutMs;
                break;
            }
            // 注意：Start() 之后的第一次 Tick 对话框尚未绘制
            //（Start 在 Present 之后运行）——失败只按超时判定。
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
            // 点击本应已生效；若模态框已消失而子进程仍存活，
            // 说明 ExecuteConfirmedAction 失败或根本没被走到。
            if (NowMs() > phaseDeadline_) {
                if (childDead) {
                    // 一帧宽限：处理点击的那一帧仍报告
                    // 模态框打开；只是断言窗口刚过。
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

// ---------------------------------------------------------------------------
// --autotest about（R-Fix Bug3）。镜像 DialogClickDriver 的注入方法，
// 但受害对象是工具栏「?」按钮：Bug1 的重叠布局曾使它
// 无法点击，而经真实管线的点击仍必须打开模态框。
// ---------------------------------------------------------------------------

bool AboutClickDriver::Done() const { return phase_ == Phase::Finished; }

void AboutClickDriver::Start(std::shared_ptr<AppContext> ctx) {
    ctx_ = std::move(ctx);
    phase_ = Phase::WaitButton;
    phaseDeadline_ = NowMs() + kPhaseTimeoutMs;
    STM_LOG_INFO("autotest", L"[about] Start");
}

void AboutClickDriver::Fail(const std::wstring& why) {
    detail = why;
    result = L"FAIL";
    pass = false;
    phase_ = Phase::Finished;
    if (ctx_) ctx_->wantExit = true;
}

void AboutClickDriver::Finish(bool ok, const std::wstring& why) {
    detail = why;
    result = ok ? L"PASS" : L"FAIL";
    pass = ok;
    phase_ = Phase::Finished;
    if (ctx_) ctx_->wantExit = true;
}

void AboutClickDriver::Tick() {
    if (phase_ == Phase::WaitStart || phase_ == Phase::Finished) return;

    ImGuiIO& io = ImGui::GetIO();
    const ui::AboutAutotestState& st = ui::AboutAutotestStateForAutotest();
    const float cx = (st.btnMinX + st.btnMaxX) * 0.5f;
    const float cy = (st.btnMinY + st.btnMaxY) * 0.5f;

    // 同时移动真实 OS 光标并为同一点排队合成位置：
    // 这样无论焦点状态如何，两个来源在 (cx,cy) 上保持一致——
    // 窗口有焦点时 Win32 后端每帧轮询 ::GetCursorPos，
    // 其事件会取代排队的合成事件（因此只动真实光标才行），
    // 而无焦点时后端沉默，只有合成事件
    // 驱动位置。只有在 ImGui 确认按钮悬停（Hover 阶段）之后
    // 才注入按下，这样与光标较劲的外来指针会大声失败，
    // 而不是点错控件。
    const auto placeCursor = [&]() {
        if (ctx_ != nullptr && ctx_->mainHwnd != nullptr) {
            POINT pt{static_cast<LONG>(cx), static_cast<LONG>(cy)};
            ClientToScreen(static_cast<HWND>(ctx_->mainHwnd), &pt);
            SetCursorPos(pt.x, pt.y);
        }
        io.AddMousePosEvent(cx, cy);
    };

    switch (phase_) {
        case Phase::WaitButton: {
            if (st.btnValid) {
                STM_LOG_INFO("autotest",
                             Fmt(L"[about] 按钮矩形 ({:.0f},{:.0f})-({:.0f},{:.0f})",
                                 st.btnMinX, st.btnMinY, st.btnMaxX, st.btnMaxY));
                placeCursor();
                phase_ = Phase::Hover;
                hoverFrames_ = 0;
                phaseDeadline_ = NowMs() + kPhaseTimeoutMs;
            } else if (NowMs() > phaseDeadline_) {
                Fail(L"工具条「?」按钮从未提交（未渲染或被布局裁剪/遮挡）");
            }
            break;
        }
        case Phase::Hover: {
            placeCursor();
            // R-Fix：不数帧 —— 等 ImGui 真实确认「?」已悬停（最多 15 s），
            // 再注入按下，避免按下落在悬浮光标争夺中的错误控件上。
            if (st.btnHovered && ++hoverFrames_ >= 2) {
                STM_LOG_INFO("autotest", L"[about] 悬停完成，按下");
                phase_ = Phase::Down;
                phaseDeadline_ = NowMs() + kPhaseTimeoutMs;
            } else {
                if (hoverFrames_ % 20 == 0) {
                    ImGuiContext& g = *ImGui::GetCurrentContext();
                    STM_LOG_INFO(
                        "autotest",
                        Fmt(L"[about] 悬停中 frames={} mouse=({:.1f},{:.1f}) hoveredBtn={} hoveredId={:#x} hoveredWin={} fg={}",
                            hoverFrames_, io.MousePos.x, io.MousePos.y,
                            st.btnHovered ? 1 : 0, g.HoveredId,
                            g.HoveredWindow ? Utf8ToWide(g.HoveredWindow->Name)
                                            : std::wstring(L"(none)"),
                            GetForegroundWindow() == static_cast<HWND>(ctx_->mainHwnd)
                                ? 1
                                : 0));
                }
                ++hoverFrames_;
            }
            if (NowMs() > phaseDeadline_) {
                Fail(L"移动真实光标后「?」始终未进入悬停态（后端未上报/被遮挡）");
            }
            break;
        }
        case Phase::Down: {
            placeCursor();
            io.AddMouseButtonEvent(0, true);
            phase_ = Phase::Up;
            break;
        }
        case Phase::Up: {
            // R-Fix 诊断：此刻 mdown 应为 true（上一帧注入的 down 已被处理），
            // 且按钮应处于悬停态 —— 说明按下确实落在「?」上。
            STM_LOG_INFO("autotest",
                         Fmt(L"[about] 释放注入前 mdown={} hoverBtn={} pos=({:.0f},{:.0f})",
                             io.MouseDown[0] ? 1 : 0, st.btnHovered ? 1 : 0,
                             io.MousePos.x, io.MousePos.y));
            io.AddMouseButtonEvent(0, false);
            STM_LOG_INFO("autotest",
                         Fmt(L"[about] 点击已注入 ({:.0f},{:.0f}) btnValid={}", cx, cy,
                             st.btnValid ? 1 : 0));
            phase_ = Phase::WaitModal;
            phaseDeadline_ = NowMs() + kPhaseTimeoutMs;
            break;
        }
        case Phase::WaitModal: {
            // 与 dialogclick 相同的单帧模态哨兵：模态框必须
            // 连续提交 >= 4 帧我们才认定它已打开。
            static int dbgFrames = 0;  // 探针期临时观察（低频采样，不刷屏）
            if (++dbgFrames % 60 == 0) {
                const ImVec2 mp = io.MousePos;
                const bool focused = GetForegroundWindow() != nullptr;
                STM_LOG_INFO(
                    "autotest",
                    Fmt(L"[about] 等模态: open={} frames={} btnValid={} hoveredBtn={} mouse=({:.0f},{:.0f}) fg={} mdown={}",
                        st.modalOpen ? 1 : 0, st.modalFrames, st.btnValid ? 1 : 0,
                        st.btnHovered ? 1 : 0, mp.x, mp.y,
                        focused ? 1 : 0, io.MouseDown[0] ? 1 : 0));
            }
            if (st.modalOpen && st.modalFrames >= kMinOpenFrames) {
                hoverFrames_ = 0;
                phase_ = Phase::CloseHover;
                phaseDeadline_ = NowMs() + kExitTimeoutMs;
            } else if (NowMs() > phaseDeadline_) {
                Fail(L"点击后关于模态未能打开或连续提交帧数不足（单帧化回归）");
            }
            break;
        }
        case Phase::CloseHover: {
            // R-Fix：Esc 注入在某些焦点状态下不触发模态取消 —— 改用与打开
            // 相同的真实管线点击模态内「关闭」按钮，验证「打开->保持->关闭」
            // 完整回路。同样是先等悬停确认再注入按下。
            const float ccx = (st.closeMinX + st.closeMaxX) * 0.5f;
            const float ccy = (st.closeMinY + st.closeMaxY) * 0.5f;
            if (ctx_ != nullptr && ctx_->mainHwnd != nullptr) {
                POINT pt{static_cast<LONG>(ccx), static_cast<LONG>(ccy)};
                ClientToScreen(static_cast<HWND>(ctx_->mainHwnd), &pt);
                SetCursorPos(pt.x, pt.y);
            }
            io.AddMousePosEvent(ccx, ccy);
            if (st.closeHovered && ++hoverFrames_ >= 2) {
                STM_LOG_INFO("autotest", L"[about] 关闭按钮悬停完成，按下");
                phase_ = Phase::CloseDown;
                phaseDeadline_ = NowMs() + kExitTimeoutMs;
            } else if (NowMs() > phaseDeadline_) {
                Fail(L"模态内「关闭」按钮未能进入悬停态");
            }
            break;
        }
        case Phase::CloseDown: {
            io.AddMouseButtonEvent(0, true);
            phase_ = Phase::CloseUp;
            break;
        }
        case Phase::CloseUp: {
            io.AddMouseButtonEvent(0, false);
            phase_ = Phase::WaitClosed;
            phaseDeadline_ = NowMs() + kExitTimeoutMs;
            break;
        }
        case Phase::WaitClosed: {
            if (!st.modalOpen) {
                Finish(true,
                       L"真实管线点击「?」生效：模态打开并保持>=4帧，点击「关闭」后退出");
            } else if (NowMs() > phaseDeadline_) {
                Fail(L"点击「关闭」后关于模态未关闭");
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// --autotest wallpaper（R-Fix Bug2）。把程序生成的测试图像经真实的
// WallpaperLoad -> AddImage -> DX11 管线渲染，并断言该帧 DrawData
// 确实携带绑定壁纸的几何（顶点 > 0 且某绘制命令的纹理
// 不是字体图集——即壁纸 SRV）。
// ---------------------------------------------------------------------------

namespace {

constexpr uint64_t kWallpaperTimeoutMs = 15000;

// 测试期间 wallpaper.* 的存档副本移为 wallpaper.<ext>.wpautobak，
// 用户的真实壁纸绝不会被破坏。
const wchar_t* const kWallpaperExts[] = {L"png", L"jpg", L"jpeg", L"bmp", L"tga"};

std::wstring StoredWallpaperPath(const wchar_t* ext) {
    return stm::LocalAppDataRoot() + L"\\wallpaper\\wallpaper." + ext;
}

// 4x4 24bpp 自底向上 BMP，纯红——stb 能确定性解码的最小图像。
bool WriteTestBmp(const std::wstring& path) {
    constexpr int w = 4, h = 4;
    const int rowBytes = (w * 3 + 3) & ~3;
    const DWORD dataSize = static_cast<DWORD>(rowBytes * h);
    const DWORD fileSize = 54 + dataSize;
    BYTE buf[54 + 4 * 4 * 4] = {};
    buf[0] = 'B';
    buf[1] = 'M';
    *reinterpret_cast<DWORD*>(&buf[2]) = fileSize;
    *reinterpret_cast<DWORD*>(&buf[10]) = 54;
    *reinterpret_cast<DWORD*>(&buf[14]) = 40;  // BITMAPINFOHEADER 头
    *reinterpret_cast<LONG*>(&buf[18]) = w;
    *reinterpret_cast<LONG*>(&buf[22]) = h;
    *reinterpret_cast<WORD*>(&buf[26]) = 1;
    *reinterpret_cast<WORD*>(&buf[28]) = 24;  // 每像素位数
    *reinterpret_cast<DWORD*>(&buf[34]) = dataSize;
    for (int y = 0; y < h; ++y) {
        BYTE* row = &buf[54 + static_cast<size_t>(y) * rowBytes];
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = 0;    // B（蓝）
            row[x * 3 + 1] = 0;    // G（绿）
            row[x * 3 + 2] = 255;  // R（BMP 按 BGR 存储）-> stbi 得到红色
        }
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, buf, fileSize, &wrote, nullptr) && wrote == fileSize;
    CloseHandle(f);
    return ok;
}

void BackupStoredWallpapers() {
    for (const wchar_t* e : kWallpaperExts) {
        const std::wstring p = StoredWallpaperPath(e);
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        MoveFileW(p.c_str(), (p + L".wpautobak").c_str());
    }
}

}  // namespace

bool WallpaperTestDriver::Done() const { return phase_ == Phase::Finished; }

void WallpaperTestDriver::CleanupFiles() {
    // 删除测试自己的存档副本，然后把备份的原件放回。
    for (const wchar_t* e : kWallpaperExts) {
        const std::wstring p = StoredWallpaperPath(e);
        DeleteFileW(p.c_str());
        const std::wstring bak = p + L".wpautobak";
        if (GetFileAttributesW(bak.c_str()) != INVALID_FILE_ATTRIBUTES) {
            MoveFileW(bak.c_str(), p.c_str());
        }
    }
    if (!bmpPath_.empty()) DeleteFileW(bmpPath_.c_str());
}

void WallpaperTestDriver::Fail(const std::wstring& why) {
    CleanupFiles();
    detail = why;
    result = L"FAIL";
    pass = false;
    phase_ = Phase::Finished;
    if (ctx_) ctx_->wantExit = true;
}

void WallpaperTestDriver::Finish(bool ok, const std::wstring& why) {
    CleanupFiles();
    detail = why;
    result = ok ? L"PASS" : L"FAIL";
    pass = ok;
    phase_ = Phase::Finished;
    if (ctx_) ctx_->wantExit = true;
}

void WallpaperTestDriver::Start(std::shared_ptr<AppContext> ctx, void* d3dDevice,
                                void* d3dContext) {
    ctx_ = std::move(ctx);
    phaseDeadline_ = NowMs() + kWallpaperTimeoutMs;

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    bmpPath_ = std::wstring(temp) + L"stm_autotest_wallpaper.bmp";
    if (!WriteTestBmp(bmpPath_)) {
        Fail(L"测试 BMP 写入失败");
        return;
    }
    BackupStoredWallpapers();

    stm::ui::WallpaperState st{};
    if (!stm::ui::WallpaperLoad(d3dDevice, d3dContext, bmpPath_, &st) ||
        !stm::ui::WallpaperActive()) {
        Fail(L"WallpaperLoad 失败：" + st.error);
        return;
    }
    STM_LOG_INFO("autotest",
                 Fmt(L"[wallpaper] 已加载 {}x{}（SRV 注册路径=裸指针直绑 DX11 后端）",
                     st.width, st.height));
    phase_ = Phase::WaitDraw;
}

void WallpaperTestDriver::Tick() {
    if (phase_ != Phase::WaitDraw) return;
    // 在 Present() 之后调用：GetDrawData() 仍持有本帧数据
    //（要到下一次 NewFrame/Render 才重建）。WallpaperLoad 在上一个
    // Present 之后运行，因此最初的 tick 可能仍看到没有壁纸的帧。
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd != nullptr && dd->TotalVtxCount > 0) {
        const ImTextureID fontTex = ImGui::GetIO().Fonts->TexRef.GetTexID();
        int wpCmds = 0;
        for (const ImDrawList* dl : dd->CmdLists) {
            for (const ImDrawCmd& cmd : dl->CmdBuffer) {
                if (cmd.GetTexID() != fontTex) ++wpCmds;
            }
        }
        if (wpCmds > 0) {
            Finish(true, Fmt(L"壁纸真实渲染：帧顶点={} 壁纸绑定绘制命令={} active={}",
                             dd->TotalVtxCount, wpCmds,
                             stm::ui::WallpaperActive() ? 1 : 0));
            return;
        }
    }
    if (NowMs() > phaseDeadline_) {
        Fail(L"15 秒内 DrawData 未出现壁纸纹理绘制命令（AddImage 未产生输出）");
    }
}

}  // namespace stm

#pragma once
// --autotest dialogclick（V14 P1）：针对 F1 修复的确认对话框的
// 真实 UI 管线回归防护。与其他 --autotest 模式（直接调用
// ui::ExecuteConfirmedAction，坏代码也能通过）不同，本驱动：
//
//   1. 启动 `cmd /c ping -n 30` 子进程，
//   2. 经与行右键菜单（app/ui/Pages.cpp）相同的 RequestConfirmKill() 路径
//      布设终止确认，
//   3. 每帧注入合成的 io.AddMousePosEvent / AddMouseButtonEvent，
//      让真实渲染的模态框在其动作按钮上收到 hover/down/up，
//   4. 断言旧缺陷哨兵（模态框在点击前连续提交 >= 4 帧——F1 的 bug
//      恰好只渲染一帧），
//      以及子进程确实死亡且模态框关闭。
// 主窗口刻意保持可见：交换链使用垂直同步的 Present，
// 隐藏窗口可能造成停顿；驱动绝不触碰真实光标。
#include <memory>
#include <string>

#include "app/ui/Pages.h"  // DialogAutotestState
#include "core/HandleGuard.h"

namespace stm {

class AppContext;

class DialogClickDriver {
public:
    // 启动受害进程并布设 shell 终止确认。需要帧循环的常规
    // autotest 门控（已渲染若干帧 + tick 正常流动）。
    void Start(std::shared_ptr<AppContext> ctx);
    // 每帧一次，在 renderer.Present() 之后：注入鼠标事件（由下一帧的
    // NewFrame 消费）并评估阶段转换 / 断言。
    // Finish() 会投递 ctx->wantExit。
    void Tick();
    bool Done() const;

    std::wstring result = L"FAIL";  // 写入 autotest_result.log 的 L"PASS" / L"FAIL"
    std::wstring detail;
    bool pass = false;

private:
    enum class Phase {
        WaitStart,     // 等待帧/tick 门控
        WaitOpen,      // 已请求对话框；等待模态框 + 哨兵帧
        Hover,         // 移到动作按钮上
        Down,          // 鼠标按下
        Up,            // 鼠标抬起
        WaitExit,      // 断言子进程退出 + 模态框关闭
        Finished,
    };

    void Fail(const std::wstring& why);
    void Finish(bool ok, const std::wstring& why);

    std::shared_ptr<AppContext> ctx_;
    UniqueHandle child_;
    uint32_t childPid_ = 0;
    Phase phase_ = Phase::WaitStart;
    int hoverFrames_ = 0;
    uint64_t phaseDeadline_ = 0;  // 基于 GetTickCount64 的每阶段超时
};

// --autotest about（R-Fix Bug3；A1 适配）：同样的真实 UI 管线思路，用于工具栏
// 「关于」按钮（A1 统一风格改造后取代旧的「?」迷你按钮；矩形仍经
// ui::AboutAutotestState 发布，改由 Pages.cpp::DrawToolbar 提交按钮）。
// Bug1 的右侧重叠项曾使它无法点击；本驱动把真实光标
// 移到真实渲染的按钮上（矩形每帧发布），
// 等待 ImGui 确认悬停，注入点击，断言关于模态框已打开、
// the click, then clicks the modal's 关闭 button the same way. Asserts: modal
// 连续多帧保持提交（单帧模态
// 哨兵），并在按钮上点击关闭。
class AboutClickDriver {
public:
    void Start(std::shared_ptr<AppContext> ctx);
    void Tick();
    bool Done() const;

    std::wstring result = L"FAIL";
    std::wstring detail;
    bool pass = false;

private:
    enum class Phase {
        WaitStart,   // 尚未调用 Start()：Tick 必须是空操作（main 会无条件调
                     // Tick，在门控放行 Start 之前）
        WaitButton,  // 工具栏「关于」矩形可见
        Hover,       // 移到按钮上，等待 ImGui 悬停确认
        Down,        // 鼠标按下
        Up,          // 鼠标抬起
        WaitModal,   // 模态框打开 + 连续 >= 4 帧
        CloseHover,  // move onto the modal's 关闭 button, hover confirmation
        CloseDown,   // 鼠标按下
        CloseUp,     // 鼠标抬起
        WaitClosed,  // 模态框消失
        Finished,
    };

    void Fail(const std::wstring& why);
    void Finish(bool ok, const std::wstring& why);

    std::shared_ptr<AppContext> ctx_;
    Phase phase_ = Phase::WaitStart;
    int hoverFrames_ = 0;
    uint64_t phaseDeadline_ = 0;
};

// --autotest wallpaper（R-Fix Bug2）：产品化的渲染输出探针。先写一张
// 程序生成的 4x4 红色 BMP 到 %TEMP%，走真实的 WallpaperLoad 路径
//（stb 解码 -> D3D11 纹理 -> SRV），然后在每帧 ImGui::GetDrawData()
// 中观察绑定到壁纸 SRV 的绘制命令（GetTexID() != 字体图集）
// 且顶点数 > 0——证明背景图真的被渲染，而不只是被加载。
// 已存在的 wallpaper.* 存档先移开、退出时恢复，测试绝不破坏
// 用户状态。由无头 GPU 探针（app/UiFixProbe.cpp --mode=wallpaper）
// 支撑。
class WallpaperTestDriver {
public:
    void Start(std::shared_ptr<AppContext> ctx, void* d3dDevice, void* d3dContext);
    void Tick();
    bool Done() const;

    std::wstring result = L"FAIL";
    std::wstring detail;
    bool pass = false;

private:
    enum class Phase {
        WaitStart,   // 尚未调用 Start()：Tick 必须是空操作（main 会无条件调
                     // Tick；phaseDeadline_ 在此无意义）
        WaitDraw,    // 等待 DrawData 携带壁纸命令的帧
        Finished,
    };

    void Fail(const std::wstring& why);
    void Finish(bool ok, const std::wstring& why);
    void CleanupFiles();

    std::shared_ptr<AppContext> ctx_;
    std::wstring bmpPath_;
    Phase phase_ = Phase::WaitStart;
    uint64_t phaseDeadline_ = 0;
};

}  // namespace stm

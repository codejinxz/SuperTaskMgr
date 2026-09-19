#pragma once
// 第 3 阶段 UI 扩展：网络/启动项/服务/驱动/传感器标签页，
// 外加可选的阈值告警监视器。以追加方式注册到第 2 阶段页面之上
//（追加进 RegisterPages，app/ui/Pages.cpp）。
#include <functional>
#include <memory>
#include <string>
#include "app/AppContext.h"

namespace stm {
namespace ui3 {

// 页面注册（由 RegisterPages 调用；追加在性能页之后）。
void RegisterPhase3Pages(AppContext& ctx);

// 第 3 阶段页面创建的 ops 任务共享上下文句柄（生命周期模式
// 与 BindAppContext 相同）。main.cpp 启动时调用一次。
void BindPhase3Context(std::shared_ptr<AppContext> ctx);
std::shared_ptr<AppContext> LiveP3Ctx();

// 外壳钩子，由 DrawShell 每帧调用一次：阈值告警监视器
//（默认关；cfg 键 alertOn / alertCpu / alertMem）。
void AlertTick(AppContext& ctx);

// 性能页钩子：紧凑的告警开关 + 阈值控件（追加在性能页底部；
// 纯增量）。
void DrawAlertControls(AppContext& ctx);

// 冒烟支持：启用时（--smoke），DrawSmokeAllPages 把每个页面
// 每帧在屏外窗口绘制一次，使新标签页的空态/错误态
// 被 SuperTaskMgr.exe --smoke 执行到。
void SetSmokeDrawAll(bool on);
void DrawSmokeAllPages(AppContext& ctx);

// 告警的托盘气泡 sink（main.cpp 接到 Tray::ShowBalloon；--smoke 下
// 不接，告警降级为仅 toast）。
using BalloonSink = std::function<void(const std::wstring& title, const std::wstring& text)>;
void SetBalloonSink(BalloonSink sink);

}  // namespace ui3
}  // namespace stm

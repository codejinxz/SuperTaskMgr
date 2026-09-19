#pragma once
// 页面注册表 + 应用外壳装饰（工具栏/标签栏/状态栏）。
// 注意：自第 2 阶段起 Pages.cpp 归 UI 开发 agent 所有；
// 外壳只保证 RegisterPages() + DrawShell() 的签名。
#include "ops/SessionState.h"
#include <memory>
#include <windows.h>

namespace stm {

class AppContext;

void RegisterPages(AppContext& ctx);   // 填充 ctx.pages（标签顺序）
void DrawShell(AppContext& ctx);       // 工具栏 + 标签 + 状态栏 + 活动页
// main 把持有所有权的句柄交给 UI 层：ops 任务 lambda 捕获该
// shared_ptr，使在途任务在拆除后仍保住 notes/jobs/details。
void BindAppContext(std::shared_ptr<AppContext> ctx);
// 会话交接辅助（架构第 7 节）：LoadSession 后恢复 / 退出前快照。
void ApplySession(AppContext& ctx, const ops::SessionState& s);
void SaveSessionFromCtx(const AppContext& ctx, HWND mainWnd);

// --- --autotest dialogclick（V14）：确认对话框的真实 UI 管线回归。
// ArmKillConfirmForAutotest 经与行右键菜单完全相同的
// RequestConfirmKill() 路径打开终止确认；dialogclick 驱动随后
// 注入合成 io 鼠标事件，真正点击渲染出的动作按钮。
// 状态暴露最后渲染的模态框/按钮几何，驱动无需触碰真实光标
// 即可瞄准；另有单帧回归哨兵
//（framesOpen 统计模态框连续提交且可交互的帧数）。
struct DialogAutotestState {
    bool modalOpen = false;    // 上一帧 BeginPopupModal 返回 true
    int framesOpen = 0;        // 连续提交帧数（哨兵：必须 >= N）
    bool requestActive = false;// 确认请求已布设（kind != None）
    float actionMinX = 0, actionMinY = 0, actionMaxX = 0, actionMaxY = 0;
};
void ArmKillConfirmForAutotest(uint32_t pid, uint64_t createTime, const wchar_t* name);
const DialogAutotestState& DialogAutotestStateForAutotest();

}  // namespace stm

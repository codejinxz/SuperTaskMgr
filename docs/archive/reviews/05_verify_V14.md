# V14 独立验证报告：确认对话框"点击无反应"修复（F1）

验证人：subagent V14（新视角，未参考修复者推导）。日期：2026-09-18。
产物：build/Release（SuperTaskMgr.exe 11:57 / stm_selftest.exe 11:57，均晚于全部源码 11:41，为当前构建）。

## 一、验证结论：PASS（修复有效）

### 证据

1. **根因①已修（Pages.cpp:241-265）**：`DrawConfirmDialogs()` 不再因 `confirmOpenRequested` 提前 return；请求激活期间每帧 `BeginPopupModal("##confirm")`，`OpenPopup` 仅发一次（`IsPopupOpen` 门控）。模态从此可持续接收点击，不再留下单帧僵尸模态。
2. **根因②已修（Pages.cpp:331、Pages3.cpp:649、Pages3.cpp:1018）**：三处 `SetKeyboardFocusHere(0)` 均改为 `IsWindowAppearing()` 时一次。经查 imgui 1.92.9 上下文：`io.ConfigFlags |= NavEnableKeyboard`（ImGuiLayer.cpp:29）确为启用键盘导航，与"逐帧 nav 移动在 mouse-down 与 up 之间 ClearActiveID 吃掉点击"的机理吻合；修复后逐帧推演不再有 nav 与鼠标争抢窗口。
3. **三条链路状态机核验**：
   - 进程页（kill/killtree/trim/purge）：确认/取消 → `CloseConfirm()` + `CloseCurrentPopup()`；Esc → `BeginPopupModal` 返回 false 且 `!IsPopupOpen` → `CloseConfirm()`（Pages.cpp:259-264）；Begin 因裁剪返回 false 时保持重试不误清。无僵尸路径。
   - 启动项（Pages3.cpp:614-664）与服务（961-1033）：每帧渲染 + 请求帧前 `IsPopupOpen` 自检清理（621-624 / 968-971），退出路径全清 `PendOp{}`。无僵尸。
   - 确认动作唯一入口：进程页/启动项确认按钮均走 `ui::ExecuteConfirmedAction`；全 UI 无绕过调用（仅 `PlanTerminateTree` 预览属设计内）。服务页启停有自己的 `Submit`（未走 ExecuteConfirmedAction），但完整复刻同一契约（Submit==0 → 立即 JobFailed note），可接受，见 P2-备注。
4. **失败路径可见性**：Submit 返回 0 → `ExecuteConfirmedAction` 立即投递 JobFailed note（ConfirmAction.h:184-188）；ops 拒绝/失败 → job 内 JobFailed note + `AdminHintSuffix`；notes 经 `DrainNotifications`→`PushToast` 上屏（Pages.cpp:1328/1512）。唯一静默是 `ctx==null`（无 notes 队列可投，注释已声明），可接受。
5. **运行验证（本机实跑）**：
   - `stm_selftest.exe`：53/54，唯一失败 `collect_tick_latency`（允许项，宿主负载漂移）。
   - `--autotest kill`：exit 0，日志 `[autotest:kill] PASS - 已终止进程 cmd.exe`。
   - `--autotest tree`：exit 0，`PASS - 已终止进程树…终止 3 个`（子进程同灭）。
   - `--autotest startup`：exit 0，`PASS - 已禁用启动项「stm_f1_autotest」`（注册表清理确认）。
   - `--smoke 150`：exit 0。
6. **交叉边界推演**：
   - 模态打开期间切页签：ImGui 模态阻止下层窗口 hover/点击、nav 锁定在模态内 → 无法切页，状态机不暴露该场景；minimize 时主循环仍渲染（仅放宽采集间隔，main.cpp:380-386），状态机按帧推进无超时依赖；`paused` 只影响采集间隔，与模态无关。均健壮。
   - 连点确认键：按钮按下即 `CloseConfirm` + 关闭模态，第二次点击落空；动作按钮 `ImGuiItemFlags_NoNav`（Pages.cpp:337）且焦点在取消键，Enter/Space 无法触发终止 → 无双杀/重复提交。
   - 请求替换：`RequestConfirmKill/Trim/TreePlan` 均先 `CloseConfirm()`；模态已开时新请求直接复用现有弹窗渲染新内容（`OpenPopupEx` reopen 语义），无泄漏。
7. **plan=-2 路径**：规划失败时模态永不打开，plan job 投递 JobFailed toast「无法规划进程树：<err>」（Pages.cpp:126-127），`DrawConfirmDialogs` 轮询到 -2 即 `CloseConfirm`。文案见 P2-3。

### 独立判断（逐帧推演人工点击）

右键行 → 菜单项 click 于 mouse-up 激活 → 同帧 `OpenPopup`（`OpenPopupEx` 会先 `ClosePopupToLevel(0)` 收掉行菜单再压入确认模态，ImGui 1.92.9:12453-12503，无菜单残留遮挡）→ 模态每帧渲染 → 鼠标按下确认键（ActiveId 稳定，appearing 帧已过、无 nav 竞争）→ mouse-up 触发 → job 提交 → toast 回填。**"点击无反应"在三条链路上均已消除**。剩余唯一"无反应感"窗口是 KillTree 规划期（见 P2-2）。

## 二、新发现问题

### P1-1：所谓"点击模拟"回归测试并未覆盖被修的 UI 层（测试假绿风险）
- 证据：`src/selftest/ui_fix_test.cpp` 5 个 fix_confirm_* 用例与 `main.cpp RunAutotest`（153/193 行）都是**直接调用 `ui::ExecuteConfirmedAction(ctx, req)`**，注释自认"executes the SAME call a confirm button makes"。全程无 ImGui 帧、无 `DrawConfirmDialogs`、无鼠标事件注入。旧 bug 恰在 UI 层（BeginPopupModal 单帧渲染 + 逐帧 SetKeyboardFocusHere），旧代码上这两套"验证"同样全 PASS。
- 影响：本次修复只靠代码阅读+人工点击背书；未来回归（如再次提前 return、再次逐帧 focus）测试不报警。
- 修法：新增 imgui 上下文测试——渲染 DrawShell 数帧、注入 `io.AddMousePosEvent/AddMouseButtonEvent` 对确认键做 down/up、步进 NewFrame，断言 job 生效/notes 到达；至少补一个"pending 请求下连续 3 帧 BeginPopupModal 返回 true、第 2 帧起 IsWindowAppearing 为假"的断言。

### P2-1：`RequestTreePlan` 忽略 `jobs.Submit()` 返回值（Pages.cpp:119）
- 队列未运行（退出期）时 planCount 永远 -1：模态不出现、零反馈，违背自家"never silent"契约（对比 ExecuteConfirmedAction:184）。ServicePage 依赖规划 submit（Pages3.cpp:948）同病——模态会卡"正在查询依赖…"。
- 修法：返回 0 时置 plan=-2（复用失败 toast 路径）或直接投 JobFailed note。

### P2-2：KillTree 规划期无任何可见反馈
- 队列忙时菜单点击到模态出现存在静默空窗（数百 ms 级），用户可能再次感到"没反应"。
- 修法：点击当下先开模态显示"正在规划进程树…"，plan 回填后更新计数。

### P2-3：文案不一致
- plan 失败 toast（Pages.cpp:126-127）缺 `AdminHintSuffix`，与其余所有失败文案不一致，非提权用户少了指引。
- plan 成功文案"预计终止 N 个（含目标）"靠 `planned+1` 拼装，正确但脆弱（依赖 PlanTerminateTree 只返回后代这一约定），建议 ops 直接返回含根计数。

### P2-4：Kill 对话框长路径溢出
- Pages.cpp:284 用 `TextUnformatted` 输出完整路径，窗口宽度被 `SetNextWindowSizeConstraints(460,460)` 锁死，长路径水平溢出/裁剪（Trim 用 `TextWrapped` 正确）。观感问题，非本次修复引入。

### 备注（不计问题）
- ServicePage 未走 `ExecuteConfirmedAction`（无 Service ConfirmKind），自建 Submit 但契约等价；后续可统一收编。
- `ExecuteConfirmedAction` ctx==null 静默返回：应用退出期无 notes 队列可投，契约已注释，可接受。

## 三、汇总

| 项 | 结果 |
|---|---|
| 修复有效性判定 | **PASS** |
| 运行验证 | selftest 53/54（允许项外全过）；autotest kill/tree/startup 全 PASS exit 0；smoke 150 exit 0 |
| 新发现 | P1 ×1（回归测试未覆盖 UI 层，假绿）；P2 ×4；备注 ×2 |
| 僵尸模态/双杀/静默失败 | 未发现残留路径 |

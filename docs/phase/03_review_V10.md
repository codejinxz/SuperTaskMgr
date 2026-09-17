# 阶段 3 审查报告 — V10（一致性 + 生命周期 + 性能）

- 审查人：V10（独立审查，与其他审查者互不知情）
- 日期：2026-09-18
- 对象：`src/app/ui3/*`（AsyncFetch / PageHelpers / Pages3 五页+告警）、`app/ui/Pages.cpp`、`app/main.cpp`、`app/ui/Tray.cpp` 的阶段 3 追加改动、`src/selftest/ui3_test.cpp`、`src/collect/CollectService.cpp` 的 ETW 开关接线
- 维度：AsyncFetch 生命周期与并发 / 破坏性操作一致性 / ETW 接线 / 告警 / 追加式回归 / 空态文案口径 / 测试覆盖
- 实测：`stm_selftest.exe` 40/40 通过（含 6 个 ui3_*）；`SuperTaskMgr.exe --smoke 150` exit 0（本机复跑 2 次，另核对本机日志中 smoke=400/150 多次历史运行均无 ERROR/WARN，服务 275 项、驱动 226 项、启动项 51 项枚举正常完成）

## 统计

| 级别 | 数量 |
|---|---|
| P0 | 0 |
| P1 | 1 |
| P2 | 9 |

---

## P1

### P1-1 ETW 开关"三重谎言"：失败不回滚、初始读 cfg 不读实际状态、toast 无条件报成功（欺骗用户）

- 位置：
  - `src/app/ui3/Pages3.cpp:226-231`（Checkbox 回调：`cfg.SetBool` + `SetNetEtwEnabled` + 无条件 `PushNote(Info, L"已开启按进程流量统计（ETW）")`）
  - `src/app/ui3/Pages3.cpp:161-164`（`etw_` 一次性从 `cfg.GetBool(L"netEtw")` 读取，config 是"唯一事实源"）
  - `src/collect/CollectService.cpp:310-324`（`SetNetEtwEnabled(true)` 时 `netEtw.Start()` 失败则 `netEtwEnabled` 保持 false，仅记日志）
  - `src/collect/NetTables.cpp:318-323`（非管理员 `StartTraceW` 返回非 0 → 仅 `STM_LOG_ERROR`）
- 场景（非管理员，常见运行态）：
  1. 启动时 cfg `netEtw=true` → `RegisterPhase3Pages`（Pages3.cpp:1373）调用 `SetNetEtwEnabled(true)` 静默失败；`NetworkPage` 勾选框仍按 cfg 显示为**勾选**，用户以为在采集，实际 `netBytesPerSec` 恒为 kUnavail（进程页"网络"列恒"—"）。
  2. 用户手动勾选 → `Start()` 失败 → 勾选框保持勾选 + toast 报"已开启"。本应用的核心卖点是诚实数据（驱动页宁可整页降级也拒绝显示假地址，Pages3.cpp:993-1003），此处 UI 状态与 toast 双双说谎，直接违背产品原则。
- 修法（三处一起改）：
  1. 勾选后读回实际状态：`ctx.collect.SetNetEtwEnabled(etw_); const bool actual = ctx.collect.NetEtwEnabled();` 不一致时回滚 `etw_` 与 cfg，并 `PushNote(JobFailed, L"开启失败：需要管理员权限（已回退）")`；
  2. 初始 `etw_` 改为 `cfg.GetBool(L"netEtw", false) && ctx.collect.NetEtwEnabled()`（Draw 时 collect 已 Start，读数真实）；
  3. 仅在 `actual == etw_` 时报"已开启/已关闭"Info toast。
- 备注：隐私文案本身已落实（"需管理员权限；在本地记录…仅本机使用、不上传"，Pages3.cpp:233-236）✓，问题只在状态反馈。

---

## P2

### P2-1 ETW Start/Stop 在 UI 线程同步执行，违反 §5"UI 线程禁止阻塞调用"
- 位置：`Pages3.cpp:228`（帧内 Checkbox 回调直接调 `SetNetEtwEnabled`）；`NetTables.cpp:294-334`（Start：先 ControlTraceW STOP 陈旧会话 + StartTraceW + EnableTraceEx2 + 起线程）；`NetTables.cpp:431-453`（Stop：ControlTraceW 停 64×64KB≈4MB 实时会话 + `consumer_.join()`）。
- 影响：勾选/取消瞬间阻塞 UI 线程。常态 10-100ms（帧预算 ≤3ms），Explorer 忙或缓冲未刷时更糟（collect 侧注释自认"block briefly"，CollectService.cpp:311-312）。启动时 `RegisterPhase3Pages` 的那次调用在帧循环外，无碍。
- 修法：开关走 jobs 队列（先例：签名校验同样在队列），UI 置"切换中…"过渡态；或至少文档化豁免并实测最坏时延。

### P2-2 JobQueue::Run 无异常屏障；producer 抛异常 = std::terminate，且 AsyncFetch busy 永久卡死
- 位置：`src/core/Jobs.cpp:90`（`job();` 裸调用，无 try/catch）；`src/app/ui3/AsyncFetch.h:59-70`（`busy=true` 后若 fn 抛出则永不复位，页面从此停在旧数据且强制刷新失效）。
- 说明：阶段 2 既有缺陷（SubmitKill 等同样裸奔），但阶段 3 新增 5 类 producer（连接表/启动项/服务/驱动/传感器）在串行队列上高频运行，扩大暴露面。`ReadSensors` 自带顶层 catch（Sensors.cpp:776-779）✓，其余 producer 为返回码式 Win32 调用，现实风险集中在 bad_alloc。
- 修法：`Run` 内 `try/catch(...)` 记日志即可（一行级改动，进程免死 + busy 可在 catch 中复位）。

### P2-3 DriverPage 缺"部分数据不可用"banner，错误被静默丢弃
- 位置：`Pages3.cpp:958-981`。`err` 非空且 `data` 非空时直接落入 `DrawTable`，err 无任何展示；网络/启动项/服务三页均有"部分数据不可用：{}"类 banner（Pages3.cpp:181-184、369-372、642-645）。
- 说明：`EnumDrivers` 现为全有全无契约，现实中暂无部分失败路径，但四页口径应一致，防契约日后放宽时静默。
- 修法：与 ServicePage 同款的 warn banner（两行）。

### P2-4 NetworkPage 进程名占位用 ASCII "-"，违反 §8 "kUnavail→'—'"统一口径
- 位置：`Pages3.cpp:316-322`（pid 非 0 且快照中找不到 → `name = L"-"`）。同页 UDP 远程端点（:79）、服务 PID/描述（:767、773）、驱动基址（:1107-1108）、传感器通电时长（:1273-1276）均为"—"。且没有说明是"进程未在快照中"还是"匿名"。
- 修法：改 `L"—"`，可加悬浮提示"该 PID 不在当前快照中"。

### P2-5 ServicePage 忽略 `ServiceInfo::canStop` 与 `startType==SERVICE_DISABLED`，提供必然失败的入口
- 位置：契约字段 `src/ops/ServiceOps.h:27`（`canStop`，"controls accepted from config"，枚举已带回）；UI 侧 `Pages3.cpp:786-801`（菜单只按 state 过滤）与 :682-686（工具栏）均未使用；启动项文案 :893-895 自己承认"启动失败时通常是因为……服务已被禁用"。
- 影响：对不可停止服务/已禁用服务的 停止/启动 必失败，靠失败 toast 兜底——诚实但多余；数据已具备，属一致性缺口（与启动项页的 canToggle 前置禁用口径不对齐）。
- 修法：菜单/按钮对 `!canStop` 禁用"停止…"、对 `startType==4` 禁用"启动…"并在禁用态旁注明原因。

### P2-6 tooltip 承诺"最快每 2/10 秒一次"与 force 绕过最小间隔的实现矛盾
- 位置：`Pages3.cpp:221-224`（net 刷新 tooltip）、:1139-1143（sensors）；`AsyncFetch.h:49-50`（`force ||` 直接短路间隔判断）。
- 影响：手动刷新不受最小间隔约束（busy 抑制仍在），tooltip 文案失真。
- 修法：文案改为"手动刷新立即执行，自动轮询最快每 2 秒一次"类表述；或 force 也过间隔（不推荐，影响手感）。

### P2-7 SetBalloonSink 的 lambda 按引用捕获栈对象且从不清理（静态槽悬挂隐患）
- 位置：`main.cpp:140-142`（`[&tray]`，tray 为 wWinMain 栈对象）；`Pages3.cpp:1358-1363`（`SinkSlot()` 静态存活到进程结束，无 `SetBalloonSink(nullptr)`）。
- 现状安全：帧循环先于 tray 析构结束；`tray.Remove()` 后 `added_=false` 使 `ShowBalloon` no-op（Tray.cpp:62-63）。属脆弱模式：任何人日后在退出路径之后再触发 sink 即 UAF。
- 修法：main 退出序列 `ui3::SetBalloonSink(nullptr);`（一行），或 sink 内改捕获 HWND。

### P2-8 ui3_test 覆盖面：仅 6 个纯函数映射测试；AsyncFetch / 告警状态机零覆盖且不可测
- 位置：`src/selftest/ui3_test.cpp`（状态标签×3、TcpState 包装、提权门、驱动降级触发，全过）。未覆盖：AsyncFetch（最小间隔抑制/busy 抑制/Submit 返回 0 回滚 :59-70）、`BecameActive`（:93-99）、告警冷却+5% 迟滞+再武装（:1376-1410）。告警逻辑位于 Pages3.cpp 匿名命名空间，现状无法在不引 ImGui 的情况下测试——与 PageHelpers"纯逻辑头"模式相悖。
- 修法：把 AlertState/告警判定与 AsyncFetch 的间隔判定抽成 header-only 纯函数（同 PageHelpers 模式）补测。微瑕顺带：`Pages3.cpp:622、959、1135` 三处 `BecameActive(lastFrame_)` 返回值被丢弃，lastFrame_ 沦为死状态，建议删调用或写明意图。

### P2-9 五页轮询共享串行 ops 队列，放大已登记的 V8-P2-10（排队数无 UI 显示）
- 位置：net 2s / services 5s / drivers 5s / sensors 10s（Pages3.cpp:328、589、935、1117、1310），全部走 `ctx.jobs` 串行队列；实测本机服务枚举单次 ~0.28s（日志 06:44:21.985 运行 22.400→22.680）。阶段 3 显著提高队列占用，而 §5"UI 显示排队数"仍未实现（V8 已登记 P2-10，PendingCount 仍无调用方，grep 验证）。
- 修法：随 V8-P2-10 一并处理（状态栏加 PendingCount）；轮询 job 可考虑在 Submit 前检查 PendingCount 让位于破坏性操作（非必须）。

---

## 已查无问题清单

1. **AsyncFetch 生命周期**：job 捕获 `shared_ptr<AppContext>` + `shared_ptr<State>`（AsyncFetch.h:59），页销毁/应用退出（Shutdown 超时 detach）后在途回调只写自持 cell，无裸指针 UAF；`Peek` 读写同锁，`Result` 发布后不可变，`lastData_` 仅作变更检测且所指对象由 `st_->result` 保活（不悬挂、不解引用陈旧指针）。
2. **最小间隔"停表"**：无定时器线程；`MaybeFetch` 仅由 `Draw` 驱动，页不可见即零调用零开销；`--smoke` 全页绘制属预期行为（日志证实服务/驱动/启动项在 smoke 下按 5s/2s 节奏轮询，间隔无违规）。
3. **破坏性操作一致性**：启动项启用/禁用、服务启动/停止的全部入口（工具栏按钮 + 行右键菜单）均走 ConfirmDialog → jobs → PushNote→toast，无绕过路径；两处对话框均为取消按钮首位 + `SetKeyboardFocusHere(0)` + 动作键 `ImGuiItemFlags_NoNav`（Pages3.cpp:552-565、897-910），与 V8-P1-2 基线一致；确认框打开瞬间 `pend_.item/svc` 值拷贝锁定，后台 refetch 换数据不影响目标；点外部/ESC 关闭均复位 pend_（:529-531、850-852）；服务停止的依赖服务计划走 `shared_ptr<DepPlan>` 异步查询（:805-841），取消后写入安全，与阶段 2 树杀计划同构。
4. **权限矩阵口径**：启动项 canToggle（HKCU 可写才免提权，StartupOps.h:24）与"需提权"徽标/按钮后缀/菜单禁用三处同源 `StartupNeedsElevation`；服务页全操作 `ctx.elevated` 门禁；驱动页 24H2 非提权整页降级 + 共享提权按钮；传感器 NeedAdmin 徽标 + 行内提权按钮；row 菜单与工具栏禁用态判定一致。
5. **ETW 接线时机**：`SetNetEtwEnabled` 在 `collect.Start()` 之前调用是安全的——Impl 构造即有效、EtwNetCollector 自持会话与消费者线程（NetTables.cpp:294-334）；`collect.Start()` 失败退出路径经 `~CollectService→Stop→~EtwNetCollector::Stop` 清理会话，无孤儿会话（会话名带 pid 防撞）；首 tick `prevNetBytes_` 为空不计速率，无假速率（CollectService.cpp:177-193）；计数器回退跳过该 tick（:184）。
6. **告警正确性**：首帧空快照 `cpuTotalPercent=kUnavail`(NaN) 被 NaN 守卫拦截、`physTotal=0` 跳过内存支路（ProcData.h:70-72），无误触发；冷却 5min + 回落 5% 再武装逻辑与 tooltip 文案逐句吻合（Pages3.cpp:1376-1410 vs :1424-1429）；每帧成本 = 3 次 cfg 线性查找 + 1 次 `Store().Get()`，远低于帧预算；`--smoke` 下 sink 未注册自动降级为 toast（main.cpp:118、146），兼容 exit 0。
7. **追加式回归**：五页追加在进程/性能之后（Pages.cpp:1264-1268），旧 session page 索引 0/1 不漂移，`ApplySession` clamp 兜底；`DrawShell` 两个新 hook（AlertTick :1314、DrawSmokeAllPages :1343）常态开销≈0；`DrawSmokeAllPages` 复用每页 Draw，ImGui ID 按窗口域隔离，无冲突；--smoke 150 实测 exit 0。
8. **空态/错误态/加载态**：五页均有 加载中…/全量错误+重试/空数据/过滤无命中 中文文案（唯一缺口见 P2-3）；UDP 状态列 UI 侧固定 `TextDisabled("—")`（Pages3.cpp:310-312），不依赖契约；TCP 行经 `UiTcpStateLabel` 非空兜底（PageHelpers.h:72-75），与契约 `TcpStateLabel(0)→hex` 不冲突；`kUnavail`/`kUnavailU64`/空串→"—" 映射齐全（:1273-1276、1107-1108 等）。
9. **运行验证**：`stm_selftest.exe` 40/40（含 ui3_service_state_labels / ui3_service_starttype_labels / ui3_startup_source_labels / ui3_tcp_state_wrapper / ui3_startup_elevation_gate / ui3_driver_err_needs_admin）；`SuperTaskMgr.exe --smoke 150` exit 0（复跑 2 次），日志无 ERROR/WARN，配置文件在 smoke 下不被写（main.cpp:176-179）✓。

## 结论

阶段 3 的追加式集成未破坏阶段 2 行为：确认框两段式基线、注册顺序、DrawShell 常态开销、session 索引兼容均验证通过。生命周期设计（shared AppContext + 不可变 Result cell）是本项目目前最干净的部分。必须修的是 P1-1（ETW 开关诚实性）——它是全应用"拒绝假数据"原则下唯一一处 UI 主动说谎的地方；P2-1/P2-2 建议随 P1-1 同批处理（同在 ETW 开关与队列路径上）。

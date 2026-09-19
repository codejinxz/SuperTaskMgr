# 终审评审 V24：兼容模式诊断体系（维护轮 10 加固）

- 评审人：终审 subagent V24（与其他评审者互不知情；禁 git；仅新增本报告文件）
- 日期：2026-09-20
- 对象：
  - `src/collect/SelfCheckGate.h/.cpp`（6 项逐项报告 + 参考进程重选 / 失败重试 2 次 / 连续 2 轮全过）
  - `src/collect/CollectService.cpp`（LastSelfCheckReport / RequestSelfCheckRetry）
  - `src/app/ui3/CompatDiag.h`（诊断报告文本 + 落盘）
  - `src/app/ui/Pages.cpp`（状态栏入口 + 兼容模式说明模态）
- 实跑环境：build/Release 产物（05:17 构建，晚于全部被审源码 03:02–03:46，非陈旧产物）

## 结论统计

| 级别 | 数量 |
|---|---|
| P0 | 0 |
| P1 | 1 |
| P2 | 4 |

实跑：`stm_selftest.exe` 127/127 通过；`--smoke 150` 退出码 0；`--autotest` kill / tree / startup / dialogclick / about / wallpaper 六条全部 PASS（exit 0）。

---

## P0

无。

## P1

### P1-1 「重新自检」完成检测有两处竞态：可在门重跑尚未发生（或报告未更新）时弹出错误结论并提前解禁按钮

- 位置：`src/app/ui/Pages.cpp:2640`（`compatRetryTickBase = ctx.collect.TickCount()`）、`src/app/ui/Pages.cpp:2666-2677`（`TickCount() > base` 即判"重跑完成"）；配合 `src/collect/CollectService.cpp:149`（`selfCheckRetry.exchange(false)` 在 DoTick 开头消费）与 `src/collect/CollectService.cpp:121-127`（`++ticks` 在 DoTick 返回之后）。
- 证据（竞态 A，跨 tick）：worker 正在执行第 N+1 个 DoTick 时用户点击 → 标志未及消费，base=N；DoTick(N+1) 结束后 `ticks=N+1` → UI 下一帧判 `N+1 > N` "完成"，但标志实际要到第 N+2 个 tick 才被消费。此时 `items`（Pages.cpp:2565 帧首取回）仍是旧报告 → 弹「重新自检仍未通过，保持兼容模式」，而真正的重跑还没发生；按钮也提前解禁。竞态 B（同帧）：`items` 在帧首 2565 取回，若本帧内 worker 完成重跑并 `ticks++`，2668 处用新 TickCount + 旧 items 判定，同样以陈旧逐项结果下结论。
- 影响放大器：降级机器恰恰走 Toolhelp+PSAPI 慢路径，DoTick 可达几十至上百 ms，"点击落在 DoTick 内"的概率不再是噪声级；而「重新自检」正是降级用户最常点的按钮，错误 toast（明明稍后就会通过，却告知"仍未通过"）直接打击该功能的可信度。
- 修法（推荐）：`Impl` 增加 `std::atomic<uint64_t> gateRunGen`，DoTick 每次真正跑完门后 `++`（与 `lastSelfCheck` 同锁写入顺序）；CollectService 契约加 `uint64_t LastSelfCheckGeneration() const`。UI 点击时记录 gen，检测到 `gen != base` 时重新调用 `LastSelfCheckReport()` 取新 items 再判 `retryOk`。次选（不扩契约）：toast 判定处重新拉取 items 且要求 `TickCount() >= base + 2`（付出 1 个采集周期的延迟，仍非严格严谨，仅作过渡）。

## P2

### P2-1 Esc 关闭模态不清理 `compatRetryPending`：完成 toast 迟到"复活"，暂停采集时按钮永久禁用

- 位置：`src/app/ui/Pages.cpp:2550`（`if (!s.compatDiagOpened) return;` 早退在 2668 完成检测之前）、`Pages.cpp:2636-2641`、`Pages.cpp:2661-2664`。
- 证据：等待重跑期间按 Esc → 模态关闭后完成检测不再执行，`compatRetryPending` 残留；之后任意时刻重开模态的第一帧 `TickCount()` 早已前进 → 立即用（可能已过时很久的）items 弹「重新自检通过/仍未通过」toast，与现场状态脱节。若期间用户「暂停采集」（`ctx.collect.Stop()`，Pages.cpp:2704），TickCount 不再前进 → 重开后按钮被 `BeginDisabled` 永久禁用且无任何解释。
- 修法：把完成检测移出模态体（DrawShell 每帧执行），或模态两条关闭路径（2661 [关闭] 与 2558-2560 Esc 复位处）一并 `compatRetryPending=false`；恢复采集（暂停→继续）时也应复位。

### P2-2 诚实性：诊断报告文本「任一项超差即整体降级」与加固后实现不符

- 位置：`src/app/ui3/CompatDiag.h:85`（`交叉比对 6 项，任一项超差即整体降级`）。
- 证据：加固后单轮超差不再直接降级——至多 2 次自动重试 + 需连续 2 轮全部通过（SelfCheckGate.cpp:380-405），瞬时超差被吸收、不降级（实机日志即为一例：第 1 轮私有工作集失败 → 重试 → 第 2、3 轮通过 → 放行）。「任一项超差即整体降级」夸大了灵敏度、与模态头部的加固描述（Pages.cpp:2585）不一致。触发环境三类归因（Windows 更新 / 安全软件挂钩 / 策略与虚拟化）两侧一致，且未夸大"已解决"——安全软件挂钩类持续超差仍会降级，措辞合规。
- 修法：改为「经至多 2 次自动重试后仍未达成连续两轮全部通过，才整体降级（瞬时读数抖动会被吸收；安全软件持续篡改则仍会降级）」。

### P2-3 `SystemVersionLine` 把 Windows Server 误标为 "Windows 10"

- 位置：`src/app/ui3/CompatDiag.h:45`（`dwBuildNumber >= 22000 ? L"Windows 11" : L"Windows 10"`）。
- 证据：Windows Server 2022（build 20348）、Server 2019（17763）等均 < 22000，被标为 "Windows 10"；诊断报告正是给用户贴 issue 用的，平台误标会误导排查方向。
- 修法：用 `GetProductInfo` / `RTL_OSVERSIONINFOW` + `wProductType != VER_NT_WORKSTATION` 判 Server，标出 "Windows Server" 家族。

### P2-4 `SaveDiagnosticsFile` 同一秒内互相覆盖；写失败残留半截文件

- 位置：`src/app/ui3/CompatDiag.h:106-119`。
- 证据：文件名仅精确到秒（`diagnostics_yyyymmdd_hhmmss.txt`），同一秒点两次「复制诊断报告」第二次覆盖第一次；`fwrite` 中途失败时返回空串（toast 提示失败，行为正确）但半截文件留在 logs 目录。均属边角，不影响主流程。
- 修法：路径已存在时追加 `_1/_2` 序号；失败时删除半截文件再返回空串。

---

## 已查无问题清单

1. **无死循环/无新死锁**：`kMaxAttempts=3` 有界（SelfCheckGate.cpp:380-405），失败轮间 200ms sleep，最坏 3 轮 + 400ms 后必然出结论；P-F-P 序列按「连续 2 轮」语义正确判降级。门在采集线程第一 tick 与重跑 tick 内联执行，不与 UI 互相持锁。
2. **降级保留最后失败轮证据**：`lastFailItems` 仅失败轮赋值（SelfCheckGate.cpp:383, 396-397），降级分支覆盖 `rep.items`（:416）；通过轮的 items 不会混入降级报告，反之通过时丢弃失败证据，结论与逐项恒一致（selftest `selfcheck_report_items` 有断言）。
3. **LastSelfCheckReport 线程安全**：`lastSelfCheck` 由 `Impl::mu` 保护（写 CollectService.cpp:160-161，读 :337-338）；`SelfCheckItem::name` 指向静态字面量、detail 深拷贝，跨线程持有安全；未自检返回空向量，UI 显式处理「尚未自检」。`degraded/degradeReason/gateDone` 仅工作线程访问，无竞争。
4. **重试协议**：`RequestSelfCheckRetry` 只置原子标志、不持 UI 锁跑门（CollectService.cpp:341-346）；多次请求 `exchange` 合并；成功后 `degraded` 归零自动退出兼容模式。
5. **参考进程重选**：自身优先、系统进程需存活 >60s 且（映像在系统目录 或 已知系统映像名）（SelfCheckGate.cpp:97-131）；每轮重取 NtQSI 快照；pid==0 / 无名行排除；OpenProcess 失败按"中途退出"少算对照数并触发重试，不误判为数据超差。
6. **6 项 detail 真实非空话**：通过项含参考进程数与明确容差（±1s / ≤25%+1MiB / ≤10%+4 / ≤10%+2 / 25%+2MiB），失败项含 PID + 双方实测值，跳过项区分具体原因（PDH 不可用 vs 未出现在实例表 vs API 不可读）；日志模块 selfcheck 逐项打印全部轮次。IO 项 NtQSI 侧 `ioOtherBytes=OtherTransferCount`（CollectUtil.cpp:156）与 `GetProcessIoCounters` 字段一一对应。
7. **私有工作集联接**：按 PDH「ID Process」pid 联接，与实例名 `#N` 后缀无关；PDH 仅此处一次性使用、绝不进 tick 循环；不可用/无可比实例诚实跳过（ran=false）不参与判定，一旦运行且超差则参与判定——与注释声明一致。
8. **诊断报告要素齐全**：生成时间 / 应用版本（`kAppVersion` 单一来源）/ 系统版本（`RtlGetVersion` 真实版本，不受清单声明虚报）/ 降级状态 / 逐项（通过/失败/跳过三态 + detail）/ 原因说明 / 受影响功能 / 隐私声明（仅本机、不自动上传），剪贴板与落盘共用同一文本；落盘 UTF-8 带 BOM、记事本直开，路径 `%LOCALAPPDATA%\SuperTaskMgr\logs\`。
9. **模态生命周期**：「请求长期有效 + 每帧渲染」模式；Esc 与 [关闭] 两条路径都复位 `compatDiagOpened`，无隐形模态残留；[重新自检] 期间 `BeginDisabled` + `AllowWhenDisabled` tooltip；模态打开期间采集继续（模态只读报告，不暂停采集）；等待期状态栏/表头随新快照自然刷新。
10. **状态栏入口**：仅 degraded 时渲染可点击文本，hand 光标 + tooltip，点击经一次性标志 `RequestOpenCompatDiag` 打开；锚点段按设计必显，后续段 `FlowSegmentFits` 避让，窄窗不互相压盖；长原因被窗口裁剪时命中区随之缩小但可见部分可点。
11. **契约兼容**：旧入口 `RunSelfCheckGate()` 委托加固版（SelfCheckGate.cpp:424），`CollectService.h` 契约未破坏；`compat_test.cpp` 明确声明超差分支无注入接缝属设计取舍，日志可人工验证。
12. **实跑证据**：selftest 127/127；--smoke 150 退出 0（首跑 exit=1 为与并行验证进程的 `SingleInstance` 互斥冲突，main.cpp:286-293 设计路径，非缺陷）；六条 autotest 全 PASS。实机日志真实演示加固路径：第 1 轮 [失败] 私有工作集（NtQSI=2.52MiB vs PDH=79.12MiB）→ 200ms 重试 → 第 2、3 轮全过 → 连续 2 轮放行，未误降级。

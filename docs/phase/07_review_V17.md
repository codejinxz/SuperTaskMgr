# 终审报告 V17（互不知情独立评审）

- 日期：2026-09-18
- 评审人：subagent V17（与其他评审者互不知情；禁 git；除本报告外未修改任何文件）
- 对象：P3 轮新增 —— 一键内存优化 / 关闭行为 / 性能图表可选化+硬故障与上下文切换图 / 崩溃解析重写 / 传感器多源
- 方法：逐行读码 + 真机事件数据核验（wevtutil 抓取 1000/1001/1002 真实 XML 人工推演）+ 全量实跑

## 实跑结果

| 项 | 结果 |
|---|---|
| build\Release\stm_selftest.exe | 85 通过 / 0 失败（含 crashlog_parse_wer_named、crashlog_parse_blob_unnamed、crashlog_parse_hang_1002、crashlog_parse_unnamed_falls_back_to_pid、crashlog_query_ok、ui_memcleanup_select） |
| --autotest kill | PASS，exit=0 |
| --autotest tree | PASS（终止 3 个），exit=0 |
| --autotest startup | PASS，exit=0 |
| --autotest dialogclick | PASS（真实管线点击生效），exit=0 |
| --smoke 150 | exit=0，stm.log 无错误 |

真机数据核验（wevtutil，本机 DKSKT-OZHOONXPQ）：
- Application 1000（.NET Runtime，无名 blob，EventRecordID 109109/109108）：人工推演 → provider=".NET Runtime"、blob 首行 Category 进摘要、无 *.exe 令牌 → Execution ProcessID 兜底 "PID 87360" —— 与代码逐行吻合。
- Application 1000（Application Error，EventRecordID 108836）：真实字段为 **ModuleName/ModulePath**（非 FaultingModule*）→ 见 P1。
- Application 1002（Application Hang，EventRecordID 109136，恰为 SuperTaskMgr.exe 自身）：AppName 正确、module 留空、headline=应用挂起 —— 正确。
- Application 1001（WER，EventRecordID 103667）：app 落到 WerFault 的 PID 而非 P1 参数 → 见 P2-1。
- Win32_PerfFormattedData_Counters_ThermalZoneInformation 实测 \_TZ.TZ00 raw=301 → 单位梯 0.1K 判 −243.05 被拒、0.1°C 判 30.1 °C 采纳 —— 与 Sensors.cpp:1481-1483 注释声称的实测值完全一致。

## P0（无）

未发现 P0。

## P1

### P1-1 崩溃解析：真实 Win10/11 Application Error 1000 的 ModuleName/ModulePath 未解析，「模块」列恒为 "—"

- 位置：`src/ops/CrashLog.cpp:394-397`（模块字段表 `{L"FaultingModule", L"FaultingModulePath", L"Module"}`）；对照 `src/selftest/control_test.cpp:47` 的 fixture（用的是 `FaultingModulePath`）。
- 证据（本机真实事件 108836，wevtutil 原文节选）：
  `<Data Name='AppName'>powershell.exe</Data> … <Data Name='ModuleName'>pdh.dll</Data> … <Data Name='ModulePath'>C:\Windows\SYSTEM32\pdh.dll</Data>`
  该 schema（AppName/AppVersion/ModuleName/ModuleVersion/ExceptionCode/AppPath/ModulePath/…）是 Windows 10/11 Event 1000 的标准字段集；FindNamed 是精确等值匹配（CrashLog.cpp:264-269），"Module" 匹配不到 "ModuleName"。事件全部字段带名 → Track B 的无名 blob 为空 → module 落空。
- 影响：GcPages.cpp:182 的「模块」列对最常见的应用崩溃行恒显示 "—"，摘要缺「（模块 pdh.dll）」。自测 fixture 与真机 schema 不符，测试通过造成覆盖假象（fixture 注释自称 "real-machine fixtures … captured by the architect"，但模块字段与本机实际不符）。
- 修法：ParseEventXmlImpl 模块字段表追加 `L"ModuleName", L"ModulePath"`（放在现有三项之后即可，精确匹配无前缀误伤）；并把 crashlog_parse_wer_named 的 fixture 换成本机捕获的真实字段集（或新增一条 ModuleName 变体用例），防回归。

## P2

### P2-1 WER 1001：app 兜底显示 WerFault 的 PID，而 P1 参数里就是崩溃应用名

- 位置：`src/ops/CrashLog.cpp:390-415`。WER 1001 命名字段只有 P1..P10（AppName/AppPath/Application 均不存在），全字段带名 → blob 空 → 最终落到 Execution ProcessID。
- 证据（真实事件 103667）：`<Data Name='P1'>powershell.exe</Data>` 存在，但推演结果 app="PID 24792"（24792 是 WER 报告进程，不是崩溃进程）。诚实但不准确，且摘要为「WER 报告：PID 24792」。
- 修法：Track A 前为 provider=Windows Error Reporting 加 `P1` 读取（或在通用兜底前尝试 FindNamed(L"P1")）；P1 值形如 powershell.exe，直接作 app。

### P2-2 一键优化批量 toast 丢失逐项失败原因与管理员提示

- 位置：`src/app/ui/ConfirmAction.h:293-300`（MakeMemCleanupJob：`err` 捕获后丢弃，仅 `RecordTrimResult` 计数）+ `MemCleanup.h:142-148`（toast 只报失败个数）。
- 证据：单项「释放工作集」路径（ConfirmAction.h:156-161）失败 note 带 `AdminHintSuffix` 与具体 err；批量路径同类失败只剩「（失败 N 个）」。非提权用户整批失败时看不到原因，违背 H5「失败必须可见原因」在本轮新路径上的延续。
- 修法：CleanupOutcome 增加 `firstError`（首条失败 err），FormatCleanupDoneText 追加「首因：{err}」，并在失败>0 时统一附 AdminHintSuffix(elevated)。

### P2-3 传感器单位梯：raw=0 被 0.1°C 解释采纳，显示 Ok 状态的 0.0 °C

- 位置：`src/collect/Sensors.cpp:1402-1404`（Win32_Temperature）与 `:1481-1483`（热区计数器）：0.1K 解释 −273.15 被拒 → 0.1°C 解释 0.0 落在 [−60,250] 内 → 以 Ok 展示 "0.0 °C"。
- 证据：窗口含 0，坏探针/未实现的 provider 常返回 0；与文件头「NEVER a 0 in place of data」红线冲突（对照 NvmeHealth 用 [−20,120]、WMI SMART 用 [10,120] 都把 0 挡住）。另注释「两解释不可能同时落窗」在 raw∈[2132,2500] 不成立（K 给 −59.9..−23.2，°C 给 213..250 均在窗内）——K 优先使其无害，但注释表述失真。
- 修法：两处下限收紧（如 c < 5.0 视为无数据跳过，或 c==0 && raw==0 直接 continue），并修正注释为「优先取 0.1K，重叠带物理上不真实」。

## P3（备案，不要求本轮修）

- closeAction 记住 1/2 后应用内无重置入口（tooltip 已如实说明删配置可恢复）——产品取舍，已文档化。
- WM_QUERYENDSESSION/WM_ENDSESSION（注销/关机）不走 WM_CLOSE 拦截路径，OS 直接终止时 session/cfg 不保存——既有行为，非本轮回归。

## 已查无问题清单

1. 保护名单在批量路径逐项生效：ops::TrimWorkingSet 每项先 `ProtectedReason(key)`（ProcessOps.cpp:423-432），名称空时现场解析镜像路径并做系统根同名校验防同名伪装（ProcessOps.cpp:482-501），OpenVerified 做 createTime 身份复核（:269）。批量 job 对每选中项独立走此门禁（ConfirmAction.h:293-299）。
2. Top10/默认勾选：stable_sort 稳定、未知工作集排尾且不可选（MemCleanup.h:47-78）；模态零勾选禁用执行钮（Pages.cpp:572-575）；无快照时入口无副作用（Pages.cpp:207）；候选行保护进程渲染为禁用（Pages.cpp:471-478）。
3. 失败计数真实：attempted 含失败项、freedBytes 仅计成功项（MemCleanup.h:132-139）；purge 失败独立 note 不并入计数（ConfirmAction.h:301-311）。
4. 模态每帧渲染正确：OpenPopup 仅一次 + BeginPopupModal 每帧（Pages.cpp:323-343），CloseAsk 复用同管线（:294-301）；Esc 关闭清态无僵尸模态（:335-343）；CloseAsk 焦点落在「取消」（:525）。
5. 关闭三分支：action 1→wantExit、2→SW_HIDE、0→closeAskPending（main.cpp:345-356）；NormalizeCloseAction 非法值归 0（ConfirmAction.h:89）；headless 退出全程不经 WM_CLOSE（--smoke/--autotest 均实跑验证）；托盘「退出」直通 wantExit（main.cpp:418）；WM_CLOSE 置 handled 不再销毁窗口（Win32Window.cpp:18-22 先于 switch），session+cfg 保存统一收敛在帧循环退出后（main.cpp:517-523），提权重启路径先行 SaveSession（main.cpp:414-416，Pages3.cpp:133-147 同）——握手完整。
6. 「记住我的选择」时机：按钮按下时 SetInt、退出统一 Save（Pages.cpp:520-524 + main.cpp:519）；取消不写（:543）；模态重开时 rememberChoice 复位（:297-299）。
7. 性能图表可选化：perfShow* 勾选即写 cfg、退出持久化（Pages.cpp:1910-1939）；两列网格以 beginCell/endCell 空组保序，块=复选框+图无空洞；Ring::hasData 仅非 NaN 置位（:609-617），硬故障/上下文切换诚实空态「本机此计数器不可用」（:2025-2055）；上下文切换求和逐项跳过 NaN、全 NaN 推 NaN（:658-667），与 PDH 无系统级计数器的口径说明一致；硬故障直读 sys.hardFaultsPerSec（SystemCollector.cpp:97）。
8. 崩溃解析其余路径：单/双引号属性 + 属性整词匹配（CrashLog.cpp:146-167）；数值实体含 &#xA;/&#13;、&amp;lt; 不二次解码、裸 & 保留（:73-121，真实 .NET blob 的 &#xA; 换行解码验证通过）；CDATA 直通（:125-133）；无名 blob 首行进摘要 + 首 *.exe / faulting module 后 *.dll 启发式（fixture 与真实事件双重验证）；PID 兜底永不「未知」（:411-415）；两通道归并稳定 + 双通道错误聚合（:505-538）；EvtNext 批句柄 RAII（:445-452）。
9. 传感器多源：ACPI 热区 0.1K 单解释（Sensors.cpp:458）正确；四类来源前缀互不冲突（ACPI 热区 N/WMI 温度/WMI 热区计数器/DPTF 温度/磁盘 WMI SMART）；热区计数器 raw=301 实测→30.1 °C 通过；DPTF 双命名空间去重（:1503-1528）；extra 空组不显示；WmiQuery 枚举期 ACCESS_DENIED 归 NeedAdmin（:342-351）。
10. LHM AMD 归类边界：LhmGroupOfCpuTemps 只在 base=Other 且路径含 "temperature" 且含 core/package/tctl/tdie/ccd 时晋级 CPU（Pages3.cpp:100-111）——"Voltages/VCore"、"Clocks/Core #1" 无 temperature 令牌不入 CPU 组；带 mem/ssd 等令牌的行 base 已非 Other 不受影响。
11. 实跑全绿：selftest 85/85、--autotest 四条 PASS、--smoke 150 exit=0。

## 统计

- P0：0；P1：1；P2：3；P3：2（备案）。

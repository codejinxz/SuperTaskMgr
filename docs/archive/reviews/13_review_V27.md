# V27 终审报告（性能图表 Phase B/C/D）—— 2026-09-20

评审人：终审 subagent V27（与其他评审者互不知情；禁 git；除本报告未改任何文件；仅运行 build/Release 产物）。
对象：`src/app/ui3/PerfChart.h`（FollowTick/PerfHistory 扩展/CoreHeatmap/BuildAdapterSeries/既有 CopyRingTail）、
`src/app/ui/Pages.cpp` PerfPage（放大还原、检视态、hover 读数、Y2 副轴、每核热图、每适配器、时间窗×tickSec×SetupAxisLimits 联动）、
`src/collect/SystemCollector.cpp`（diskQueueDepth PDH、GetIfTable2 按 ifIndex 差分）。
设计基准：`docs/phase/08_chart_design.md`；V21-P0/V23 复核口径（CopyRingTail 线性化 + tickSec xscale）为既定基线，不得回退。
方法：通读三文件 + vendored ImPlot 1.0 源码逐函数推演（implot.cpp UpdateInput/SetupFinish/SetupAxisLimits/ApplyNextPlotData、implot_internal.h SetupLock/ImPlotAxis::Reset）+ ui_chart_test.cpp 覆盖核对 + 全量产物实跑。
产物新鲜度：build/Release（SuperTaskMgr.exe 与 stm_selftest.exe 均今日 11:15）晚于全部被审源码（最新 Pages.cpp 11:10），非陈旧产物。

## 0. 结论统计

| 级别 | 数量 |
|---|---|
| P0 | 0 |
| P1 | 1 |
| P2 | 8 |

实跑：`stm_selftest.exe` **146/146 ×2 轮**（exit 0，collect_tick_latency 本轮 PASS，无环境性失败）；`--smoke 150` exit 0；`--autotest` kill/tree/startup/dialogclick/about/wallpaper 六条全 PASS（exit 0，autotest_result.log 尾 6 条逐一核对）；伪场景 `--autotest bogus_scenario` 正确 FAIL（exit 1），验证了判定器本身不作假。

FollowTick 状态机对 vendored ImPlot 1.0 的适配经源码推演**成立**：`SetupAxisLimits(Cond_Always)` 立即 `SetRange`（implot.cpp:2200-2205），`SetupFinish` 内 `UpdateInput`（:2696，定义 :1870）把平移/滚轮/框选释放（:2075-2090）与双击 fit（:1900-1905）同帧叠加/标记在该 Range 上，故 `EnlFollowDetect`（SetupFinish 后取 `GetPlotLimits` 比对）同帧检出用户动作；检视态依赖 `ImPlotAxis::Reset()` **不清 Range**（implot_internal.h:723-740）与 plot 池按 ID 持久化，成立。V21-P0 基线未回退（PlotRing 全调用点走 CopyRingTail 线性化 + Offset=0 + xscale）。

## 1. P0

无。

## 2. P1（应修，1 个工作日内）

### P1-1 悬停读数时间标签「N 秒前」按 tick 数硬编码，intervalMs≠1000 时系统性失实
- 文件：`src/app/ui3/PerfChart.h:347-349`（`Fmt(L"{} 秒前", count - 1 - idx)`）；消费点 `src/app/ui/Pages.cpp:2189-2196`（EnlReadoutTooltip）。
- 证据：V21-P0 起 x 轴为真实秒（xscale=tickSec，Pages.cpp:1970，`tickSec_ = max(200, intervalMs)/1000.0`），采样间隔是用户可调的 **500–5000 ms**（Pages.cpp:3081 `SliderInt(L"刷新间隔", &interval, 500, 5000)`，ClampInterval [500,5000]）。PerfReadoutText 的首行时间标签把「tick 序差」直接当秒：500ms 档下 10 tick 前的样本实际是 5 秒前，tooltip 却写「10 秒前」（放大 2 倍）；5000ms 档反向缩小 5 倍。同函数其余序列值均正确（经 ReadoutIndexAt 用 tickSec 反推，PerfChart.h:204-211，selftest 已覆盖），唯独标签失实——违反「诚实数据」总则，且是用户可达配置，非理论边界。
- 修法：`PerfReadoutText(hist, blockId, idx, double tickSec)` 增参（默认 1.0 兼容既有自测），标签改 `Fmt(L"{:.0f} 秒前", (count - 1 - idx) * tickSec)`（或按量级格式化「x 分 x 秒前」）；ui_chart_test 补 tickSec≠1 用例钉死。

## 3. P2（建议修）

- **P2-1 GPU 利用率小图固定 600 样本窗，不随时间窗档位**（Pages.cpp:2591 `DrawGpuUtilSpark(hist, cellW, sparkH, kHistCap)`）：其余 8 块均按 perfWindowSec 取尾窗，GPU 块（含放大态）恒显满容量；同屏口径不一致。修法：把当前 window 传入 DrawGpuUtilSpark。
- **P2-2 提交占比序列无 hasData 门，极端机器出空图例项**（Pages.cpp:2248 恒 `SetupAxis(Y2)`、:2263 恒 PlotRing；对照磁盘队列的 `showQueue = hist.diskQueue.hasData` 门，:2273）：commitLimit 恒 0（NtQSI+GetPerformanceInfo 双失效）时全部 NaN 仍注册图例「提交占比」。修法：与 diskQueue 同款 hasData 门。
- **P2-3 NtQSI 每核失败拍使 cores ring 与系统 ring 永久失步 1 样本**（SystemCollector.cpp:228-245 失败时 `perCorePercent` 留空；Pages.cpp:727-734 空表拍跳过 Push）：此后 cores 环比 cpuTotal 环恒短 1+，CPU/每核曲线右端提前 1+ 秒收尾，tooltip 最新 idx 处核心列恒「—」（PerfReadoutText 以 cpuTotal.Count() 为统一时钟，PerfChart.h:345）。触发率极低但失步不可自愈。修法：失败拍按核数推 NaN（保持同步推进），或读数端按各环自身 Count() 对齐。
- **P2-4 每核热图放大态暴露无效交互**（Pages.cpp:2355-2386）：BeginPlotBox(enlarge) 给 Crosshairs 且未禁框选，但 X/Y 均被 Cond_Always 每帧覆盖，框选一帧后回弹；「跟随最新/回到最新」控件照常渲染但状态机不参与。修法：heatmap 分支用 NoBoxSelect、隐藏跟随控件（tooltip 文案同改）。
- **P2-5 Phase D CSV 适配器列未按设计落地 + GPU 列口径与图表分叉**（PerfCsv.h:23-30/62-67）：08 设计 §6-Phase D 明确「届时统一加『适配器N收/发』列（一次破坏性变更）」，实现未加且无书面决定；「GPU利用率%」列取 `gpus[0]`（首卡），图表与读数为多卡合计钳 100（GpuUtilClampSum），CSV 与曲线不可互证。修法：补适配器列（变长列需在表头说明）或书面记入 backlog；GPU 列改多卡合计并在注释/表头声明。
- **P2-6 网络块放大 tooltip 缺每适配器序列行**（PerfChart.h:380-382 只列接收/发送总量；Pages.cpp:2322-2327 绘制 1–8 条适配器线）：设计 §2.2 口径为「tooltip 逐序列列值」。修法：kPerfBlockNet 分支追加 netAdapters/netAdapterNames 逐行。
- **P2-7 容量语义是样本数而非秒数，「600 秒」档在快节奏下历史不足 600 秒**（PerfChart.h:36 kHistCap=600 按 1Hz 假设；08 设计 §2.5 同假设）：500ms 档 600 样本仅 300s，时间窗选 600 时曲线只占左半轴（视觉如数据停在 300s 处）。显示本身诚实（轴为真实秒），但与档位承诺不符。修法：容量改按 `kHistCap = 600s / intervalMs` 动态或在档位 UI 注明；至少登记 backlog。
- **P2-8 队列深度读数注释口径措辞失准**（PerfChart.h:328-329「Current Disk Queue Length 为浮点均值口径」）：Current 为瞬时采样值（非 Avg.）；实现选 Current 而非设计 §3.4 的 Avg. Disk Queue Length 属合理偏离（无需 /sec 归一、更贴任务管理器语义），仅注释需更正为「瞬时值，1 位小数」。

## 4. 已查无问题清单

1. **FollowTick×ImPlot 1.0 全链路**：SetupLock 在首个 plot item / EndPlot / GetPlotLimits(:3794) 惰性锁；UpdateInput 于 SetupFinish 内、Cond_Always SetRange 之后执行 → 平移/滚轮/框选释放同帧可检（框选在 release 帧直接 SetMin/SetMax，:2075-2090）；检视态持久化成立（Axis::Reset 不清 Range，implot_internal.h:723-740；plot 池按 ID 复用）；kEps=1e-6 与双精度直赋比对无假阳；双击 fit 在跟随态被下帧 Cond_Always 覆盖（1 帧闪烁，可视为「跟随态不可 fit」的合理语义，检视态正常）。
2. **缩略图/放大共用 plot ID（##cpu 等）无串扰**：flags 逐帧 diff 后重应用；缩略图 NoBoxSelect、放大 Crosshairs；缩略图平移被下帧 Cond_Always 自愈。
3. **放大/还原会话一致性**：perfZoom 持久化、越界 ClampZoomBlock 回 -1；zoomApplied_ 变化（含重启恢复）即复位跟随；还原按钮与 Draw 层双重复位；放大态下该块显隐关断仅藏体不藏头，可还原。
4. **ReadoutIndexAt 数学**：round 吸附、shown=min(count,window)、(count-shown)+local 反推、越界/NaN/tickSec≤0 全 -1；与 PlotRing 首点=窗内最老（x=0）逐点对齐；selftest 双用例（含 count=610/window=60 裁剪）覆盖。
5. **CopyRingTail 基线未回退**：MakeRingView/RingViewChunks/两段 memcpy 与 V23 复核逐点口径一致；线程局部缓冲无嵌套调用；PlotRing 全调用点（含 netAdapters/gpuUtil）走线性化路径。
6. **diskQueue PDH 口径**：跳 `_Total` 防重、无单盘有效值回退 `_Total`、与速率同查询同节奏；`diskQueue_` 失败路径留 null（PdhAddCounterW 失败不写 out，CollectUtil.cpp:340-344）+ `if (diskQueue_)` 双防御；无计数器→kUnavail→hasData 诚实空态；Y2 无数据时整轴隐藏（Pages.cpp:2273-2281）。
7. **CommitPercent**：limit==0→NaN（绝不除零/伪 0%）；uint64 比值无下溢；Y2 0-100 固定 + AuxDefault；selftest 三断言覆盖。
8. **GPU 2s 摄取**：CollectService.cpp:228-250 采集中间拍复用 lastGpus → AppendHistory 每 tick 有值，「阶梯无锯齿」注释成立；sys.gpus 空→不推→诚实空态；GpuUtilClampSum 跳 NaN、全 NaN→NaN、钳 100（与按进程聚合同口径，selftest 覆盖）。
9. **热图**：行=核心、列=尾窗、行主序 [core*cols+t] 与 PlotHeatmap bounds/Invert 推演一致；cols=min(120,window)；不满窗左侧 NaN 补位（最新贴右）；0-100 固定色标 Viridis；NaN 格渲染空白；selftest 3 组断言覆盖。
10. **GetIfTable2 差分**：首拍/新接口/单侧计数回退→该侧 kUnavail（`r.InOctets >= pr->second` 防下溢，SystemCollector.cpp:166-175）；prev 表整拍替换（回退后下拍自愈）；总量 V6-P1-2 口径独立保留、回退时总量 kUnavail 而单适配器仍有效——两者并存无矛盾；名称链 Alias→Description→`接口 N`；typeLabel 复用 IfTypeLabel 无第二张表。
11. **BuildAdapterSeries**：NaN 排末/双 NaN 等价、流量降序为严格弱序、stable_sort+Top8 截断、回环/非 Up 排除；重建条件=排序后 id 集变化（防流量排名抖动重建史，Top8 成员冻结到下次接口集变化为有意取舍）；重建后 ids/names/rings 三表对齐；UI「共 X 个仅显示 Top8」口径正确。
12. **组合矩阵逐项推演**：放大态切时间窗（跟随重设/检视保持）、放大态切显隐、热图↔曲线切换（含放大中切）、每适配器开关（缩略/放大）、计数器空态块放大（诚实文案撑高）、CSV 记录与显隐/放大正交——均无错位。
13. **CSV**：表头/行列数严格一致（csv_header_roundtrip 锁定），新列「磁盘队列」NaN→空单元格，摄取点与 ring 同一 tick 去重门。
14. **实跑与判定器**：selftest 146/146×2（exit 0，含 collect_tick_latency PASS）；--smoke 150 exit 0；六条 --autotest PASS（日志逐条核对）且伪场景 exit 1——判定器不作假。
15. **cfg 键名与设计 §5 一致**：perfWindowSec/perfZoom/perfPerCoreHeatmap/perfShowNetAdapters，默认值内联（120/-1/false/false），旧配置零迁移。

## 5. 证据索引

- 实跑：selftest 双轮（`合计：146 通过，0 失败`，exit 0×2）；`--smoke 150` exit 0；`%LOCALAPPDATA%\SuperTaskMgr\logs\autotest_result.log` 尾 7 条 = 六场景 PASS + bogus FAIL。
- 源码核对：PerfChart.h/Pages.cpp(SystemCollector.cpp) 行号见各条目；ImPlot：implot.cpp:2200-2205（SetupAxisLimits Cond_Always 立即 SetRange）、:2696（SetupFinish→UpdateInput）、:1870-2090（输入全量）、implot_internal.h:1314-1319（SetupLock）、:723-740（Reset 不清 Range）。

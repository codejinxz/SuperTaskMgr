# 08 性能页图表设计规格（R-ChartEval 评定稿）

> 评审对象：`src/app/ui/Pages.cpp` PerfPage（8 块两列网格、逐块显隐 cfg perfShow\*、ImPlot 1.0、
> 120 点@1Hz、CSV 导出）、`AppendHistory` 摄取点、`docs/phase/01_架构设计文档.md` §8/§10、
> README 功能清单、`third_party/implot/implot_demo.cpp` 能力盘点。
> 性质：设计规格（只读评定），供实现 agent 直接照做。本文档不改动任何源文件。

---

## 1. 首选方案总述

**首选 C：混合布局——保留两列网格作"扫视层"，点击块标题放大为全宽"详查层"。**
系统监控是高频扫视场景：网格保证 8 项指标一屏同现（现状 A 的核心价值），而 tooltip、
拖拽平移/框选缩放、精确读数等交互只放在放大视图——这同时消解了"1Hz 追加 vs 用户缩放"
的冲突（缩略图永远锁定跟随，放大视图才有跟随开关）。任务管理器式左栏（B）把一屏拆成
两条窄带，牺牲扫视密度且改造量大；单页滚动分组（D）让指标在滚动中离开视野，与扫视场景
直接矛盾。放大层还天然承接后续时间窗 60–600s 与新增序列。

判定细节：
- 现状网格代码（BeginGroup 单元格 + perfShow\* 复选框）原样保留，仅在每格标题行加一个
  「放大」入口（Selectable 覆盖标题或标题行按钮），新增运行态 `enlarged_`（cfg 持久化）。
- 放大块 = 全宽 + 高度 ×1.8 + 增强交互（见 §2），网格其余块照常。再点「还原」返回。
- GPU 块（现为表格）同样可放大；放大时追加适配器利用率历史曲线（见 §3.2）。

---

## 2. ImPlot 能力利用（逐项评定）

| # | 能力 | 结论 | 理由与实现要点 | 量 |
|---|------|------|----------------|----|
| 2.1 | 图例点击隐藏序列 | **采纳（已具备，零代码）** | 本版 ImPlot 默认 `SetupLegend(NorthEast)` 未设 `ImPlotLegendFlags_NoButtons`，图例图标即点击隐藏/显示按钮；右键图例菜单也默认可用。仅需在验收时确认 + README 提一句 | S |
| 2.2 | hover 精确值 tooltip | **采纳** | 缩略图维持默认鼠标坐标文本（现状已显示）；放大视图加"吸附读数"：鼠标 x 取整到采样栅格（1Hz，`idx = round(mouseX)`），tooltip 逐序列列值 + 时间标签。纯函数 `PerfReadoutText(hist, blockId, idx)`（ui3/PerfChart.h，可单测）。绘制用 `ImPlot::GetPlotMousePos` + `ImGui::SetTooltip`，每帧一次字符串拼接仅放大态发生 | S |
| 2.3 | 拖拽平移/框选缩放 | **采纳（仅放大视图）** | 冲突根源：缩略图 X 用 `SetupAxisLimits(0..W-1, ImPlotCond_Always)` 每帧覆盖，任何用户缩放下一帧即被冲掉。策略=分层：<br>① 缩略图：维持 `Cond_Always`（永远跟随，禁用无意义的框选——加 `ImPlotFlags_NoBoxSelect` 防误操作）；Y 按 2.5 策略。<br>② 放大视图：跟随态每帧 `SetupAxisLimits(X, newest-W+1, newest, Cond_Always)`；用户一旦平移/框选（检测：`BeginPlot` 后 `ImPlot::GetPlotLimits()` 与上一帧设定值差 > ε）自动切非跟随态——非跟随态不再调用 SetupAxisLimits(X)，由 ImPlot 保存用户视窗；提供「跟随最新」复选 + 「回到最新」按钮（置回跟随态）。<br>跟随状态机提为纯函数 `FollowTick(bool follow, bool userMoved, bool resumeReq) -> bool`（可单测） | M |
| 2.4 | Y 轴自适应与固定范围 | **采纳（分图固定策略）** | 现状缺陷：内存/磁盘/网络 Y 轴仅首帧 fit，之后流量峰值越界即削顶。修正：<br>① CPU/每核：固定 0–100（不变，扫视可比性优先）；<br>② 字节类（内存/磁盘/网络）缩略图：Y 加 `ImPlotAxisFlags_AutoFit` 每 tick 自适应；<br>③ 放大视图非跟随态：去 AutoFit，允许用户框选缩放 Y；回到跟随态恢复 AutoFit；<br>④ 网络对数轴：**不采纳默认对数**——Log10 画不了 0（网络空闲常态为 0），SymLog 对普通管理员不直观；线性+自适应在 1Hz 粒度下已可读（ImPlot 轴右键菜单本就允许用户手动切比例，不写代码） | S |
| 2.5 | 时间窗可调 60/120/300/600s | **采纳** | `kHistCap` 120→600（内存代价 26 ring × 600 × 4B ≈ 62KB，可忽略）；Ring 恒按 600 容量摄取，绘制经纯函数 `RingView(const Ring&, int window)` 返回 `{data, count=min(Count(),W), offset=(head-W+cap)%cap}` 零拷贝取尾窗（现 PlotRing 的 values-only + Offset 机制直接支持）。工具条加 Combo（cfg `perfWindowSec`，默认 120=现状）。窗口只影响显示不影响摄取，切换零迁移 | M |
| 2.6 | ImPlotScale_Time 绝对时间轴 | **不采纳** | 相对秒 + 自定义 formatter 更轻；values-only Ring 无需引入 double 时间数组；时间轴自带本地化/格式开关是额外成本，60–600s 窗口内"秒前"语义反而更贴合扫视。tooltip 里用本地时刻（快照 timestamp）补足绝对时间语义 | — |
| 2.7 | 阈值线可视化 | **采纳（低优先）** | `DrawAlertControls` 已有 CPU/内存阈值（cfg alertCpu/alertMem，默认关）：开启时在 CPU、内存缩略图叠 `ImPlot::DragLineY(…, ImPlotDragToolFlags_NoInputs)` 常量线，告警触发后 5 分钟内在对应块画一次 `ImPlot::Annotation` 标记。纯渲染增强，成本可忽略 | S |

## 3. 新增图表候选（逐项评定）

| # | 候选 | 结论 | 理由与实现要点 | 量 |
|---|------|------|----------------|----|
| 3.1 | 每适配器吞吐 | **采纳（预留接口，等数据层）** | 数据层 `GetIfTable2` 按接口差分即将落地。现在只做三件预留：① 契约占位——`SystemInfo` 增 `std::vector<NetAdapterRate> netAdapters;`（`{uint64_t ifIndex; std::wstring name; double recvBps, sendBps;}`，缺省 kUnavail）；② `PerfHistory` 增 `std::vector<Ring> netAdapters` + 名称表，AppendHistory 尾部按"核心 rings 同款"模式逐接口 Push（口径变化→重建、历史重启，与 cores 一致）；③ UI 块 `perfShowNetAdapters`（默认关）待数据源落地后启用。CSV 暂不加列（列数变更推迟到数据落地同 Phase，避免两次破坏格式） | S（UI 侧） |
| 3.2 | GPU 曲线 | **部分采纳**：适配器利用率历史**采纳**；按进程 Top5 曲线**不采纳**。适配器 1–2 条线（2s 节奏）：Ring 增"缺席 tick 沿用上次值"策略（纯函数 `CarryPush(Ring&, float, bool hasValue)`），放大数据层 2s 粒度避免锯齿 NaN；渲染进 GPU 块（放大态显示）。按进程曲线否决理由：PID 漂移/进程退出使序列频繁断裂，Top5 排名秒级换血，曲线语义不成立，现有 Top5 表格已覆盖需求 | S / — |
| 3.3 | 提交内存占比 | **采纳** | 内存块加副轴（`SetupAxis(ImAxis_Y1, …)` 之外 `SetupAxis(ImAxis_Y2, L"占比", ImPlotAxisFlags_AuxDefault)`，0–100 固定）序列 `commit/commitLimit×100`，formatter `%g%%`。commitLimit=0（异常）时推 NaN 诚实跳过。副轴只占 ~24px，缩略图可读 | S |
| 3.4 | 磁盘队列长度（PDH） | **采纳** | `SystemCollector` 在现有 PDH 会话追加 `\\PhysicalDisk(_Total)\\Avg. Disk Queue Length`（与现有磁盘字节同查询节奏，增量 ~µs 级）；`SystemInfo::diskQueueLength`（kUnavail 诚实缺省）；磁盘块 Y2 副轴渲染。部分机器无计数器→NaN→沿用 `hasData` 诚实空态模式（队列长度有数据但全 0 也如实画 0）。CSV 增「磁盘队列」列（表头/行纯函数同步改，`csv_header_roundtrip` 锁列数） | M |
| 3.5 | 逐核热力图 | **采纳（可选视图，默认关）** | 行=核心、列=时间窗，`ImPlot::PlotHeatmap`（0–100 固定标尺，Colormap 用默认 Jet/ Viralis 系）。成本核算：16 核 × 600 点 = 9600 float（38KB/帧上传）≈ 9600 格，每格 2 三角形 → ~38k 顶点/帧，实测 ImPlot heatmap 在万格级 <0.5ms，可入 3ms 预算（见 §4）。cfg `perfPerCoreHeatmap`（默认 false）切换"每核：曲线/热图"，互斥渲染。纯函数 `CoreHeatmapValues(cores, window) -> vector<float>`（行主序展平 + 尾窗裁剪，可单测）。热图胜在多核密度扫视（>16 核机器显著优于 16 条曲线）；默认仍曲线，尊重现状 | M |

## 4. 性能红线论证（§10 预算：UI 空闲帧 ≤3ms）

逐帧成本构成（最坏组合：600s 窗 + 16 核 + 全部显隐开 + 放大态）：

| 项 | 估算 | 依据 |
|----|------|------|
| 折线顶点 | 系统图 ~7 序列 ×600 = 4.2k 点；每核曲线 16×600 = 9.6k 点 | ImPlot 线图经验 ~20–40ns/点（含剔除+变换+顶点），≈ 0.3–0.6ms |
| 热图 | 9600 格 ≈ 38k 顶点 | 万格级 <0.5ms；且热图/曲线互斥，不叠加 |
| 每图固定开销 | 8–9 张 × BeginPlot/Setup\*/EndPlot ≈ 10–20µs/张 | ≈ 0.15ms |
| 轴 formatter | `FmtBytesAxis` 每 tick 标签一次 WideToUtf8+FormatBytes：~8 标签×2 轴×9 图=144 次/帧 | ≈ 0.1ms；必要时缓存上次标签串（S 优化备选） |
| 摄取 AppendHistory | O(procs) 每 tick 一次（1Hz 非 60fps）：500 进程 ctx 聚合循环 ≈ 数 µs | 与 UI 帧预算无竞争 |
| 悬停读数 | 仅放大态 1 帧 1 次字符串拼接 | 可忽略 |

**合计 ≈ 1.2–1.8ms，预算内留 ~40% 余量。** 保护措施：① 缩略图始终 `Cond_Always` 无交互查询；
② 500 进程不进 UI 帧（快照一次读，页内不遍历 procs——现状已是，必须保持）；③ 现状
`TopGpuProcs` 合并为 O(n²)（gpuProcs 数十行无碍），登记为观察项不上预算；
④ 验收门：`--smoke` 帧 + 状态栏 `ctx.frameMs`（§8 自省）在 600s/16 核/热图开档位
p95 ≤3ms，超限先降热图默认关、再降窗口档默认值（写入回归记录）。

跟随实现要点（对应任务书"SetNextPlotLimits 与自动跟随/shrink-rasterize"）：
每帧渲染前计算 `xMin = newest - W + 1, xMax = newest`（newest=Count-1，经 RingView 对齐），
跟随态 `SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always)` 在 `SetupFinish` 前提交
（"每帧 shrink 后再栅格化"，用户视窗必然回到最新）；非跟随态不提交 X limits，由 ImPlot
内部持久化用户视窗；以 `GetPlotLimits()` 差值检测用户动作并经 `FollowTick` 状态机落态。

## 5. 与现有 cfg（perfShow\*）的兼容与迁移

- **零迁移**：Cfg 为扁平 KV（`core/Cfg.h`），`perfShowCpu/PerCore/Mem/Disk/Net/HardFaults/
  CtxSwitch/Gpu` 八键语义不变（仍是块级显隐复选框），老配置原样生效；新键
  `perfWindowSec`(int, 120)、`perfZoom`(int, -1)、`perfPerCoreHeatmap`(bool, false)、
  `perfShowNetAdapters`(bool, false) 均带默认值，旧配置文件缺键无感。
- 跨版本安全：新版读旧配置=取默认；旧版读新配置=未知键被忽略（find-or-append 槽位不冲突）。
- `ui3::ColWidthCfgKeys()` 白名单只覆盖 colW_\*（恢复默认列宽），不涉及本批键，不改动。
- CSV：仅 Phase C 增「磁盘队列」一列（新文件自带新表头，旧 captures 文件不受影响），
  `PerfCsvHeader/PerfCsvRow` 同步改并更新 `csv_header_roundtrip` 自测；每适配器列推迟到
  数据层落地同一 Phase，不做两次破坏性变更。

---

## 6. 分阶段实现规格

### Phase A 体验基线（缩略图修正 + 时间窗 + 读数）——量 M
**改哪些文件**
- `src/app/ui/Pages.cpp`：`kHistCap` 120→600；`PlotRing` 改用 `RingView` 取尾窗；字节类
  图 Y 轴加 `ImPlotAxisFlags_AutoFit`；工具条加时间窗 Combo（cfg `perfWindowSec`，
  `ClampWindowSec` 合法化 60/120/300/600，非法回落 120）；CPU/内存缩略图叠告警阈值线
  （alertOn 时，`DragLineY` NoInputs）。
- `src/app/ui3/PerfChart.h`（新增，纯函数头）：
  - `int ClampWindowSec(int64_t v)` — 60/120/300/600 白名单；
  - `struct RingView { const float* data; int count; int offset; }` +
    `RingView MakeRingView(const Ring&, int window)` — 尾窗零拷贝；
  - `std::wstring PerfReadoutText(const PerfHistory&, int blockId, int idx)` — 放大态读数
    （NaN→"—"，诚实口径）；
  - `bool FollowTick(bool follow, bool userMoved, bool resumeReq)` — 跟随状态机。
  （Ring/PerfHistory 结构体需随迁或前置声明；建议把 `Ring`/`PerfHistory` 移入该头，
  Pages.cpp 与 selftest 共用。）
- `src/selftest/ui3_test.cpp`：新增 ClampWindowSec/MakeRingView（含不满窗、回绕 offset、
  window>Count 边界）/PerfReadoutText（NaN 路径）用例。

**验收标准**
1. 时间窗切 60/600：曲线立即只显示尾窗，历史不重置（600s 窗填满后仍每秒右移一格）。
2. 磁盘/网络/内存图在运行中流量冲高后 Y 轴自动扩，不再削顶；CPU 图恒 0–100。
3. 图例点击可隐藏/恢复任一序列（默认能力，人工确认）。
4. cfg 手工写入非法 `perfWindowSec=137` → 回落 120 且不崩。
5. `stm_selftest` 新用例全绿；`--smoke 300` 通过；状态栏帧耗时与改动前持平（≤±0.3ms）。

### Phase B 放大视图（C 布局核心）——量 M
**改哪些文件**
- `src/app/ui/Pages.cpp`（PerfPage）：`beginCell/endCell` 单元格标题行加「放大/还原」
  Selectable；成员 `enlarged_`（cfg `perfZoom`，-1=无，0–7=块索引，越界回落 -1）；
  放大块全宽、`plotH*1.8f`，网格跳过该块（该块占独立行）；放大态启用：
  - X 框选缩放/拖拽平移（移除缩略图加的 NoBoxSelect）；
  - 「跟随最新」复选（默认开）+「回到最新」按钮；每帧 `GetPlotLimits()` 与上帧设定差值
    >ε 经 `FollowTick` 判定用户动作→自动退出跟随；
  - 吸附读数 tooltip（`PerfReadoutText`）+ `ImPlotFlags_Crosshairs`。
- `src/app/ui3/PerfChart.h`：`PerfReadoutText` 已在 A；如需按块元表（标题/显隐键/绘图器
  分发）可加纯数据表 `BlockMeta kBlocks[8]`（标题、cfg 键、绘制 id），消除 8 段重复代码
  （可选重构，行为不变）。
- `src/selftest/ui3_test.cpp`：FollowTick 三态迁移表用例（follow→userMoved→resume）。

**验收标准**
1. 点「放大」该块独占全宽，其余块继续实时滚动；「还原」恢复网格，enlarged_ 重启会话保持。
2. 放大态拖拽/框选生效；一旦手动操作，X 不再每帧跳到最新（自动非跟随）；点「回到最新」
   或勾选跟随，右缘重新对齐最新采样。
3. 缩略图（非放大态）框选被禁、永远跟随，任何缩放冲突不复现。
4. 悬停放大图显示逐序列精确值 + 偏移秒；NaN 序列显示"—"。
5. `--smoke` 与帧预算复测通过。

### Phase C 新序列与热图——量 M
**改哪些文件**
- `src/core/ProcData.h`：`SystemInfo` 增 `double diskQueueLength = kUnavail;`。
- `src/collect/SystemCollector.cpp/.h`：现有 PDH 会话追加 `\\PhysicalDisk(_Total)\\Avg. Disk
  Queue Length`；取数失败保持 kUnavail；selftest（collect_test.cpp）补计数器存在/缺失两路
  诚实性用例（缺失机器断言 NaN 而非 0）。
- `src/app/ui/Pages.cpp`：`AppendHistory` 增 `Ring diskQueue` 摄取（NaN 照推）；内存图 Y2
  提交占比序列（`commit/commitLimit`，limit≤0 推 NaN）；磁盘图 Y2 队列长度（hasData 空态
  同硬故障模式）；GPU 块增适配器利用率 Ring（`CarryPush` 2s 沿用策略，放大态渲染）；
  每核块热图/曲线切换（cfg `perfPerCoreHeatmap`，默认曲线）。
- `src/app/ui3/PerfChart.h`：`float CarryPush(Ring&, float v, bool hasValue)`、
  `std::vector<float> CoreHeatmapValues(const std::vector<Ring>&, int window)`、
  `double CommitPercent(uint64_t commit, uint64_t limit)`（limit≤0→NaN）。
- `src/app/ui3/PerfCsv.h`：表头与行尾增「磁盘队列」列（AppendDouble，NaN→空格）。
- `src/selftest/`：ui3_test 增 CarryPush/CoreHeatmapValues/CommitPercent；
  csv_header_roundtrip 更新列数断言；collect_test 增队列长度用例。

**验收标准**
1. 磁盘图出现队列长度副轴曲线；无计数器机器显示既有「本机此计数器不可用」诚实态。
2. 内存图 Y2 占比与状态栏/内存条「提交 x%（物理/提交上限）」口径一致。
3. GPU 适配器利用率历史曲线 2s 一点、无锯齿断裂（采集缺席时沿用旧值）。
4. 热图开关互斥正确：热图行=核心、列=时间、色标 0–100 固定；16 核帧耗时 p95 ≤3ms
   （600s 窗、热图开，记录进回归）。
5. CSV 新文件末列为磁盘队列，NaN 为空格；selftest 全绿。

### Phase D 每适配器吞吐（预留，blocked-on 数据层）
**范围**：数据层（GetIfTable2 按接口差分）落地后启用。本阶段只交付规格约定的预留位：
- 契约：`SystemInfo::netAdapters`（§3.1 结构），由 collect 差分填充（回退/换接口集→该 tick
  kUnavail，与 V6-P1-2 口径一致）；
- UI：`PerfHistory::netAdapters` rings + 名称表已在 Phase C 前的 AppendHistory 模式位；
  「网络」块加「总计/按适配器」下拉（cfg `perfShowNetAdapters`，默认总计），适配器序列用
  ImPlot 自动配色 + 图例可点隐藏；
- CSV：届时统一加「适配器N收/发」列（一次破坏性变更）。
**验收标准**：多网卡机器逐接口曲线与系统总计吻合（求和口径）；VPN 拆链当 tick 显示"—"
空态；无新增预算超标。

---

## 7. 不采纳名单（含理由）

| 项 | 理由 |
|----|------|
| B 任务管理器式左栏缩略图+点选大图（整体布局） | 一屏两条窄带，扫视密度下降；8 块信息不可同览；改造量 M-L 收益不抵。放大需求由 C 方案的点选放大覆盖 |
| D 单页滚动分组 | 指标随滚动离开视野，违背"高频扫视"；分组的认知收益用现有块标题+显隐已覆盖 |
| 网络对数 Y 轴（默认） | Log10 无法表示 0（网络空闲常态）；SymLog 认知成本高。轴右键菜单已允许用户手动切换，不设默认 |
| GPU 按进程 Top5 历史曲线 | 2s 节奏 + PID 漂移/退出导致序列断裂与排名换血，曲线语义不成立；现有 Top5 表格保留 |
| ImPlotScale_Time 绝对时间轴 | 相对秒更轻且贴合扫视语义；避免时间格式/时区成本；绝对时刻由 tooltip 读数承担 |
| 缩略图启用拖拽/框选缩放 | 与 1Hz 追加每帧覆盖 X limits 冲突；缩略图定位扫视，交互收敛到放大视图 |
| ImPlot Subplots 重构网格 | 网格布局已由 ImGui 承担且带显隐/两列逻辑；Subplots 迁移成本 M、无对应收益 |
| 悬浮独立图表窗口 | 架构 §8 已裁定暂缓（无多视口，需独立 Win32 顶层窗口另评） |
| 新序列默认全开 | 违背"显隐=用户选择"现状契约；除队列/占比等副轴轻量序列外，新视图默认关 |

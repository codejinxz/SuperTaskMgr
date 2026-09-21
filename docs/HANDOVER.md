# SuperTaskMgr 项目转接文档（Handover）

> 版本：**v1.0.0**（构建日期 2026-09-21）｜仓库：本地 Git，19 个提交，**尚未配置远端**
> 本文档面向接手的新 agent / 新会话：读完后应能独立构建、测试、继续开发。
> 配套文档：`README.md`（用户视角）、`docs/phase/`（51 份各阶段报告与评审记录）、`docs/research/`（6 份技术调研）。

---

## 1. 项目一句话概述

Windows 平台的「超级任务管理器」：**单文件便携 exe（约 2.9MB、静态 CRT、零第三方 DLL）**，纯 C++20 自绘界面（Win32 + Dear ImGui 1.92.9 + D3D11 + ImPlot 1.0），覆盖进程全维度监控、终止/进程控制、内存清理、启动项/服务/驱动管理、网络（含 ETW 与可选 Npcap 抓包）、硬件传感器（含可选 PawnIO 内核温度）、性能图表、日志查看与诊断上报。

**核心产品原则（贯穿全部代码，改代码时不得违背）**
1. **诚实数据**：拿不到就显示 `—` / "需提权" / "需驱动"，**绝不显示 0 或假值冒充**。
2. **零持久化 / 零隐蔽 / 零联网上传**：不写自启动、不驻留服务、无监听端口、不上传任何数据。
3. **破坏性操作双层防线**：UI 二次确认 + ops 层保护名单硬拒绝；保护名单内的进程不可杀、不可挂起、不可 trim。
4. **内核组件全 opt-in**：本体零驱动；可选增强依赖**用户自行安装**的官方签名运行时（PawnIO / Npcap），不捆绑、不分发、不自动安装。
5. **全 Unicode**：一律宽字符 API，中文路径与进程名全程正确。

---

## 2. 快速上手（三条命令）

```bat
:: 1) 完整构建（Debug/Release）
scripts\build.bat Release

:: 2) 自测（176 项，退出码 = 失败数；--json 输出机器可读结果）
build\Release\stm_selftest.exe

:: 3) GUI 无交互冒烟 + 真实输入管线回归（均 exit 0 为通过）
build\Release\SuperTaskMgr.exe --smoke 150
build\Release\SuperTaskMgr.exe --autotest dialogclick
build\Release\SuperTaskMgr.exe --autotest kill       :: 另有 tree / startup / about / wallpaper
```

日常开发最省事：双击仓库根目录 **`一键编译.bat`**（编译 → 跑自测 → 询问是否启动）。

**构建环境要求**：Windows 10 21H2+ / 11 x64；**VS2022 Build Tools**（含 MSVC v143 与 CMake 组件）。构建脚本不依赖 `vcvarsall.bat`（改用 VS 生成器自定位工具链），可从任意 shell 调用。CMake 路径在 `scripts/build.bat` 内硬编码 + `vswhere` 兜底。

---

## 3. 技术栈与架构

### 3.1 技术选型结论（详见 `docs/phase/00_技术选型评估报告.md`）

在 6 条候选路线（Qt6 / Win32+ImGui / WinUI3 / Avalonia / Tauri / 原生 Win32 自绘）中选定 **Win32 + Dear ImGui + D3D11 + ImPlot**（加权 4.75/5）：POD 快照直供立即模式 UI、零胶水、单 exe 最小分发面、双 MIT 许可、提权无框架约束。Qt6 为备案（触发条件：无障碍/原生观感成为硬需求，代价是 UI 全量重写）。

### 3.2 分层与依赖方向

```
app(UI) ──> ops ──> core
   └─────> collect ──> core        ops ↛ collect（禁库依赖；ops 允许自行最小重枚举）
```
- `src/core`：数据契约 + 平台工具（无 UI、无业务）。**stm_core**
- `src/collect`：快照采集。**stm_collect**
- `src/ops`：破坏性操作/提权/签名/服务/启动项/驱动/崩溃查询。**stm_ops**
- `src/app`：UI 壳 + 各页。**SuperTaskMgr**（WIN32 子系统）
- `src/selftest`：控制台自测。**stm_selftest**

### 3.3 线程模型

| 线程 | 数量 | 职责 | 约束 |
|---|---|---|---|
| UI | 1 | 消息循环、ImGui 帧、模态、通知 drain | 帧预算 ≤3ms；禁阻塞调用 |
| 采集 | 1 | 周期快照写入 SnapshotStore | tick p50 ≤15ms @500 进程 |
| 操作 | 1 | JobQueue 串行执行慢/破坏性操作 | 禁止 UI API |
| 网络监视 | 1 | NetMonitor 1s 差分连接表 | 免管理员 |
| ETW 消费 | 0-3 | Kernel-Network / DNS-Client 各自的 ProcessTrace 线程 | 仅管理员，RAII 清理 |
| Npcap 消费 | 0-1 | pcap_next_ex 抓包解析 | 仅管理员 |

**同步原语仅四种**：`SnapshotStore`（mutex+shared_ptr）、`JobQueue`（mutex+condvar，退出时丢弃未开始任务、在途任务最多等 2s）、`NotificationQueue`（UI 帧内 drain）、各采集器内部互斥。跨线程无裸指针（job lambda 一律按值捕获 `shared_ptr<AppContext>`）。

### 3.4 数据流

```
采集线程 → Snapshot（不可变 shared_ptr）→ SnapshotStore
UI 每帧: Store().Get() 一次 → 各页只读消费
UI 操作 → JobQueue → ops 执行 → NotificationQueue → UI toast/状态栏
```

### 3.5 关键设计决策与理由

1. **进程身份 = (PID, CreateTime) 二元组**（`ProcKey`）：PID 会被复用，所有跨帧引用、终止前校验、树杀枚举都必须比对 createTime（±1s 容差）。**这是全项目最常被违背的规则**，改任何涉及进程的代码前先确认。
2. **采集走 NtQuerySystemInformation(SystemProcessInformation) 半文档化结构 + 启动自检门**：一次调用取全（CPU/IO/句柄/线程/父链），但字段偏移未文档化 → 启动时用文档化 API（GetProcessTimes/GetProcessMemoryInfo/GetProcessIoCounters/GetProcessHandleCount）交叉比对 **6 项**，连续 2 轮全过才启用；失败重试 2 次后降级到 Toolhelp+PSAPI 慢路径（"兼容模式"，缺失列显示"—"）。
3. **提权模型 1A**：`asInvoker` 启动 → 需要提权的功能带徽标 → 一键 `ShellExecuteExW(runas)` 重启自身；重启前把会话状态原子写入 `session.json`（页签/选中 ProcKey/排序/窗口矩形/间隔），旧实例退出释放命名互斥体，新实例 `CreateMutex` 等待 ≤1000ms 后恢复。
4. **PDH 一律用 `PdhAddEnglishCounterW`**：中文系统计数器名本地化，英文路径是唯一稳的写法（本机 zh-CN 实测 11 条路径全通）。
5. **图表 x 轴线性化**：ImPlot 的 `spec.Offset` 按"绘制点数"取模而非环容量 → 环形缓冲必须先经 `CopyRingTail` 线性化并以 `Offset=0` 绘制；x 轴用 `tickSec` 换算真实秒。**（V21 P0 血泪教训）**
6. **布局防抖三件套**：状态栏用定宽槽位（`StatusLayout.h`），页面区块用定高滚动区（`PageLayout.h`），列宽用 `NetColCfgKey` 持久化 + 布局重置代际（`LayoutResetGeneration`）。
7. **ImGui 模态必须每帧 `BeginPopupModal`**：只在请求帧渲染会变成"僵尸模态"（吞点击）；`SetKeyboardFocusHere` 只能在 `IsWindowAppearing()` 时调一次（每帧调会偷走鼠标按住的按钮 ActiveId）。**（用户报告的"点击无反应"根因）**

---

## 4. 目录结构

```
CMakeLists.txt            5 个 target（3 静态库 + GUI + selftest）
一键编译.bat              一键构建+自测
scripts/build.bat         官方构建入口（Release/Debug）
scripts/make_logo.bat     重新生成应用图标（调用 tools/gen_logo.cpp）
src/core/                 契约与工具：ProcData.h(数据契约) Log.* Str.* Err.* Cfg.* Jobs.*
                          Notifications.* FsUtil.* Privilege.* ProtectedList.* HandleGuard.h LogFile.h(日志读取)
src/collect/              CollectService.* CollectDetail.h(内部头) CollectUtil.* ProcessCollector.*
                          SystemCollector.* GpuCollector.* SelfCheckGate.* NetTables.*(连接表+ETW)
                          NetMonitor.* AdapterInfo.* Sensors.* LhmSource.* PawnIoLink.*
                          NpcapSource.* NpcapParse.h KernelProbe.*
src/ops/                  ProcessOps.* ProcessControl.* MemoryOps? Signature.* ServiceOps.*
                          StartupOps.* DriverOps.* CrashLog.* Elevate.* SingleInstance.* SessionState.*
                          DetailsProvider.*
src/app/                  main.cpp AppContext.h Win32Window.* D3DRenderer.* ImGuiLayer.* Theme.*
                          AboutInfo.h(发布信息) app.rc(图标+版本) app.manifest AutotestDialog.* Logo.*
  ui/                     Pages.cpp(壳+进程页+性能页+模态) SortKey.h StatusLayout.h UIPage 契约 IPage.h
                          AboutUi.* Tray.* UiText.h VersionInfo.h ConfirmAction.h HeaderLayout.h ModulesUi.h
  ui3/                    Pages3.cpp(网络/启动项/服务/驱动/传感器) GcPages.*(崩溃/窗口页)
                          PerfChart.h PerfCsv.h MemCleanup.h ProcKind.h ProcTree.h JumpState.h
                          NetMonUi.h NetAdapterUi.h PcapUi.h CompatDiag.h LogViewer.h Splitters... 
                          PageLayout.h StatusLayout.h ThemeCfg.h *(持久化键登记与清理)
src/selftest/             176 项用例（框架 TestFramework.h 的 STM_TEST 宏）
assets/                   app.ico / logo_256.png / 各尺寸预览（由 tools/gen_logo.cpp 生成）
tools/gen_logo.cpp        图标生成器（程序化绘制 + 手写 ICO/PNG 容器）
third_party/imgui/        vendored v1.92.9（MIT）
third_party/implot/       vendored v1.0（MIT）
third_party/stb/          stb_image.h v2.30（公有领域）
docs/phase/               各阶段报告 + 35 份评审/复核记录（见 §9）
docs/research/            R1-R6 技术调研（API 矩阵/权限/开销，含官方引用与本机实测）
```

---

## 5. 关键契约头（冻结，改前先看）

| 契约 | 内容 | 备注 |
|---|---|---|
| `core/ProcData.h` | `ProcKey` / `ProcInfo` / `SystemInfo`(含 `netAdapters`/`diskQueueDepth`) / `Snapshot` / `SnapshotStore` / `kUnavail` 约定 | **改动需极谨慎**，牵动全仓 |
| `core/LogFile.h` | 日志尾读与解析、级别过滤、上报格式 | header-only 纯逻辑 |
| `collect/CollectService.h` | 采集服务接口 + `SelfCheckItem`/`LastSelfCheckReport`/`LastSelfCheckGeneration`/`RequestSelfCheckRetry` | |
| `collect/NetMonitor.h` | 连接事件/远程聚合/DNS 三类接口 | |
| `collect/AdapterInfo.h` / `Sensors.h` | 适配器信息 / 传感器四态快照 | Sensors.h 允许纯增量扩展 |
| `ops/ProcessOps.h` | **含强制执行协议注释**：身份重验 → 保护名单硬门 → 树杀两段式 | 新 ops 必须照做 |
| `ops/ProcessControl.h` | 挂起/恢复/优先级/亲和性 | 挂起关键进程比杀死更危险，四操作全过保护名单 |
| `ops/{ServiceOps,StartupOps,DriverOps,CrashLog}.h` | 服务/启动项/驱动/崩溃记录 | 启动项写前强制备份 |
| `app/AboutInfo.h` | **发布信息单一来源**（版本/日期/仓库地址） | 改这一个文件即可发版 |

**契约纪律**：契约头由架构师写入并冻结；开发 agent 只实现 `.cpp`；确需变更时在当轮报告登记并同步更新相关 selftest 断言。

---

## 6. 已实现功能清单（v1.0.0）

**进程**：13 列全量表格（CPU/私有工作集/提交/磁盘/网络/硬故障/句柄/线程/上下文切换/徽标/描述）、平铺↔树形、区分系统进程三态（关闭/高亮/只看用户）、列拖动重排+列宽持久化、过滤、双击详情（路径/签名/命令行/用户名/GDI+USER/**模块列表+按模块签名**/控制区/父进程跳转）。
**进程控制**：终止、进程树终止（两段式+逐成员重验+保护成员跳过报告）、挂起/恢复、优先级六档（Realtime 警示）、CPU 亲和性逐核。
**内存**：一键内存加速（Top10 勾选批量释放工作集 + 可选清待机缓存，诚实文案）、单进程释放工作集。
**性能**：8+ 块图表（CPU/每核/内存/磁盘/网络/硬故障/上下文切换/GPU/每适配器）、时间窗 60/120/300/600s、放大详查+跟随/检视状态机、每核热图、Y2 副轴、拖拽告警阈值线、CSV 记录导出。
**网络**：适配器信息卡（类型/IPv4/IPv6/网关/DNS/MAC/速度/DHCP/复制 IP）、连接表、**实时监视**（连接事件流+过滤+Top 远程目标+DNS 记录+CSV）、**深度抓包**（Npcap，含载荷 hex 视图）、纵向可拖拽分栏。
**启动项**：注册表 Run/Run32、启动文件夹、计划任务、UWP 四源枚举与启停（写前备份可逆）。
**服务/驱动**：服务枚举/启停（依赖树警告）、驱动列表+按需签名校验（24H2+ 需提权）。
**硬件传感器**：CPU 每核频率占用、ACPI 热区逐实例、**PawnIO 每核 DTS 温度/风扇/电压**、SMART（NVMe/SATA 细项）、GPU 逐传感器（NVML）、电池、内存/分页池、LHM 可选数据源、分组显隐+详细模式。
**其他**：崩溃记录页、窗口管理页、日志查看器（过滤/诊断报告生成/打开目录）、兼容模式可点击诊断（逐项测量值+重试+报告导出）、主题三态（深/浅/跟随系统）、自定义壁纸（遮罩可调+性能提示）、一键优化布局+重置、任务栏/托盘图标、全局热键、阈值告警、关闭行为三选、单实例、会话交接。

---

## 7. 测试与验证体系

**三层验证（每一轮改动都必须全过）**：

| 层 | 命令 | 覆盖 | 判据 |
|---|---|---|---|
| 自测 | `stm_selftest.exe` | 176 项：契约/纯函数/真实 API 行为/权限降级 | 全绿（`--json` 可机器读） |
| 冒烟 | `SuperTaskMgr.exe --smoke 150` | 全部页签离屏渲染 150 帧 | exit 0 |
| 输入注入 | `--autotest {dialogclick,kill,tree,startup,about,wallpaper}` | **走真实 ImGui 输入管线**：注入鼠标点击确认按钮/关于按钮，断言动作真的生效 | 全 exit 0 + `autotest_result.log` 全 PASS |
| 构建 | `scripts\build.bat Release` 与 `Debug` | /W4 全仓 | **0 错误 0 警告** |

> `--autotest dialogclick` 内含"模态单帧化"哨兵（把旧 bug 复原会 FAIL）——这是防止历史缺陷复发的核心保险。

**已知 flaky**：`collect_tick_latency`（tick p50 ≤15ms）对宿主负载极敏感，VM/高负载时偶发假阴性。**判定方法**：若 tick 路径源码零改动（`git diff` 该目录为空）且基线二进制同期同样失败，即为环境性，可注明后放行；否则需查。

**纯函数优先原则**：所有可测逻辑（排序比较、解析、布局数学、配置键、状态机）抽 header-only 纯函数放 `ui3/*.h`，供 selftest 直接断言——这是本仓 176 项用例能覆盖 UI 逻辑的原因。新增功能必须同时加纯函数与其用例。

**验证前置条件（踩过的坑）**：
1. 跑 `--smoke`/`--autotest` 前**必须确认没有正在运行的 SuperTaskMgr 实例**——它持有单实例互斥体，headless 启动会按设计直接 `return 1`（避免 CI 被弹窗阻塞），表现为"六条 autotest 全失败"的假象。
2. 应用运行期间会独占 `logs/stm.log`，此时外部工具（含本仓日志读取器）无法打开它；要看日志请先退出应用。
3. 提权实例（`runas` 启动）无法被普通权限 shell 终止（access denied）——遇 `build/` 目录被占用，先关闭应用；或换目录构建（`cmake -S . -B build_x -A x64`）。
4. Git Bash 调用 cmake 需 `MSYS_NO_PATHCONV=1`（否则 `/m` 等参数被路径转换破坏）；GUI 子系统 exe 的退出码建议用 PowerShell 获取。
5. 构建/验证动作会改动 `%LOCALAPPDATA%\SuperTaskMgr\` 下的 cfg/session——自动化验证前建议备份用户配置，验证后还原。

**v1.0.0 转接时的验证快照**：Release/Debug 双配置 **0 错误 0 警告**；`stm_selftest` **176/176 连续两轮通过**；`--smoke 150` 与六条 `--autotest` 在**同一份源码**上于转接前一轮已全 PASS（其后仅新增本文档，不影响二进制）。转接时因存在一个提权运行中的应用实例占用互斥体与日志文件，未能重跑后两项——如需复核，关闭该实例后重跑即可。

---

## 8. 权限与安全（红线清单）

| 项 | 实现 |
|---|---|
| 提权 | `asInvoker` + `ShellExecuteExW(runas)` 重启；`ERROR_CANCELLED` 静默回退 |
| SeDebugPrivilege | 仅提权实例，按需启用、**用后即释**，每次启用写日志 |
| 保护名单 | `core/ProtectedList.h`：PID 0/4 + 关键映像名（csrss/lsass/wininit/winlogon/services/smss/Registry/Memory Compression/dwm 等）；**双层**：UI 禁用 + ops 硬拒；同名伪造防护（名单命中需路径在 SystemRoot 下，路径未知时保守保护） |
| 破坏性操作 | 全部二次确认（确认框取消键首位 + 破坏性按钮移出键盘导航） |
| 启动项写 | 写前备份原值到 `logs/startup_backup/`（时间戳命名保留历史；计划任务额外备份 XML） |
| 内核组件 | PawnIO：模块 **SHA-256 白名单**校验、只读寄存器、SuperIO 进入/退出序列 **RAII 配对**、`PawnIoShutdown` 生命周期；Npcap：仅本机抓包、载荷 ≤256B、不做 .pcap 导出 |
| 无隐蔽能力 | 全仓 grep 验证：无自启动写、无服务安装、无监听端口、无注入 API（WriteProcessMemory/CreateRemoteThread） |
| 日志隐私 | 日志**不记录命令行/窗口标题**；诊断报告内置隐私提示；不自动上传 |

---

## 9. 评审历史（35 份评审/复核记录 + 设计规格，docs/phase/）

工作模式：**每轮 = 并行开发 subagent（文件归属互斥）→ 集成 → 新一批互不知情的评审 subagent → 修复 → 复验**。评审记录编号 V1-V34，阶段报告 00-16。

**发现并修复的重大缺陷（按严重度）**
- **P0 壁纸永不渲染**：绘制调用在 `NewFrame()` 之前，ImGui 背景绘制列表因 g.Time 戳永不并入 DrawData（V18 以独立探针实证）。
- **P0 图表整窗画错数据**：ImPlot `spec.Offset` 语义与环形缓冲 offset 不匹配，稳态下 7 个图表画旧数据（V21）。
- **P0 网络监视三重缺陷**：IPv4 事件被 IPv6 差分清空、两个 ETW 功能会话互踢静默失效、远程端口未换字节序（443 显示成 47873）（V25）。
- **P0 传感器幻影磁盘 / NVMe+ATA 偏移错误**：`CreateFileW` 失败返回值判断错误虚构 30 块盘；温度/通电时长字段偏移错（V9）。
- **P0 手写 PNG 压缩器缺陷**：距离码编码错误导致 256px 资产全损（V19）。
- **用户报告的"点击无反应"**：单帧模态 + 每帧抢焦点偷走 ActiveId（F1，实证 + 输入注入回归兜底）。
- 另有 P1 级 30+ 项（UAF、句柄/SAFEARRAY 泄漏、竞态、诚实性缺陷等）。

**评审报告的读者指南**：`*_review_V*.md` / `*_verify_V*.md` 是发现记录（含文件:行、证据、修法）；`*_维护报告.md` / `*_阶段报告.md` 是汇总与决策；`08_chart_design.md` 是图表设计规格；`11_kernel_research.md` 是内核路线调研。

---

## 10. 已知限制（诚实清单，勿当 bug 修）

1. **Npcap 免费版不支持静默安装**（官方限 OEM），且**许可禁止外部再分发** → 用户需自行从 https://npcap.com/#download 安装；仓库不含安装包。
2. **笔记本风扇转速**通常由 EC 控制，LpcIO 白名单不含 EC 端口 → 诚实显示"无数据"；LpcIO 芯片表仅覆盖常见 ITE/NCT。
3. **每核 DTS 温度/风扇/电压需 PawnIO**；未安装时如实标注。
4. ETW Kernel-Network 的部分字段（direction）在个别系统缺失 → 按事件 ID 判向、字段缺失退化为 pid 级并计数。
5. 兼容模式（Toolhelp 慢路径）下部分列显示"—"（半文档化结构自检未通过时）。
6. PDH 计数器（如 `\Memory\Hard Faults/sec`）在个别系统缺失 → `—`。
7. Windows 11 24H2+ 驱动列表需提权（系统行为）。
8. StartupApproved / UWP StartupTask 注册表语义为社区考证+实测（非官方文档化），标记实验性，写前有备份兜底。
9. ImGui 自绘 UI 的原生限制：无 UIA/屏幕阅读器支持、IME 有边角细节、中文列排序为码点序。
10. 未签名 exe：SmartScreen/AV 可能提示。
11. 空载内存 ≈128MB（D3D11 运行时+驱动+字体图集基线，长跑零增长）；启动 ≈350ms。
12. 定高常量按 DPI 缩放系数（`LayoutScaleFromEnv`）缩放，极端 DPI 下可视行数会减少。
13. 浅色主题 + 高亮壁纸的可读性依赖用户调遮罩。

---

## 11. Backlog（按建议优先级）

1. **Npcap 真机抓包验收**（代码就绪、解析纯函数有单测；待用户装驱动后跑手工验收清单）。
2. 图表放大态的适配器序列 tooltip 行；每适配器 CSV 列。
3. 崩溃记录页导出（CSV/文本）。
4. Find Window（拖拽十字定位窗口所属进程）。
5. PerfCsv 记录中逻辑核数变化时的行列对齐。
6. HostSvcCache / CtrlSlot 缓存淘汰策略。
7. `EllipsizeTextUtf8` 的 O(N²) 优化（量级微小）。
8. 浅色主题 × 壁纸对比度增强（按壁纸亮度自动选文字色）。
9. `colOrder` 恒等时的噪音键清理；`colOrderAppliedGen_` 死字段。
10. LpcIO 芯片表扩展（SMU/EC 类）。

---

## 12. 开发工作流约定（重要，接手请沿用）

1. **每轮先结构化任务**：用 `prompt` 技能（`/prompt`）按「目标 / 上下文 / 约束 / 完成标准」四要素写任务书，再派 subagent。
2. **文件归属互斥**：并行 subagent 必须各自拥有互不重叠的文件集合（本项目的固定分区：`ui/Pages.cpp` 归一个、`ui3/Pages3.cpp` 归一个、`collect/*` 按模块分），跨文件需求在报告登记由集成者执行。
3. **契约先行**：新增能力先由架构师写好冻结契约头，再让开发 agent 实现 `.cpp`。
4. **每轮必过验收门**：双配置 0 警告 + selftest 全绿 + `--smoke 150` + 六条 `--autotest`。
5. **每轮开新评审批次**：2-3 名互不知情的评审 subagent，从不同维度（正确性 / 泄漏与生命周期 / 安全与性能）审查，发现的问题**当轮修完并复验**。
6. **诚实记录**：环境性失败、无法验证项、降级方案都要如实写入报告，不得粉饰。
7. 术语与风格：代码注释与标识符**英文**（注释已全中文化，新增注释请用中文保持一致）；UI 文案中文经 `U8()`；日志模块名与文件名对应。

---

## 13. Git 状态与发布流程

- 当前：**19 个提交，纯本地，未配置远端**。
- `.gitignore` 已覆盖：`build*/`、`out/`、`.vs/`、`*.user`、`vcpkg_installed/`、`*.pdb`、`*.ilk`、`*.obj`、`*.log`。
- 发布一个版本：改 `src/app/AboutInfo.h`（版本/日期/仓库地址）+ `src/app/app.manifest` + `src/app/app.rc` 的 `FILEVERSION` → 双击 `一键编译.bat` → 验证自测全绿。

```bat
:: 首次推送到 GitHub（用户手动执行）
git remote add origin https://github.com/<你的账号>/<仓库名>.git
git push -u origin main
```

---

## 14. 用户待办（转接时未完成）

1. **填写仓库地址**：`src/app/AboutInfo.h` 的 `kRepoUrl`（当前为空 → 关于页隐藏该行）。版本号 1.0.0、日期 2026-09-21 已填。
2. **Npcap 可选安装**：从 https://npcap.com/#download 下载安装（默认勾选含回环），网络页「深度抓包」区点「重新检测」。
3. 推送仓库到 GitHub（命令见 §13）。

---

## 15. 给新 agent 的起步建议

1. 先跑通三条命令（§2）确认环境，再读 `README.md` 了解产品形态。
2. 读 `docs/phase/01_架构设计文档.md`（含数据契约、线程模型、提权协议、破坏性操作协议）——这是最浓缩的设计说明。
3. 要改某功能前，先在 `docs/phase/` grep 该模块的历史评审（例如改图表先读 `08_chart_design.md` + `13_review_V27.md`），**很多坑已经踩过并记录了修法**。
4. 新增功能：先加纯函数 + selftest 用例，再接 UI；契约变更走 §12.3。
5. 任何涉及进程的代码，先确认 §3.5.1 的身份规则。

# SuperTaskMgr（Windows 超级任务管理器）

面向系统管理员的本地进程/系统监控与维护工具。参考 Process Explorer / Process Hacker 的能力集，使用纯 Win32 + Dear ImGui + D3D11 + ImPlot 自绘 UI 与纯 C++ 采集层，构建产物为**单文件、零第三方 DLL、静态 CRT** 的便携 exe（v1.0.0 实测 **2,914,816 字节 ≈ 2.9 MB**）。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Platform: Windows 10/11 x64](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6.svg)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)
![Single file](https://img.shields.io/badge/Single--file-~2.9%20MB-brightgreen.svg)
![Tests](https://img.shields.io/badge/selftest-176%20passing-brightgreen.svg)

> **徽章说明**：以上均为静态徽章（许可证 / 平台 / 语言标准 / 体积 / 自测项数），**不表示任何 CI 服务状态**——本项目未配置 CI 服务，所有验证均在本地以脚本执行（详见[测试与质量](#测试与质量)）。

> **定位声明**：本工具面向管理员自用。UI 为 ImGui 自绘（非原生控件观感，无屏幕阅读器/无障碍支持）；所有"拿不到的数据"如实显示 `—` 或"需提权/需驱动"，**绝不显示假数据**。

---

## 截图

<!--
建议在此处放置截图（当前仓库未包含运行时截图，请按下表补拍后再替换本注释块）。
建议截图清单（均为深色主题，1280×800 窗口）：
  1. 进程页：全量进程表 + 工具条（暂停采集/间隔/提权重启/清理待机缓存/内存加速/主题/日志/关于）+ 状态栏自省信息。
  2. 性能页：CPU/每核/内存/磁盘/网络/GPU 多块实时曲线 + 时间窗选择 + 告警阈值线。
  3. 深色主题外观（默认主题）；如需对比，可再补一张浅色主题与一张自定义壁纸效果。
推荐放于 assets/ 下，命名如 assets/screenshot-processes.png，并按上述顺序用 Markdown 图片语法插入。
-->

| 进程页（建议截图） | 性能页（建议截图） |
|---|---|
| _待补_ `assets/screenshot-processes.png` | _待补_ `assets/screenshot-performance.png` |

---

## 特性

### 进程监控

- **全量进程表**：13 列（CPU / 私有工作集 / 提交 / 磁盘 / 网络 / 硬故障 / 句柄 / 线程 / 上下文切换 / 徽标 / 描述等），1 秒级刷新、排序、过滤、列拖动重排与列宽持久化。
- **平铺 / 树形视图切换**：树形按父子链展开（父已退出的孤儿标注"父已退出"）。
- **区分系统进程三态**（关闭 / 高亮 / 只看用户进程）：关键进程、Windows 系统进程、服务宿主、UWP、用户应用分类着色 + 图例。
- **详情面板**：路径 / 数字签名 / 命令行 / 用户名 / GDI+USER 对象 / 父进程跳转，以及**已加载模块列表**（点击任一模块即时校验其签名）。

### 进程控制

- 挂起 / 恢复、优先级六档（Realtime 档红字警示）、CPU 亲和性逐核设置。
- 全部为破坏性操作：二次确认 + 执行时身份重验 + 保护名单硬拒。

### 终止与内存

- **终止**：单进程 / 进程树（两段式确认：先给出"预计 N 个"，执行时重新快照、逐成员重验身份、保护成员跳过并如实报告）。
- 自动按需启用 `SeDebugPrivilege`，**用后即释**；服务宿主进程终止前红字警告。
- **内存清理**：释放进程工作集 / 清理系统待机缓存（诚实文案：只清系统缓存，不回收进程内存；伴随缺页代价，性能页可见 Hard Faults 抖动）。
- **一键内存优化**：Top10 高占用勾选批量释放工作集 + 可选清待机缓存（保护名单/系统进程不可选）。

### 性能与网络

- **性能**：CPU（总+每核）/ 内存 / 磁盘 / 网络 / 硬故障 / 上下文切换 / GPU 等多块 120s–600s 实时曲线；时间窗 60/120/300/600 秒可切（切换不清历史）；每核热图；拖拽告警阈值线；CSV 记录导出（captures 目录）；全局热键 `Ctrl+Alt+M` 呼出/隐藏（默认关）。
- **网络**：TCP/UDP 连接表（本地/远程/状态/PID/进程名）；适配器信息卡（以太网/Wi-Fi、IPv4/IPv6/网关/DNS/MAC/链路速度/DHCP、一键复制 IP）；实时监视（连接事件流 + 过滤 + Top 远程目标 + DNS 记录 + CSV）。
- **可选 ETW 按进程流量统计**（需管理员，仅本机统计、不上传）；**可选 Npcap 深度抓包**（含载荷 hex/ASCII 视图，需自行安装，见[使用说明](#使用说明)）。
- 跨页联动（连接/服务 ↔ 进程跳转、服务宿主列出同 PID 服务）。

### 系统管理

- **启动项**：注册表 Run/Run32、启动文件夹、计划任务（登录/启动触发）、UWP StartupTask 四源枚举与启用/禁用；**写前自动备份**（时间戳保留历史，计划任务同时备份 XML，可逆向回滚）。
- **服务与驱动**：服务枚举 / 启动 / 停止（依赖树警告）、驱动列表与按需签名校验（Windows 11 24H2+ 需提权）。
- **崩溃记录**：事件日志中的应用错误(1000) / 挂起(1002) / WER 报告(1001) 最近 200 条，免管理员。
- **窗口管理**：顶层窗口前置 / 置顶 / 最小化 / 温和关闭（关闭需确认，执行前复核句柄有效性与 PID）。

### 硬件传感器

- CPU 每核频率与占用率、**ACPI 热区逐实例温度**、SMART 磁盘健康细项（NVMe 备件%/磨损%/通电时长、SATA 温度）、GPU 逐传感器展开（温度/功耗/利用率/显存，NVML 存在时）、每适配器网络吞吐、电池、内存/分页池、风扇。
- 各分组**可勾选显示** + **详细模式**（逐条展示全部读数实例）。
- **可选内核增强（全 opt-in）**：安装 [PawnIO](https://pawnio.eu) 官方签名运行时并把官方模块放入 `%LOCALAPPDATA%\SuperTaskMgr\modules\` 后，自动启用 **CPU 每核 DTS 温度、主板风扇转速、电压**（模块哈希白名单校验，只读，不篡改风扇策略）。
- **可选 LibreHardwareMonitor 数据源**（默认关）：自行运行 LHM 并启用其 Web 服务器，本工具仅读本机 `127.0.0.1`。

### 外观与其他

- 主题三态（深色 / 浅色 / 跟随系统，实时响应系统切换）；**自定义壁纸**（PNG/JPG/BMP，可调遮罩浓度，选项旁常驻性能开销提醒）。
- 「关于」对话框（版本 / 发布时间 / 仓库链接 / 运行环境）；日志查看器（过滤 / 诊断报告生成 / 打开目录）；兼容模式可点击诊断（逐项测量值 + 重试 + 报告导出）。
- 托盘图标、阈值告警（CPU/内存，托盘气泡，默认关）、标题栏 X 行为三选（退出 / 最小化到托盘 / 每次询问，可记住选择）、单实例、提权重启时会话状态交接（页签 / 选中进程 / 排序 / 窗口矩形）。

---

## 快速开始

### 路径 A：下载即用（推荐普通用户）

1. 从 [Releases](https://github.com/codejinxz/SuperTaskMgr/releases) 下载 `SuperTaskMgr-v1.0.0-win64.zip`。
2. 解压到任意目录（**无需安装**，绿色便携）。
3. 双击 `SuperTaskMgr.exe` 运行（默认普通权限）。
4. 需要查看他人进程详情、SMART、ETW 流量、服务启停等能力时，点工具条「以管理员身份重启」，在 UAC 中确认（会话状态自动保留）。

> 解压包的 SHA256 校验值与使用说明见包内 `SHA256SUMS.txt` 与 `使用说明.txt`。

### 路径 B：从源码构建

环境要求：**Windows 10 21H2+ / Windows 11 x64**；**VS2022 Build Tools**（含 MSVC v143 与 CMake 组件）。无其他依赖（imgui/implot/stb_image 已 vendoring，见 `third_party/SHA256SUMS.txt`）。

```bat
:: 一键（编译 Release + 跑自测 + 询问是否启动）
一键编译.bat

:: 或分步：官方构建入口（默认 Release，可传 Debug）
scripts\build.bat            :: Release（默认）
scripts\build.bat Debug      :: Debug

:: 产物
build\Release\SuperTaskMgr.exe        :: 主程序（单文件便携）
build\Release\stm_selftest.exe        :: 自测程序（176 项，--json 输出机器可读结果）
```

构建脚本**不依赖 `vcvarsall.bat`**（改用 VS 生成器自定位工具链），可从 Git Bash 等任意 shell 调用。详见[构建指南](#构建指南)。

---

## 使用说明

### 提权

- 默认以**普通权限**启动。需要提权的功能在界面上有明确徽标；一键「以管理员身份重启」经 UAC 授权，重启前把会话状态（页签 / 选中进程 / 排序 / 窗口矩形 / 刷新间隔）写入 `session.json`，新实例恢复。
- `SeDebugPrivilege` 仅在提权实例中按需启用、**用后即释**，每次启用写日志（可审计）。

### 页签一览

进程 / 性能 / 网络 / 启动项 / 服务 / 驱动 / 崩溃记录 / 窗口 / 传感器。

### 可选增强的安装（均默认关闭，本程序不捆绑任何驱动）

| 能力 | 需要安装 | 安装要点 |
|---|---|---|
| 深度抓包（含载荷） | [Npcap](https://npcap.com/#download) | 官方签名驱动；安装时保持默认选项（含回环抓包）；装后到网络页「深度抓包」区点「重新检测」。**免费版不支持静默安装，需手动双击安装包**；其许可禁止外部再分发。 |
| CPU 每核 DTS 温度 / 主板风扇转速 / 电压 | [PawnIO](https://pawnio.eu) | 官方签名运行时（HVCI/Secure Boot 兼容）；把官方模块放入 `%LOCALAPPDATA%\SuperTaskMgr\modules\`。未安装时传感器页如实显示"需要驱动支持"。 |
| 更多传感器读数 | LibreHardwareMonitor | 自行运行 LHM 并开启其 Web 服务器；本工具仅读本机 `127.0.0.1`。 |

### 权限与隐私（重要）

- 本工具**不联网上传任何数据**、无监听端口、不写自启动、不留常驻服务；ETW 网络统计与网络监视均为显式开关（默认关）、会话名唯一、正常退出即清理。
  - **例外（已知限制）**：进程崩溃时，管理员权限下已开启的 ETW 会话可能残留约 4 MB 非分页池，直至系统重启或下次同名实例启动时清理。
- 破坏性操作（杀进程/树、释放工作集、清待机缓存、禁用启动项、停服务）全部**二次确认**；系统关键进程（csrss / lsass / wininit 等）受**双层保护名单**硬拒绝。
- 未签名 exe：SmartScreen / 杀软可能提示"未知发布者"——本工具以本地构建分发，不申请代码签名。所有操作写本地日志（`%LOCALAPPDATA%\SuperTaskMgr\logs`，可审计；日志**不记录命令行/窗口标题**）。

---

## 构建指南

### 依赖

| 依赖 | 版本 | 用途 | 分发方式 |
|---|---|---|---|
| Visual Studio 2022 Build Tools | v143 工具集 + CMake 组件 | 编译（`/std:c++20 /W4 /utf-8 /MT`） | 用户自备 |
| Dear ImGui | v1.92.9（vendored） | UI 框架 | 已入库 `third_party/imgui/` |
| ImPlot | v1.0（vendored） | 实时图表 | 已入库 `third_party/implot/` |
| stb_image.h | v2.30（vendored） | 壁纸加载 | 已入库 `third_party/stb/` |

CMake 依赖：≥ 3.20；MSVC 工具集必需（`CMakeLists.txt` 对非 MSVC 直接 `FATAL_ERROR`）。vendoring 归档哈希记录于 `third_party/SHA256SUMS.txt`。

### 命令

```bat
:: 生成 build\ 并编译（构建脚本硬编码 VS 自带 cmake 路径 + vswhere 兜底）
scripts\build.bat Release

:: 手工 CMake（Git Bash 下需 MSYS_NO_PATHCONV=1）
MSYS_NO_PATHCONV=1 cmake -S . -B build_x -A x64
MSYS_NO_PATHCONV=1 cmake --build build_x --config Release -- /m

:: 换目录构建（避免运行中的实例占用 build\ 目录）
cmake -S . -B build_x -A x64
```

五个 CMake 目标：`stm_core`（契约+工具）、`stm_collect`（采集）、`stm_ops`（操作层）三个静态库，加 `SuperTaskMgr`（GUI 子系统）与 `stm_selftest`（控制台）。

### 验证方式

```bat
:: 自测（176 项，退出码 = 失败数；--json 输出机器可读结果）
build\Release\stm_selftest.exe

:: GUI 无交互冒烟（离屏渲染 N 帧，覆盖全部页签）
build\Release\SuperTaskMgr.exe --smoke 150

:: 真实 ImGui 输入管线注入回归（六条，均 exit 0 为通过）
build\Release\SuperTaskMgr.exe --autotest dialogclick
build\Release\SuperTaskMgr.exe --autotest kill
build\Release\SuperTaskMgr.exe --autotest tree
build\Release\SuperTaskMgr.exe --autotest startup
build\Release\SuperTaskMgr.exe --autotest about
build\Release\SuperTaskMgr.exe --autotest wallpaper
```

### 故障排查

| 现象 | 原因与处理 |
|---|---|
| `--smoke` / `--autotest` 全部失败、exit 1 | 已有 SuperTaskMgr 实例在运行，持有单实例互斥体，headless 启动按设计直接 `return 1`（避免 CI 被弹窗阻塞）。**关闭所有实例后重跑**。 |
| 无法删除/覆盖 `build\` 下的 exe | 有提权实例正在运行（普通权限 shell 杀不掉），先关闭应用；或换目录构建（`-B build_x`）。 |
| 外部工具读不到 `logs/stm.log` | 应用运行期间独占该日志文件，先退出应用再读。 |
| Git Bash 里 cmake 参数异常 | 加 `MSYS_NO_PATHCONV=1` 前缀（否则 `/m` 等参数被路径转换破坏）。 |
| CMake 找不到 | 构建脚本按硬编码路径找 VS 自带 CMake，失败时用 `vswhere` 兜底；仍失败说明未装 VS2022 的 CMake 组件。 |
| 取不到 GUI 子系统 exe 退出码 | 建议用 PowerShell（`$LASTEXITCODE`）而非部分 shell 的 `$?`。 |

### 发布一个新版本

编辑 `src/app/AboutInfo.h` 顶部三个常量后重新构建即可：

- `kAppVersion`：版本号（显示在"关于"对话框与窗口标题）；
- `kBuildDate`：发布时间（如 `L"2026-09-23"`，留空则不显示该行）；
- `kRepoUrl`：GitHub 仓库地址（留空则隐藏链接行）。

建议同步更新 `src/app/app.manifest` 的 `assemblyIdentity version` 与 `src/app/app.rc` 的 `FILEVERSION`。

---

## 架构概览

四层单向依赖，采集与操作互不依赖：

```text
src/app (UI 壳 + 各页)  ──>  src/ops (破坏性操作/提权/签名/服务/启动项/驱动/崩溃)
        │                          │
        └──────>  src/collect (快照采集)  ──┐
                        │                  ├──>  src/core (数据契约 + 平台工具)
                        └──────────────────┘
```

| 层 | 目标 | 职责 |
|---|---|---|
| `src/core` | `stm_core` | 数据契约（`ProcData.h`）、日志/字符串/错误/配置/队列/通知/特权/保护名单 |
| `src/collect` | `stm_collect` | 进程/系统/GPU 快照采集、网络表、传感器、自校验门 |
| `src/ops` | `stm_ops` | 终止/控制/内存、提权、签名、服务/启动项/驱动/崩溃查询 |
| `src/app` | `SuperTaskMgr` | Win32 窗口 + D3D11 + ImGui 壳与各页（`ui/` 进程与性能页、`ui3/` 其余页） |
| `src/selftest` | `stm_selftest` | 176 项控制台自测 |

**线程模型**：UI 线程（1，帧预算 ≤3ms）/ 采集线程（1，tick p50 ≤15ms）/ 操作线程（1，串行 JobQueue）/ 网络监视线程（1）/ ETW 消费线程（0–3，仅管理员）/ Npcap 消费线程（0–1，仅管理员）。同步原语仅四种：`SnapshotStore`、`JobQueue`、`NotificationQueue`、各采集器内部互斥；跨线程无裸指针。

**数据流**：采集线程 → 不可变 `Snapshot`（`shared_ptr`）→ `SnapshotStore` → UI 每帧 `Get()` 一次只读消费；UI 操作 → `JobQueue` → ops 执行 → `NotificationQueue` → UI toast / 状态栏。

完整的模块、线程、契约、数据流、关键决策与扩展点见 **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**；历史设计文档见 [docs/phase/01_架构设计文档.md](docs/phase/01_架构设计文档.md)。

---

## 测试与质量

| 层 | 命令 | 覆盖 | 判据 |
|---|---|---|---|
| 自测 | `stm_selftest.exe` | **176 项**：契约 / 纯函数 / 真实 API 行为 / 权限降级 | 全绿（`--json` 可机器读） |
| 冒烟 | `SuperTaskMgr.exe --smoke 150` | 全部页签离屏渲染 150 帧 | exit 0 |
| 输入注入 | `--autotest {dialogclick,kill,tree,startup,about,wallpaper}` | **六条**：走真实 ImGui 输入管线，注入鼠标点击确认按钮 / 关于按钮，断言动作真的生效 | 全 exit 0 + `autotest_result.log` 全 PASS |
| 构建 | `scripts\build.bat Release` 与 `Debug` | `/W4` 全仓 | **0 错误 0 警告** |

- **纯函数优先**：所有可测逻辑（排序比较、解析、布局数学、配置键、状态机）抽为 header-only 纯函数并直接断言——这是 176 项用例能覆盖 UI 逻辑的原因。
- `--autotest dialogclick` 内含"模态单帧化"哨兵：把历史缺陷复原会 FAIL，用于防止回归。
- **已知 flaky**：`collect_tick_latency`（tick p50 ≤15ms）对宿主负载极敏感，VM / 高负载下偶发假阴性。判定方法：若 tick 路径源码零改动且基线二进制同期同样失败，即为环境性，可注明后放行。

---

## 已知限制（诚实清单）

- 需要管理员的能力（提权后可用）：他人进程的路径/内存/命令行、SMART、ACPI 热区温度、ETW 流量、服务启停、HKLM 启动项、计划任务系统文件夹、模块列表；驱动列表在 Windows 11 24H2+ 需要提权（系统行为）。
- **每核 DTS 温度 / 风扇 / 电压**需要内核访问：默认不可用；安装 PawnIO 运行时后自动启用。本应用**不内置、不捆绑** WinRing0 类驱动（其安全风险见 [docs/phase/00_技术选型评估报告.md](docs/phase/00_技术选型评估报告.md)）；笔记本风扇通常由 EC 控制，可能仍无读数（如实显示）。
- GPU 温度需要厂商运行时（NVIDIA=NVML；AMD/Intel 暂未接入）。
- StartupApproved / UWP StartupTask 的注册表语义为社区考证+本机实测（非官方文档化），禁用操作有备份可逆兜底，标记为"实验性"（日志 WARN）。
- PDH 部分计数器（如 `\Memory\Hard Faults/sec`）在个别系统缺失，显示 `—`。
- 兼容模式（NtQSI 半文档化结构自检未通过时的 Toolhelp 慢路径）下部分列显示 `—`。
- ImGui 自绘 UI 的 IME / 无障碍限制；中文列排序为码点序。
- 空载内存 ≈ 128 MB（D3D11 运行时 + 驱动 + 字体图集基线，长跑零增长；预算与证据见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)）。
- 定高常量按 DPI 缩放系数缩放，极端 DPI 下可视行数会减少。
- 未签名 exe：SmartScreen / 杀软可能提示"未知发布者"。
- 深度抓包 / 内核温度均需用户自行安装官方签名运行时（许可与安全原因不随包分发驱动）。

---

## 常见问题 FAQ

**Q1. 为什么需要管理员权限？**
枚举进程、读取系统级计数器等基础功能在普通权限下即可用。但查看**他人进程**的路径/内存/命令行、SMART 磁盘健康、ACPI 热区温度、ETW 按进程流量、服务启停、HKLM 启动项与计划任务系统文件夹等，需要提权后的句柄与特权（`SeDebugPrivilege` 等）。本工具采用"默认普通权限 + 需要时一键 `runas` 重启"的模型（1A），不做启动即提权。

**Q2. 为什么某个进程杀不掉？**
两类原因：① 它是系统关键进程，命中**双层保护名单**（UI 禁用 + ops 层硬拒绝），这是设计上的安全防线，不可绕过；② 目标是**提权/受保护进程**而当前是普通权限，会被系统拒绝（access denied）——界面对应功能会给出"需提权"提示，点「以管理员身份重启」后再试。同名伪造进程不会被名单误放行（名单命中要求路径位于 `SystemRoot` 下；路径未知时保守保护）。

**Q3. 为什么某些数据显示"—"？**
这是本工具的**诚实数据**原则：拿不到的数据不编造。常见于三种情况——**需提权**（普通权限读不到他人进程的内存/命令行等）、**需驱动**（每核 DTS 温度/风扇需 PawnIO）、**本机不支持**（PDH 计数器缺失、笔记本 EC 风扇无读数、兼容模式缺列）。状态栏的"兼容模式"锚点可点击查看逐项原因与诊断报告。

**Q4. 会不会弹 SmartScreen / 杀软警告？**
会。exe **未做代码签名**，Windows SmartScreen 与部分杀软可能提示"未知发布者"或"未知应用"。这是本地构建分发的固有现象，不表示程序有恶意行为（内核组件全 opt-in、不联网、不持久化，见 [SECURITY.md](SECURITY.md)）。如需核对完整性，请比对 Releases 中的 SHA256。

**Q5. 要不要安装驱动？**
**不需要**。本应用本体零驱动，全部默认功能（进程/性能/网络表/传感器基础读数等）不含任何驱动。仅当你需要"深度抓包"或"每核 DTS 温度/风扇/电压"这类内核能力时，才需要**你自行安装**对应官方签名运行时（Npcap / PawnIO）；本工具不捆绑、不分发、不自动安装任何驱动，未安装时界面如实标注。

**Q6. 数据会不会上传？有没有联网？**
不会。本工具**不联网上传任何数据**、无监听端口、不写自启动、不留常驻服务。ETW 网络统计与网络监视是本地只读能力且为显式开关（默认关）；LibreHardwareMonitor 数据源仅读本机 `127.0.0.1`。所有操作只写本地日志（`%LOCALAPPDATA%\SuperTaskMgr\logs`），且日志不记录命令行/窗口标题。

**Q7. 怎么反馈 bug / 提功能建议？**
在 GitHub [Issues](https://github.com/codejinxz/SuperTaskMgr/issues) 提交，附上「日志」页一键生成的诊断报告（含系统与版本信息，**不会自动上传**）。安全相关问题请按 [SECURITY.md](SECURITY.md) 的渠道报告。

---

## 贡献指南

欢迎贡献。构建、测试、代码风格（**注释中文 / 标识符英文**）、提交信息约定、契约头冻结纪律与 PR 检查清单见 **[CONTRIBUTING.md](CONTRIBUTING.md)**；安全模型与漏洞报告渠道见 **[SECURITY.md](SECURITY.md)**。

---

## 许可证

本项目代码以 [MIT](LICENSE) 许可证发布。

| 组件 | 许可证 |
|---|---|
| 本项目代码 | MIT |
| Dear ImGui v1.92.9（vendored） | MIT |
| ImPlot v1.0（vendored） | MIT |
| stb_image.h v2.30（vendored，壁纸加载） | 公有领域 / MIT 双许可 |
| 运行时加载的系统中文字体（msyh.ttc 等） | 随 Windows 分发，本工具不复制/不分发 |
| NVML（如存在，运行时动态加载） | NVIDIA 随驱动分发，本工具不分发 |

---

## 致谢

- [Dear ImGui](https://github.com/ocornut/imgui)（Omar Cornut）与 [ImPlot](https://github.com/epezent/implot)（Evan Pezent）——本项目的 UI 与图表基石。
- [stb_image](https://github.com/nothings/stb)（Sean Barrett）——壁纸图片加载。
- [PawnIO](https://pawnio.eu)（namazso）与 [Npcap](https://npcap.com)（Nmap Software LLC）——可选内核能力的官方签名运行时。
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)——可选传感器数据源。
- 设计上参考了 Process Explorer / Process Hacker (System Informer) / HWiNFO 的能力集与交互习惯。

---

## 文档

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — 当前有效的架构说明（分层 / 线程 / 契约 / 数据流 / 关键决策 / 扩展点）。
- **[docs/README.md](docs/README.md)** — 文档索引（导航 `docs/phase/` 的 5 份设计规格、`docs/archive/` 的 46 份过程记录与 `docs/research/` 的 6 份技术调研）。
- **[docs/HANDOVER.md](docs/HANDOVER.md)** — 面向新接手开发者/AI agent 的交接文档（索引 + 增量信息）。
- **[CHANGELOG.md](CHANGELOG.md)** — 版本变更记录（Keep a Changelog 格式）。

## redist/ 目录说明

`redist/` 为可选的第三方安装包暂存目录（该目录已被 `.gitignore` 排除，不随仓库分发）。例如官方 Npcap 安装包 `npcap-1.89.exe`（Nmap Software LLC 签名，已验证）供「网络页 → 深度抓包」功能使用——本应用**不捆绑、不自动安装**，需你手动运行安装。不使用抓包功能可删除该文件。

# 架构说明（当前有效）

> 本文档描述 SuperTaskMgr v1.0.0 **当前有效**的架构——去除了开发过程中的历史包袱（探路、废弃方案、阶段性遗留），只写当前状态与理由。历史设计与每轮决策过程见 [phase/01_架构设计文档.md](phase/01_架构设计文档.md) 与 [phase/](phase/) 下的阶段/维护/评审报告。

## 1. 定位与硬约束

- **产品**：Windows 超级任务管理器（codename `SuperTaskMgr`），面向系统管理员的**本地自用**工具。
- **平台**：Windows 10 21H2+ / Windows 11 **x64**（仅 x64）。
- **技术栈**：纯 Win32 + Dear ImGui v1.92.9 + D3D11 + ImPlot v1.0 + 纯 C++ 采集层；C++20；MSVC `/W4 /permissive- /utf-8 /MT`；CMake ≥ 3.20。
- **分发形态**：单文件、零第三方 DLL、静态 CRT 的便携 exe（v1.0.0 实测 2,914,816 字节）。

**六条贯穿全部代码的硬约束**（改代码时不得违背）：

1. **诚实数据**：拿不到就显示 `—` / "需提权" / "需驱动"，绝不显示 0 或假值。
2. **零持久化 / 零隐蔽 / 零联网上传**：不写自启动、不驻留服务、无监听端口、不上传数据。
3. **破坏性操作双层防线**：UI 二次确认 + ops 层保护名单硬拒绝。
4. **内核组件全 opt-in**：本体零驱动；可选增强依赖用户自行安装的官方签名运行时。
5. **文档化 API 优先**：使用半文档化结构（如 `NtQuerySystemInformation`）必须自校验并提供降级路径。
6. **全 Unicode**：一律宽字符 API，中文路径与进程名全程正确。

> 性能与资源预算（tick 延迟、UI 帧耗时、内存、启动时间）见 §9。

## 2. 分层与依赖方向

```text
        app  (UI 壳 + 各页)
        │  │
        │  └──────────────> collect  (快照采集)
        │                        │
        └──> ops ────────────────┤
             (破坏性操作/提权)     │
                 │                │
                 └────> core <────┘
                  (数据契约 + 平台工具)
```

- 依赖方向固定：`app → ops → core`、`app → collect → core`。
- `ops` 与 `collect` **互不依赖**（禁的是库依赖；ops 内部允许直调 OS API 做最小重枚举，见 §5）。
- `collect` / `ops` **禁止 include ImGui 或任何 UI 头**（静态库边界强制）。
- 五个 CMake 目标：`stm_core`、`stm_collect`、`stm_ops` 三个静态库 + `SuperTaskMgr`（WIN32 子系统）+ `stm_selftest`（控制台）。

| 层 | 目标 | 职责 | 关键文件 |
|---|---|---|---|
| `src/core` | `stm_core` | 数据契约、日志、字符串、错误、配置、队列、通知、文件工具、特权、保护名单 | `ProcData.h` `Log.*` `Str.*` `Err.*` `Cfg.*` `Jobs.*` `Notifications.*` `FsUtil.*` `Privilege.*` `ProtectedList.*` `HandleGuard.h` `LogFile.h` |
| `src/collect` | `stm_collect` | 进程/系统/GPU 快照采集、自校验门、网络表与监视、传感器、可选内核/抓包源 | `CollectService.*` `ProcessCollector.*` `SystemCollector.*` `GpuCollector.*` `SelfCheckGate.*` `NetTables.*` `NetMonitor.*` `AdapterInfo.*` `Sensors.*` `LhmSource.*` `PawnIoLink.*` `NpcapSource.*` `NpcapParse.h` `KernelProbe.*` |
| `src/ops` | `stm_ops` | 终止/控制/内存、提权、签名、服务/启动项/驱动/崩溃查询、单实例、会话状态、详情提供 | `ProcessOps.*` `ProcessControl.*` `Signature.*` `ServiceOps.*` `StartupOps.*` `DriverOps.*` `CrashLog.*` `Elevate.*` `SingleInstance.*` `SessionState.*` `DetailsProvider.*` |
| `src/app` | `SuperTaskMgr` | Win32 窗口 + D3D11 渲染 + ImGui 壳 + 各页（`ui/` 进程与性能页、`ui3/` 其余页） | `main.cpp` `AppContext.h` `Win32Window.*` `D3DRenderer.*` `ImGuiLayer.*` `Theme.*` `AboutInfo.h` `AutotestDialog.*` `ui/Pages.cpp` `ui3/Pages3.cpp` `ui3/GcPages.*` |
| `src/selftest` | `stm_selftest` | 176 项控制台自测（框架 `TestFramework.h` 的 `STM_TEST` 宏） | `*_test.cpp` |

## 3. 线程模型

| 线程 | 数量 | 职责 | 约束 |
|---|---|---|---|
| UI | 1 | 消息循环、ImGui 帧、模态、通知 drain | 空闲帧 ≤3 ms；禁阻塞调用 |
| 采集 | 1 | 周期快照写入 `SnapshotStore` | tick p50 ≤15 ms @500 进程 |
| 操作 | 1 | `JobQueue` 串行执行慢/破坏性操作 | 禁止 UI API；COM 走 `CoInitializeEx(STA)` 且仅在本线程 |
| 网络监视 | 1 | `NetMonitor` 1 秒差分连接表 | 免管理员 |
| ETW 消费 | 0–3 | Kernel-Network / DNS-Client 各自的 `ProcessTrace` 线程 | 仅管理员；RAII 清理 |
| Npcap 消费 | 0–1 | `pcap_next_ex` 抓包解析 | 仅管理员 |

**同步原语仅四种**：

- `SnapshotStore`（`mutex` + `shared_ptr<const Snapshot>`，单写多读，构造即置空快照，`Get()` 永不返回 null）；
- `JobQueue`（`mutex` + `condition_variable`；退出协议：在途破坏性任务最多等 2000 ms，未开始的丢弃并记日志）；
- `NotificationQueue`（UI 帧内 drain）；
- 各采集器内部互斥。

**跨线程无裸指针**：job lambda 一律按值捕获 `shared_ptr<AppContext>`。

## 4. 数据契约

核心契约在 `src/core/ProcData.h`（**改动需极谨慎，牵动全仓**）：

```cpp
struct ProcKey { uint32_t pid = 0; uint64_t createTime = 0; };  // 身份 = (PID, 创建时刻)
inline constexpr double   kUnavail    = std::numeric_limits<double>::quiet_NaN();  // UI 渲染 "—"
inline constexpr uint64_t kUnavailU64 = std::numeric_limits<uint64_t>::max();

enum ProcFlag : uint32_t {
    PF_Elevated    = 1u << 0,  PF_Uwp      = 1u << 1,  PF_Wow64   = 1u << 2,
    PF_ServiceHost = 1u << 3,  PF_HasWindow = 1u << 4, PF_Protected = 1u << 5,
    PF_Suspended   = 1u << 6,  PF_AccessDenied = 1u << 7,   // AccessDenied 为粘滞标志
};

struct ProcInfo {
    ProcKey key; uint32_t parentPid = 0, sessionId = 0;
    std::wstring name, path;                 // path 无权限时为空（诚实空），非假路径
    uint64_t kernelTime = 0, userTime = 0;   // 累计 100ns（NtQSI，自校验门覆盖）
    double cpuPercent = kUnavail;            // Δ(kernel+user)/(tickSec×核数)×100，全核归一
    uint64_t workingSet = 0;                 // NtQSI WorkingSetSize
    uint64_t privateWorkingSet = kUnavailU64;// 字节（PDH Working Set - Private）
    uint64_t commitBytes = 0;                // PrivateUsage（任务管理器"提交大小"口径）
    uint64_t ioReadBytes = 0, ioWriteBytes = 0;
    double diskBytesPerSec = kUnavail;       // 口径 = 文件+网络+设备总量（UI 已标注）
    double netBytesPerSec = kUnavail;        // 仅 CAP_NET_ETW 时有效
    double pageFaultsPerSec = kUnavail, contextSwitchesPerSec = kUnavail;
    uint32_t handles = 0, threads = 0, gdiObjects = 0, userObjects = 0;
    uint32_t flags = 0; std::wstring windowTitle;  // 每 5 tick 刷新一次
};

struct SystemInfo { /* cpuTotalPercent / perCorePercent / 物理与提交内存 / 内核池 /
                      磁盘·网络速率 / hardFaultsPerSec / diskQueueDepth / gpus / netAdapters / uptimeSec */ };
enum SnapshotCap : uint32_t { CAP_NET_ETW = 1u << 0 };

struct Snapshot {
    uint64_t tickId = 0; double tickSec = 0; int64_t timestamp = 0;
    bool degraded = false; std::wstring degradeReason;    // 兼容模式时 UI 显示
    uint32_t caps = 0;
    std::vector<ProcInfo> procs;                          // 按 pid 升序
    std::vector<GpuProcUsage> gpuProcs;                   // 独立于 procs，按 (luid, pid)
    SystemInfo sys;
};

class SnapshotStore { void Set(std::shared_ptr<const Snapshot>); std::shared_ptr<const Snapshot> Get() const; };
```

其他关键契约头见 §6。

## 5. 破坏性操作执行协议（安全关键）

`src/ops/ProcessOps.h` 头部以注释强制下列协议，**每个实现者都必须照做**：

1. **身份复核**：`OpenProcess` → `GetProcessTimes` → 比对 `createTime`（±1s）。不匹配即报告"已退出或 PID 复用"并拒绝执行。**绝不只信任 PID。**
2. **保护名单是 ops 内的硬闸门**：UI 确认只是第一道闸门。
3. **树操作**：执行时重新快照；逐成员做保护检查，受保护成员**跳过并上报**（绝不静默丢弃）。

两段式树杀接口：

```cpp
bool PlanTerminateTree(const ProcKey& root, std::vector<ProcKey>* out, std::wstring* err);  // 纯遍历，无副作用
struct TreeResult { int planned, terminated, failed, skippedProtected; std::wstring firstError; };
bool TerminateTree(const ProcKey& root, TreeResult* out, std::wstring* err);
```

UI 先用 `PlanTerminateTree` 显示"预计 N 个（执行时可能变化）"，确认后再 `TerminateTree`（执行时重新快照、leaf-first、逐个重验）。

**保护名单**（`core/ProtectedList.h`）实现要点：

- PID 0 / 4 + 关键映像名（csrss / lsass / wininit / winlogon / services / smss / Registry / Memory Compression / dwm 等）。
- 名单命中要求路径位于 `SystemRoot` 下——**防同名伪造进程绕过**；路径未知时**保守保护**。
- 挂起关键进程比杀死更危险（挂起 csrss 会冻结系统），故挂起/恢复/优先级/亲和性/trim 全部走同一硬门。

## 6. 关键契约头（冻结）

| 契约 | 内容 |
|---|---|
| `core/ProcData.h` | `ProcKey` / `ProcInfo` / `SystemInfo` / `Snapshot` / `SnapshotStore` / `kUnavail` 约定 |
| `core/LogFile.h` | 日志尾读与解析、级别过滤、上报格式（header-only 纯逻辑） |
| `core/Jobs.h` / `core/Notifications.h` | `JobQueue` 与 `NotificationQueue` 的队列契约 |
| `core/ProtectedList.h` | 保护名单判定 `ProtectedReason(pid, name, path)` |
| `collect/CollectService.h` | 采集服务接口 + `SelfCheckItem` / 自检重试代际 |
| `collect/NetMonitor.h` | 连接事件 / 远程聚合 / DNS 三类接口 |
| `collect/AdapterInfo.h` / `Sensors.h` | 适配器信息 / 传感器四态快照（`Sensors.h` 允许纯增量扩展） |
| `ops/ProcessOps.h` | **含强制执行协议注释**（见 §5） |
| `ops/ProcessControl.h` | 挂起/恢复/优先级/亲和性（四操作全过保护名单） |
| `ops/{ServiceOps,StartupOps,DriverOps,CrashLog}.h` | 服务 / 启动项 / 驱动 / 崩溃记录（启动项写前强制备份） |
| `app/AboutInfo.h` | **发布信息单一来源**（版本 / 日期 / 仓库地址） |

**契约纪律**：契约头由架构师写入并冻结；开发只实现 `.cpp`；确需变更时在当轮报告登记并同步更新相关 selftest 断言。

## 7. 数据流

```text
采集线程 → Snapshot（不可变 shared_ptr）→ SnapshotStore
UI 每帧：Store().Get() 一次 → 各页只读消费
UI 操作 → JobQueue → ops 执行 → NotificationQueue → UI toast / 状态栏
```

- 采集走 `NtQuerySystemInformation(SystemProcessInformation)` 半文档化结构一次取全，**启动时经自校验门交叉验证**（`GetProcessTimes` / `GetProcessMemoryInfo` / `GetProcessIoCounters` / `GetProcessHandleCount` 共 6 项），连续 2 轮全过才启用；失败重试 2 次后降级到 Toolhelp + PSAPI 慢路径（"兼容模式"，缺失列显示 `—`）。
- 选中进程的富信息（签名 / 命令行 / 用户 / GDI+USER / 模块）按需经 `JobQueue` 拉取并缓存（缓存键 `path+size+mtime`）。

## 8. 关键设计决策与理由

1. **进程身份 = `(PID, CreateTime)` 二元组**：PID 会被复用，所有跨帧引用、终止前校验、树杀枚举都必须比对 `createTime`（±1s 容差）。这是全项目最常被违背的规则，改任何涉及进程的代码前先确认。
2. **半文档化结构 + 启动自检门 + 慢路径降级**：`SystemProcessInformation` 字段偏移未文档化，故启动时与文档化 API 交叉比对，失败自动降级而非崩溃或显示假数据。
3. **提权模型 1A**：`asInvoker` 启动 → 需提权的功能带徽标 → 一键 `ShellExecuteExW(runas)` 重启自身；重启前把会话状态原子写入 `session.json`（页签 / 选中 `ProcKey` / 排序 / 窗口矩形 / 间隔），旧实例退出释放命名互斥体，新实例 `CreateMutex` 等待后恢复。`ERROR_CANCELLED` 静默回退。
4. **PDH 一律用 `PdhAddEnglishCounterW`**：中文系统计数器名会本地化，英文路径是唯一稳的写法。
5. **图表 x 轴线性化**：ImPlot 的 `spec.Offset` 按"绘制点数"取模而非环容量，环形缓冲必须先经 `CopyRingTail` 线性化并以 `Offset=0` 绘制；x 轴用 `tickSec` 换算真实秒。（历史 P0 教训）
6. **布局防抖三件套**：状态栏用定宽槽位（`StatusLayout.h`）、页面区块用定高滚动区（`PageLayout.h`）、列宽用配置键持久化 + 布局重置代际（`LayoutResetGeneration`）。
7. **ImGui 模态必须每帧 `BeginPopupModal`**：只在请求帧渲染会变成"僵尸模态"（吞点击）；`SetKeyboardFocusHere` 只能在 `IsWindowAppearing()` 时调一次（每帧调会偷走鼠标按住按钮的 ActiveId）。（历史用户报告缺陷）
8. **纯函数优先**：可测逻辑抽为 header-only 纯函数供 selftest 直接断言——这是 176 项用例能覆盖 UI 逻辑的原因。

## 9. 性能与资源预算

| 项 | 预算 | 说明 |
|---|---|---|
| 采集 tick（NtQSI + PSAPI 路径） | p50 ≤15 ms @500 进程 | 实测 p50 ≈11–12 ms |
| GPU / PDH 通配符查询 | 独立 2 s 节奏；p95 >10 ms 自动降 4 s 并告警 | 运行时自省 + 状态栏 |
| UI 空闲帧 | ≤3 ms；最小化时采集降频至 2 s | 帧计时 |
| 空载工作集 | ≤150 MB 且无增长趋势（实测 ≈128 MB：D3D11 运行时/驱动 + 动态字体图集 + 堆） | 泄漏门禁 = 长期运行增长 <5 MB |
| 启动到主窗 | <1.3 s（实测 1.26 s，含 30 帧 vsync） | 日志时间戳 |
| 日志 | 滚动 ≤3×1 MB | |
| 数据目录 | `%LOCALAPPDATA%\SuperTaskMgr\`（`logs\stm.log`、`config.json`、`session.json`、`captures\`、`modules\`） | |

## 10. 扩展点（想加功能看这里）

| 想做的事 | 扩展点 | 注意 |
|---|---|---|
| 加一个页签 | `app/ui/IPage.h` 页签注册契约；实现放 `ui/` 或 `ui3/` | 页面区块用定高滚动区（`PageLayout.h`）避免顶栏抖动 |
| 加一个破坏性操作 | `ops/ProcessOps.h` 或 `ops/ProcessControl.h` 加函数 | **必须**走 §5 协议；契约头变更需登记 |
| 加一列进程指标 | `collect/ProcessCollector.*` 采数 + `core/ProcData.h` 加字段 + 页面渲染 | `ProcData.h` 改动牵动全仓；无权限时务必置 `kUnavail` |
| 加一个图表块 | `ui3/PerfChart.h`（环形缓冲 + 绘制，注意 §8.5 的 offset 线性化） | 逐块显隐用配置键持久化 |
| 加一个传感器读数 | `collect/Sensors.h`（四态：Ok / NeedAdmin / NeedDriver / NoHardware） | 拿不到就标对应状态，**绝不显示假值** |
| 加一个可测纯逻辑 | 抽 header-only 纯函数放 `ui3/*.h` | 同时补 `src/selftest/` 用例（`STM_TEST`） |
| 改版本 / 发布信息 | `src/app/AboutInfo.h` 三个常量 | 同步 `src/app/app.manifest` 与 `app.rc` 版本 |

新增功能的工作流与验收门见 [HANDOVER.md](HANDOVER.md)；构建与测试命令见 [../README.md](../README.md) 与 [../CONTRIBUTING.md](../CONTRIBUTING.md)。

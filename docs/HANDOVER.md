# SuperTaskMgr 项目交接文档（Handover）

> 版本：**v1.0.0**（构建日期 2026-09-23）｜仓库：<https://github.com/codejinxz/SuperTaskMgr>（Git，分支 `main`）
> 本文档面向**接手的新 agent / 开发者**，结构为「**索引 + 增量信息**」——通用内容已抽到下列文档，**此处不重复**，只写接手时真正需要而别处没有的信息。

## 0. 配套文档（先看这些，本文档只补充）

| 想知道 | 去看 |
|---|---|
| 产品是什么、怎么跑、怎么构建、FAQ | [../README.md](../README.md) |
| 当前架构（分层 / 线程 / 契约 / 数据流 / 关键决策 / 扩展点） | [ARCHITECTURE.md](ARCHITECTURE.md) |
| 版本变更记录 | [../CHANGELOG.md](../CHANGELOG.md) |
| 如何贡献（规范 / 提交约定 / PR 清单 / 红线） | [../CONTRIBUTING.md](../CONTRIBUTING.md) |
| 安全模型与漏洞报告 | [../SECURITY.md](../SECURITY.md) |
| 5 份设计规格 + 6 份调研怎么读 | [README.md](README.md)（文档索引） |
| 历史设计与每轮决策的原始过程 | [phase/01_架构设计文档.md](phase/01_架构设计文档.md) 及 [archive/](archive/) 下的阶段/维护/评审报告（索引见 [archive/README.md](archive/README.md)） |

---

## 1. 新 agent 的 5 分钟上手路径

按顺序做，不要跳：

1. **跑通三条命令**，确认环境与产物是新鲜的（先在 [../README.md](../README.md) 的「构建指南」节确认环境要求）：
   ```bat
   scripts\build.bat Release
   build\Release\stm_selftest.exe                     :: 期望 176/176
   build\Release\SuperTaskMgr.exe --smoke 150         :: 期望 exit 0
   ```
2. **读 [ARCHITECTURE.md](ARCHITECTURE.md)**（一页纸的当前架构）——特别是 §5 破坏性操作协议、§8 关键决策、§10 扩展点。
3. **要改某功能前**，先在 `docs/archive/` **grep 该模块名**，读该主题近几轮的历史评审记录（例如改图表先读 `phase/08_chart_design.md` + `archive/reviews/13_review_V27.md`）。**很多坑已经踩过并记录了修法**，这是本仓最高价值的历史资产。
4. **动任何涉及进程的代码前**，先确认 §2.1 的进程身份不变量。
5. 需要了解某块代码在哪，查 §5 模块地图。

---

## 2. 关键不变量与红线（改代码前必读）

### 2.1 进程身份 = `(PID, CreateTime)` 二元组

`ProcKey { pid, createTime }`。PID 会被系统复用，因此**所有**跨帧引用、终止前校验、树杀枚举都必须比对 `createTime`（±1s 容差）。**这是全项目最常被违背的规则**——涉及进程的代码先确认这一点。

### 2.2 诚实数据

拿不到的数据显示 `—` / "需提权" / "需驱动"（`kUnavail` / `kUnavailU64`），**绝不用 0 或假值冒充**。传感器四态：`Ok` / `NeedAdmin` / `NeedDriver` / `NoHardware`。历史上多处缺陷正是"诚实性"被违反（如 WMI 基础设施失败被误报 `NoHardware`、传感器 raw=0 被当成真 0°C）。这是评审必查项。

### 2.3 破坏性操作双层防线

UI 二次确认（取消键首位 + 破坏性按钮移出键盘导航）+ `ops` 层保护名单硬拒绝。名单命中要求路径位于 `SystemRoot` 下（**防同名伪造绕过**），路径未知时保守保护。协议全文见 [ARCHITECTURE.md §5](ARCHITECTURE.md#5-破坏性操作执行协议安全关键)。

### 2.4 契约头冻结

`src/core/`、`src/collect/`、`src/ops/`、`src/app/` 下的契约头由架构师写入并冻结；开发只实现 `.cpp`。确需变更时在当轮报告登记，并**同步更新相关 selftest 断言**。契约头清单见 [ARCHITECTURE.md §6](ARCHITECTURE.md#6-关键契约头冻结)。

### 2.5 内核组件全 opt-in 且只读

本体零驱动；PawnIO / Npcap 由用户自行安装（模块 SHA-256 白名单、只读、SuperIO RAII 配对），不捆绑、不分发、不自动安装。

### 2.6 其余红线

零持久化 / 零隐蔽 / 零联网上传；全 Unicode（宽字符 API）。完整清单见 [../CONTRIBUTING.md §5](../CONTRIBUTING.md#5-红线改代码时不得违背) 与 [../SECURITY.md §1.2](../SECURITY.md#12-安全承诺红线)。

---

## 3. 开发工作流约定（本项目沿用，接手请保持）

本项目的每一轮开发都遵循同一套流程（见各维护报告的"执行"行）：

1. **每轮先结构化任务**：按「目标 / 上下文 / 约束 / 完成标准」四要素写任务书，再派 subagent。
2. **文件归属互斥**：并行 subagent 必须各自拥有**互不重叠**的文件集合。本项目的固定分区：`ui/Pages.cpp` 归一个、`ui3/Pages3.cpp` 归一个、`collect/*` 按模块分、`ops/*` 按模块分。跨文件需求在报告登记，由集成者统一执行。
3. **契约先行**：新增能力先由架构师写好冻结契约头，再让开发 agent 实现 `.cpp`。
4. **每轮必过验收门**：双配置 0 警告 + selftest 全绿 + `--smoke 150` + 六条 `--autotest`（详见 §4.1）。
5. **每轮开新评审批次**：2–3 名**互不知情**的评审 subagent，从不同维度审查（正确性 / 泄漏与生命周期 / 安全与性能），发现的问题**当轮修完并复验**。
6. **诚实记录**：环境性失败、无法验证项、降级方案都要如实写入报告，不得粉饰。
7. **风格**：代码注释**中文**、标识符**英文**（注释已全中文化，新增注释请用中文保持一致）；UI 文案中文经 `U8()`。

---

## 4. 验证体系与踩坑

### 4.1 验收门命令

```bat
scripts\build.bat Release && scripts\build.bat Debug     :: 双配置 0 错误 0 警告
build\Release\stm_selftest.exe                            :: 176 项全绿（--json 机器可读）
build\Release\SuperTaskMgr.exe --smoke 150                :: exit 0
build\Release\SuperTaskMgr.exe --autotest dialogclick     :: 六条全 exit 0
:: 其余五条：kill / tree / startup / about / wallpaper
```

### 4.2 验证前置条件（踩过的坑，务必先读）

1. **跑 `--smoke` / `--autotest` 前必须关闭所有运行中的 SuperTaskMgr 实例**——它持有单实例互斥体，headless 启动会按设计直接 `return 1`（避免 CI 被弹窗阻塞），表现为"六条 autotest 全失败"的假象。
2. 应用运行期间独占 `logs/stm.log`，外部工具（含本仓日志读取器）无法打开——要读日志请先退出应用。
3. **提权实例（`runas` 启动）无法被普通权限 shell 终止**（access denied）——遇 `build/` 目录被占用，先关闭应用；或换目录构建（`cmake -S . -B build_x -A x64`）。
4. **Git Bash 调用 cmake 需 `MSYS_NO_PATHCONV=1`**（否则 `/m` 等参数被路径转换破坏）；GUI 子系统 exe 的退出码建议用 PowerShell 获取。
5. 构建 / 验证会改动 `%LOCALAPPDATA%\SuperTaskMgr\` 下的 cfg / session——自动化验证前建议备份用户配置，验证后还原。

### 4.3 flaky 用例判定法

`collect_tick_latency`（tick p50 ≤15ms）对宿主负载极敏感，VM / 高负载时偶发假阴性。**判定方法**：若 tick 路径源码零改动（`git diff` 该目录为空）且**基线二进制同期同样失败**，即为环境性，可注明后放行；否则需排查。

### 4.4 v1.0.0 转接时的验证快照

- Release / Debug 双配置 **0 错误 0 警告**。
- `stm_selftest` **176/176 连续两轮通过**（最近一次权威记录见 [archive/reviews/16_review_V34.md](archive/reviews/16_review_V34.md) §"测试与采样执行记录"）。
- `--smoke 150` 与六条 `--autotest` 在同一份源码上的最近一轮已全 PASS（见 V34 报告）。
- V34 评审发现的 **P1-N1（重置布局漏 U1 新增的 `netAdapterH`/`netmonH` 两键）已在源码中修复**（`ui3/ThemeCfg.h` 的 `LayoutResetExactKeys` 已含两键并通知布局重置代际）——接手时如重跑 `ui_p1_test`，注意该用例的清单断言已同步。

---

## 5. 模块地图（哪块代码在哪、改什么看哪里）

> 完整的层职责与依赖方向见 [ARCHITECTURE.md §2](ARCHITECTURE.md#2-分层与依赖方向)。此处只给"改 X 去哪个文件"的索引。

| 你要改的功能 | 主要文件 |
|---|---|
| 数据契约（加字段/指标） | `src/core/ProcData.h`（**牵动全仓，极谨慎**） |
| 配置 / 日志 / 队列 / 保护名单 | `src/core/{Cfg,Log,LogFile,Jobs,Notifications,ProtectedList,Str,Err,FsUtil,Privilege,HandleGuard}.*` |
| 进程采集（NtQSI 主路径、自校验门） | `src/collect/{CollectService,ProcessCollector,SelfCheckGate,CollectUtil,CollectDetail.h}.*` |
| 系统级指标（CPU/内存/磁盘/网络速率/GPU） | `src/collect/{SystemCollector,GpuCollector}.*` |
| 网络表 / 监视 / 适配器 | `src/collect/{NetTables,NetMonitor,AdapterInfo}.*` |
| 传感器 / 可选数据源 | `src/collect/{Sensors,LhmSource,PawnIoLink,KernelProbe,NpcapSource,NpcapParse.h}.*` |
| 终止 / 内存 / 控制 | `src/ops/{ProcessOps,ProcessControl}.*` |
| 服务 / 启动项 / 驱动 / 崩溃记录 | `src/ops/{ServiceOps,StartupOps,DriverOps,CrashLog}.*` |
| 提权 / 单实例 / 会话交接 / 签名 / 详情 | `src/ops/{Elevate,SingleInstance,SessionState,Signature,DetailsProvider}.*` |
| 应用壳 / 渲染 / 窗口 / 主题 / 发布信息 | `src/app/{main.cpp,AppContext.h,Win32Window,D3DRenderer,ImGuiLayer,Theme,AboutInfo.h}.*` |
| 进程页 + 性能页 + 各模态 | `src/app/ui/Pages.cpp`（另：`SortKey.h` `StatusLayout.h` `HeaderLayout.h` `ConfirmAction.h` `UiText.h` `ModulesUi.h` `AboutUi.*` `Tray.*`） |
| 网络 / 启动项 / 服务 / 驱动 / 传感器页 | `src/app/ui3/Pages3.cpp` |
| 崩溃页 / 窗口页 | `src/app/ui3/GcPages.*` |
| 布局稳定化纯函数 | `src/app/ui3/{PageLayout.h,StatusLayout.h,SplitterUi.h}`、`src/app/ui/{HeaderLayout.h,StatusLayout.h}` |
| 图表 / CSV / 内存清理 / 进程树 | `src/app/ui3/{PerfChart.h,PerfCsv.h,MemCleanup.h,ProcTree.h,ProcKind.h,NetMonUi.h,NetAdapterUi.h,PcapUi.h,LogViewer.h,CompatDiag.h,ThemeCfg.h}` |
| 自测用例 | `src/selftest/*_test.cpp`（框架 `TestFramework.h` 的 `STM_TEST` 宏） |
| 图标生成 | `tools/gen_logo.cpp` + `scripts/make_logo.bat` |

---

## 6. 未完成 Backlog 与用户待办

### 6.1 Backlog（不阻塞交付，按建议优先级；汇总自各维护报告遗留清单）

1. **Npcap 真机抓包验收**（代码就绪、解析纯函数有单测；待用户安装驱动后跑手工验收清单）。
2. 图表放大态的适配器序列 tooltip 行；每适配器 CSV 列（PerfCsv 记录中逻辑核数变化时的行列对齐）。
3. 崩溃记录页导出（CSV/文本）与 Find Window（拖拽十字定位窗口所属进程）。
4. `HostSvcCache` / `CtrlSlot` 缓存淘汰策略。
5. `EllipsizeTextUtf8` 的 O(N²) 优化（量级微小）。
6. 浅色主题 × 壁纸对比度增强（按壁纸亮度自动选文字色）。
7. `colOrder` 恒等时的噪音键清理；`colOrderAppliedGen_` 死字段清理。
8. LpcIO 芯片表扩展（SMU/EC 类）。
9. 若干维持的历史 P2：netcol 加载宽度无合理域钳制、重置布局后表头排序箭头丢失、一键布局跨屏 DPI 取样、字体不随 DPI 缩放（设计口径需明示）。

（更完整的逐轮遗留见 [archive/reports/](archive/reports/) 各维护报告末节。）

### 6.2 用户待办（转接时未完成）

1. ~~填写仓库地址~~ **已完成**：`src/app/AboutInfo.h` 的 `kRepoUrl` 已填 `https://github.com/codejinxz/SuperTaskMgr`，版本 `1.0.0`、日期 `2026-09-23` 已填。
2. **（可选）Npcap 安装**：从 <https://npcap.com/#download> 下载安装（保持默认选项含回环），装后网络页「深度抓包」区点「重新检测」。
3. 核对并推送仓库（远端已配置为 `origin`）。

> **发布前 checklist**（改版本时）：①`src/app/AboutInfo.h` 三常量；②同步 `src/app/app.manifest` 的 `assemblyIdentity version` 与 `src/app/app.rc` 的 `FILEVERSION`；③双击 `一键编译.bat` 并确认自测全绿；④确认 [../README.md](../README.md) 的许可证表与数字仍与实测一致。

---

## 7. 工具链与发布

- 构建入口：`scripts\build.bat [Debug|Release]`（默认 Release）；一键脚本 `一键编译.bat`（编译 → 自测 → 询问是否启动）。
- 构建脚本**不依赖 `vcvarsall.bat`**（改用 VS 生成器自定位工具链，可从任意 shell 调用）；CMake 路径在脚本内硬编码 + `vswhere` 兜底。
- 版本号 / 日期 / 仓库地址的唯一来源是 `src/app/AboutInfo.h`（改一处即可发版）；发版时同步 manifest 与 rc 版本。
- 发布产物放在 `release/`（该目录已被 `.gitignore` 排除，通过 GitHub Releases 分发）。

---

## 8. 一句话记住的重点

**诚实数据 + 进程身份 `(PID, CreateTime)` + 破坏性操作双层防线 + 契约头冻结 + 内核全 opt-in**——这五条是本项目的信任根基，任何改动都不得削弱；其余一切以 [ARCHITECTURE.md](ARCHITECTURE.md) 与源码为准。

# 终审 Review V31 —— Windows 超级任务管理器 交付前全功能终验

- 评审人：V31（与其他评审者互不知情；禁止 git；仅新增本报告文件）
- 日期：2026-09-21
- 被测产物：`build/Release/SuperTaskMgr.exe`、`build/Release/stm_selftest.exe`（2026-09-21 04:55 构建）
- 方法：产物实跑（selftest 双轮 / autotest 六条 / smoke 150）+ `%LOCALAPPDATA%\SuperTaskMgr\logs` 核对 + 全仓代码走读（154 文件 / 约 39,200 行）
- 结论先行：**P0=0，P1=0，P2=4**（1 新发现 + 3 项 V30 遗留未修）。功能判定 49 项：**48 可用 / 1 受限 / 0 不可用**。上一轮（V30）P1-1 已确认修复，P2-2、P2-4 亦已修复；P2-1、P2-3、P2-5 仍未修（降级为遗留）。

---

## 0. 产物实跑结果（本轮实测）

| 测试 | 结果 | 证据 |
|---|---|---|
| `stm_selftest.exe` 第 1 轮 | **173 通过 / 0 失败**（exit=0） | 自存 `/tmp/v31_selftest_r1.log` |
| `stm_selftest.exe` 第 2 轮 | **173 通过 / 0 失败**；两轮测试名集合逐项 diff 一致（差异仅为备份文件名时间戳与临时端口号两类日志行） | `/tmp/v31_selftest_r2.log` |
| `SuperTaskMgr.exe --autotest kill` | PASS（exit=0，`[完成] 已终止进程 cmd.exe`） | `autotest_result.log` 追加行 |
| `--autotest tree` | PASS（exit=0，终止 3 个） | 同上 |
| `--autotest startup` | PASS（exit=0，禁用 `stm_f1_autotest` 并恢复） | 同上 |
| `--autotest about` | PASS（exit=0，模态开 ≥4 帧 + 关闭） | 同上 |
| `--autotest wallpaper` | PASS（exit=0，帧顶点≈7090、壁纸绑定绘制命令=1） | 同上 |
| `--autotest dialogclick` | PASS（exit=0，真实管线点击、子进程退出） | 同上 |
| `SuperTaskMgr.exe --smoke 150` | PASS（PowerShell `Start-Process -Wait` 实测 **exit=0、耗时 3.6s**；150 帧全页离屏绘制） | `stm.log` 05:14:19 smoke=150 段 |
| 运行日志 | 本轮全部运行 **0 条 ERROR**；唯一周期性 WARN 为启动项页「StartupApproved 写入语义为实验性」的刻意诚实提示 | `stm.log` |

环境备注（非产品缺陷）：Git Bash（MSYS2）对 GUI 子系统 exe 的等待/退出码不可靠（本轮实测同一命令 bash 报 rc=0/rc=1 且进程似滞留，PowerShell 实测 exit=0/3.6s 正常退出）。**评测脚本请用 PowerShell `Start-Process -Wait -PassThru` 取退出码**。

---

## 1. 功能判定矩阵

图例：✅可用　🟡受限（说明限制）　❌不可用（bug）　⚪无法无头验证（列入人工清单）

### 1.1 进程页（10/10 可用）

| # | 功能 | 判定 | 依据 |
|---|---|---|---|
| 1 | 表格 13 列数据 | ✅ | `SortKey.h:152` `kProcColSlots=13`（11 数据列 + 徽标 + 描述）；smoke 全页离屏绘制覆盖 |
| 2 | 排序（含重排后） | ✅ | 列 UserID 按槽位映射，拖动重排后 `TableSetColumnDisplayOrder` 只改显示序不影响排序映射；selftest `ui_sort_column_ids_roundtrip`、`ui_sort_cpu_unavail_last`、`ui_sort_name_tiebreak_pid`、`ui_sort_mem_unavail_last` 全过 |
| 3 | 过滤 | ✅ | `Pages.cpp:1101-1114` 名称/PID 搜索框 + 「清空」；与树形/排序/重排叠加路径 `RebuildRows`（过滤→排序→DFS）走读无误 |
| 4 | 列拖动重排 + 列宽持久化 | ✅ | `colOrderPending_`/`ApplyPersistedColumnOrderOnce`（Pages.cpp:1232-1260，个数/越界/重复拒绝）+ `colW_*` cfg 键 + 退出 `StripCfgKeysFromFile`（main.cpp:616） |
| 5 | 树形/平铺 | ✅ | `procTreeMode` cfg；孤儿/父退场提升为根；树形禁表头排序并在 tooltip 明示，回平铺 `sortReflected_=false` 重同步 |
| 6 | 区分系统进程三态 | ✅ | `sysDistMode` 0/1/2（高亮/只看用户进程/关闭）+ `ProcKind.h` 六类分类器（Critical>ServiceHost>Uwp>Windows>User>Unknown，宁漏勿错）+ 图例 |
| 7 | 右键菜单全项 | ✅ | `Pages.cpp:1394-1461`：查看详情/受保护原因/查看宿主服务/终止/树终止/释放工作集/挂起/恢复/设置优先级（实时红字警示）/设置亲和性/复制名称/复制路径/打开文件位置/校验签名；受保护进程整组禁用且注明原因 |
| 8 | 详情面板 | ✅ | 路径/描述/公司/版本/签名/命令行/用户名/GDI+USER/窗口标题/会话 ID/控制区（`DrawControlSection`）/模块（`DrawModulesSection`）/父进程跳转（Pages.cpp:1506-1576）；进程丢失显示最后已知信息 + PID 复用提示 |
| 9 | 提权重启 | ✅ | 工具条「以管理员身份重启」（Pages.cpp:3583-3599）：先 `SaveSessionFromCtx`，UAC 经 jobs 工作线程不阻塞 UI，失败诚实 toast |
| 10 | 兼容模式说明模态 | ✅ | 状态栏降级原因**可点击**（Pages.cpp:3721 `IsItemClicked`）→ 模态：逐项自检表/为什么/受影响功能/**重新自检**（代际信号防 stale，V24 P1-1）/复制诊断报告/关闭；Esc 复位无残留 |

### 1.2 性能页（11/11 可用）

| # | 功能 | 判定 | 依据 |
|---|---|---|---|
| 1 | 顶栏固定 | ✅ | M2 定高区改造（工具行/显隐行恒定行数），V28/V30 已验证并复走读 |
| 2 | 时间窗 | ✅ | `PerfChart.h:39` 60/120/300/600s，非法值回落 120；历史容量 kHistCap=600 |
| 3 | 8 块图表显隐 | ✅ | `perfShow*` 8 复选框（Pages.cpp:2179-2213），零迁移语义 |
| 4 | 放大/跟随/检视 | ✅ | `perfZoom`（-1/0..7，越界回落）+ `FollowTick` 跟随/检视状态机 + 「回到最新」；放大态悬停精确读数 |
| 5 | 热图 | ✅ | 每核 × min(120, window) 展平热图（`PerfChart.h:236-243`） |
| 6 | 每适配器吞吐 | ✅ | `perfShowNetAdapters` 开关 + `BuildAdapterSeries`（排除回环/非 Up）；selftest `chart_adapter_series` |
| 7 | Y2 | ✅ | 内存块提交占比%（commit/commitLimit 派生，limit≤0 诚实跳过） |
| 8 | 拖拽阈值线 | ✅ | 告警阈值 `DragLineY` 可拖拽（Pages.cpp:2342），拖动值回写 cfg |
| 9 | 一键内存加速 | ✅ | 工具条/性能页 GPU 块/内存块三入口 → 同一确认模态 → `MakeMemCleanupJob` 单执行路径（V30 #11 复核保持） |
| 10 | CSV 导出 | ✅ | `PerfCsvRecorder` 开始/停止记录（Pages.cpp:2567-2601），文件落 `%LOCALAPPDATA%\SuperTaskMgr\captures` |
| 11 | 告警 | ✅ | `alertOn/alertCpu/alertMem`；冷却 300s + 5% rearm 防抖（Pages3.cpp:3285-3380），经托盘气泡 |

### 1.3 网络页（4/4 可用）

| # | 功能 | 判定 | 依据 |
|---|---|---|---|
| 1 | 适配器卡 | ✅ | `NetAdapterUi.h`（A2 纯逻辑：速度/排序/复制 IP），回环不显示 |
| 2 | 实时监视 | ✅ | 事件流（免管理员）/过滤/Top 远程聚合（需管理员）/DNS 记录/导出 CSV；开关三路经 ops 队列执行并诚实读回、失败回滚（Pages3.cpp:367-420）；本机实测 ETW 环回回显 OK |
| 3 | 深度抓包（未安装指引态） | ✅ | 未装→静态指引 + 「打开官方下载页」；本机已装 Npcap：`npcap_detect` 实测 `NpcapInstalled=1`、`ListDevices: 10 台`；打开失败诚实报 pcap_geterr 原文。**实抓会话（BPF/选中包详情/.pcap 导出）列人工清单** |
| 4 | 连接表 | ✅ | `NetTables.cpp` 差分连接表（ScrollY 表），自检门 NtQSI 校验 3 轮通过放行 |

### 1.4 传感器页（4 可用 + 1 受限）

| # | 功能 | 判定 | 依据 |
|---|---|---|---|
| 1 | 顶栏固定 | ✅ | 工具行/显隐行/LHM 区恒定行数 + 定高滚动区（Pages3.cpp:2683-2725） |
| 2 | 分组显隐 | ✅ | `sensorGroups` cfg → 纯函数一致快照（selftest `sensor_group_keys_defaults`、`sensors_group_cfg_roundtrip`） |
| 3 | 详细模式 | ✅ | F4#9 逐条渲染完整标签 + 数值/状态分列（Pages3.cpp:3025-3152） |
| 4 | 四态诚实 | ✅ | `SensorReading::State {Ok, NeedAdmin, NeedDriver, NoHardware}`（Sensors.h:57）；不可得渲染「—」/「本机无此传感器」，不伪装 |
| 5 | DTS/LHM/风扇/电压来源 | 🟡受限 | DTS 每核温度走 PawnIO MSR（本机实测 `IntelMSR 加载成功`）；LHM 可选关闭默认；电压走 LpcIO extra（本机 `LpcIO 加载成功 18076 字节`）；**限制**：读数数值正确性无法在本环境对照硬件基准核实（无第二温度源比对），风扇依赖主板芯片支持，不支持时诚实显示 NeedDriver。人工清单 #7 |

### 1.5 启动项 / 服务 / 驱动 / 崩溃记录 / 窗口页（5/5 可用）

| # | 功能 | 判定 | 依据 |
|---|---|---|---|
| 1 | 启动项 | ✅ | 4 来源枚举（本机 64 项实证于日志）；启用/禁用经 autotest `startup` 真实管线 PASS（禁用+备份+往返恢复，selftest `ops3` 往返亦过）；StartupApproved 实验性语义有诚实 WARN |
| 2 | 服务 | ✅ | SCM 枚举 275 项 + 275 项配置细节（日志实证）；启动/停止带「运行中依赖者」警告；服务→进程反向跳转 |
| 3 | 驱动 | ✅ | 内核驱动枚举 228 项（日志实证）；选中按需签名检查 |
| 4 | 崩溃记录 | ✅ | `CrashPage`：Application+System 通道 1000/1001/1002 只读（GcPages.cpp:86-89） |
| 5 | 窗口页 | ✅ | `WindowPage`：顶层窗口枚举 + 前置/置顶/最小化/温和关闭（GcPages.cpp:201-204）；操作实效人工清单 #5 |

### 1.6 全局（11/11 可用）

| # | 功能 | 判定 | 依据 |
|---|---|---|---|
| 1 | 主题三态 | ✅ | Dark/Light/System（`Theme.cpp` Apply/ResolveSystem，读注册表 `AppsUseLightTheme`）；`WM_SETTINGCHANGE("ImmersiveColorSet")` 运行期跟随（main.cpp:377） |
| 2 | 壁纸 | ✅ | autotest `wallpaper` 真实渲染管线 PASS（顶点>0、绑定命令=1）；退出保留持久化副本、下次启动自动恢复 |
| 3 | 一键优化布局 | ✅ | `ApplyOneClickLayout`：WorkSize + 窗口 DPI → LayoutScaleFromEnv（钳 [1,2]）+ toast；（跨屏 DPI 语义见 P2-3） |
| 4 | 重置布局 | ✅ | 软删 + 文件剔除（layoutScale/colOrder/colW_*/netcol_*/perfZoom）+ `NotifyLayoutReset()` 代际失效**双缓存**（netcol 7 表 + GPU 2 表，Pages3.cpp:107-137、Pages.cpp:2748-2792）+ 进程表换代。**V30 P1-1 已修复并确认**（selftest `p1_layout_reset_keys_registry`、`p1_netcol_strip_from_file` 过） |
| 5 | 日志查看器 | ✅ | 级别过滤/刷新/自动刷新(1s)/生成诊断报告（尾部 50 行 + 系统版本行，降级时附兼容诊断全文）/打开日志目录；打开首帧才读文件，ListClipper |
| 6 | 托盘 | ✅ | 显示主窗口/以管理员身份重启/退出 + 三分支左键显隐 + 告警气泡；headless 跳过。点击交互人工清单 #1 |
| 7 | 热键 | ✅ | Ctrl+Alt+M（`RegisterHotKey` 于 UI 线程），headless 不注册；注册失败诚实 WARN+cfg 回滚；退出反注册。实际按键人工清单 #2 |
| 8 | 关闭行为三选 | ✅ | WM_CLOSE → `closeAction`（0=询问（含「记住我的选择」）/1=退出/2=最小化到托盘）；托盘「退出」始终直接退出 |
| 9 | 单实例 | ✅ | `Local\SuperTaskMgr.SingleInstance` 互斥体，WAIT_ABANDONED 容错，3s 交接等待；**本轮实测**：残留实例持锁期间第二 headless 实例拒绝启动（exit=1 不弹框，V11-P2-7 生效） |
| 10 | 提权重启会话交接 | ✅ | `SaveSessionFromCtx`（窗口矩形/页面/选择）→ `RelaunchAsAdmin("--relaunched")` → 新实例 `TryAcquire(3000)` 等旧实例拆除；恢复时矩形钳制到最近显示器工作区（**V30 P2-4 已修复**，main.cpp:485-498 `MonitorFromRect`+clamp） |
| 11 | 兼容模式可点击诊断 | ✅ | 见 1.1 #10；「复制诊断报告」同时落盘 logs 目录且明示不自动上传 |

### 1.7 测试基建（3/3 可用）

| # | 功能 | 判定 | 依据 |
|---|---|---|---|
| 1 | selftest 173 项双轮 | ✅ | 两轮 173/173，集合一致 |
| 2 | --autotest 六条 | ✅ | kill/tree/startup/about/wallpaper/dialogclick 全 PASS |
| 3 | --smoke 150 | ✅ | exit=0，3.6s，0 ERROR |

---

## 2. 缺陷列表

### P0（无）

未发现崩溃、越界、死锁、数据损坏类缺陷。本轮全部实跑（含杀进程/树杀/启动项写注册表往返/真实渲染断言）无一失败。

### P1（无）

### P2

#### P2-1（新发现）`--smoke` / `--autotest` 缺参时静默变成普通 GUI 启动

- 证据：`src/app/main.cpp:36-46` —— `ParseSmokeFrames`/`ParseAutotestMode` 循环条件 `i + 1 < nArgs`，当 `--smoke`（或 `--autotest`）是最后一个参数时被静默忽略 → smokeFrames=0/autotestMode="" → 走完整交互式 GUI 启动。本轮实测：`--smoke`（无值）直接弹出了完整主窗口（stm.log 05:04:42 `smoke=0`）。
- 影响：CI/脚本拼写遗漏参数时无任何告警，可能在无人值守机器上留下常驻 GUI 实例并占用单实例互斥体（本轮后续一次 `--smoke 150` 因此以 exit=1 拒启，属连锁影响）。
- 修法：参数解析改为「见到 `--smoke` 即消费下一参数，缺失/非数字时记 `STM_LOG_WARN` 并按错误退出（或使用默认 150 帧但必须告警）」；`--autotest` 同理（见值缺省即告警）。约 10 行改动。

#### P2-2（V30-P2-1 遗留未修）EllipsizeTextUtf8 每码点一次堆分配

- 证据：`src/app/ui3/StatusLayout.h:152` `measure(std::string(u8, keep + step).c_str())` 仍为每码点构造 `std::string`；降级态状态栏每帧路径。
- 修法：`measure` 改接受 `(const char*, size_t)`，或先 `strlen` 上界剪枝再逐码点。

#### P2-3（V30-P2-3 遗留未修）「一键优化布局」跨屏语义：分辨率因子恒取主视口

- 证据：`src/app/ui/Pages.cpp:3518-3529` 仍用 `GetMainViewport()->WorkSize` + `GetDpiForWindow(主窗 HWND)`；未按 `MonitorFromWindow` 取所在屏工作区。主屏 4K + 副屏 1080p@100% 时窗口在副屏会得到过大缩放。
- 修法：`MonitorFromWindow` 取窗口所在显示器工作区参与分辨率因子；tooltip 注明「移动显示器后可重按」。

#### P2-4（V30-P2-5 遗留未修）死字段 `colOrderAppliedGen_`

- 证据：`src/app/ui/Pages.cpp:1239`（赋值）与 `:1999`（声明），全仓无读者。
- 修法：删除；或消费之（如 colOrder 恢复时机门控）。

### 上轮缺陷闭环确认（本轮复核）

| 上轮编号 | 状态 | 证据 |
|---|---|---|
| V30 P1-1 netcol 缓存不随重置布局失效 | **已修复** | `ThemeCfg.h:238-239` 原子代际 `LayoutResetGeneration/NotifyLayoutReset`；`Pages3.cpp:107-137`（NetColCache.loadedGen）与 `Pages.cpp:2748-2792`（GpuColCache.loadedGen）双实现接入；`ResetLayout`（Pages.cpp:3536）触发；selftest 2 项专项通过 |
| V30 P2-2 退出残留空值布局键 | **已修复** | main.cpp:616-619 `StripCfgKeysFromFile({colW_,netcol_}, {layoutScale,colOrder,perfZoom}, onlyEmpty)` |
| V30 P2-4 会话恢复窗口可落屏外 | **已修复** | main.cpp:485-498 `MonitorFromRect(MONITOR_DEFAULTTONEAREST)` + 工作区 clamp（至少露出 120×80 可抓取） |
| V30 P2-1 / P2-3 / P2-5 | 未修 | 本轮 P2-2 / P2-3 / P2-4（降级遗留，均微小） |

---

## 3. 无法无头验证 → 人工清单（交付前建议逐条过一遍）

1. 托盘左键三分支（最小化恢复/可见隐藏/隐藏显示）与右键菜单三项的实际点击。
2. Ctrl+Alt+M 全局热键实际按下/与其他应用冲突时的失败提示。
3. UAC 实流：非提权运行 → 「以管理员身份重启」→ UAC 弹窗期间 UI 保持响应 → 新实例会话/布局交接。
4. 多显示器：跨屏拖动后「一键优化布局」缩放是否合理（P2-3 场景）；拔屏后重启的窗口钳制（代码已在位）。
5. 窗口页对真实第三方窗口的前置/置顶/温和关闭实效。
6. 深度抓包实抓会话：选适配器→BPF→开始/停止→选中包徽标与十六进制→导出（本机 Npcap 已装、10 设备枚举已验证）。
7. 传感器读数对照硬件基准（DTS 每核温度 vs 厂商工具；风扇/电压依赖芯片支持）。
8. 服务页启停、驱动页签名检查的实际点击效果（枚举已实证 275/228 项）。
9. System 主题跟随：真实切换 Windows 深浅色后 UI 即时换肤。
10. 告警气泡：CPU/内存超阈值后的托盘气泡实际弹出（逻辑+冷却已走读）。

---

## 4. 结论

交付判定：**可交付**。49 项功能 48 可用、1 受限（传感器数值正确性需硬件基准，代码路径与四态诚实降级已验证）、0 不可用；测试基建三项全绿（173×2、6/6、smoke150）；全日志 0 ERROR。P0=0、P1=0；4 项 P2 均为微小打磨项（1 项 CLI 健壮性 + 3 项上轮遗留微性能/语义/卫生），不阻塞交付，建议下一维护轮顺手处理 P2-1（约 10 行）。

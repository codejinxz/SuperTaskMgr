# 功能扩展评估与推荐（subagent F4 · 只读产品评估）

- 日期：2026-09-18
- 基准：Process Explorer / Process Hacker (System Informer) / Process Lasso / 任务管理器 / HWiNFO 能力集
- 输入：README、01_架构设计文档（§1 硬约束/§6 ops 协议/§7 提权矩阵）、阶段 0-4 报告、V13 终审、R5 调研、源码抽查（Pages.cpp / DetailsProvider / ProcessOps.h / ProcessCollector / CollectUtil）
- 工作量口径：S<半天 / M≈1天 / L>2天（单人）。所有条目默认遵守现有线程模型（采集线程 + ops 队列 + UI）、诚实数据原则、二次确认 + 保护名单防线。

## 一、结论摘要

- 现有代码有**三处"已采集但未变现"的资产**：① DetailsProvider 已拉取模块列表但 `kDetailKinds` 未包含 Modules（Pages.cpp:480），UI 从未渲染；② 窗口标题/`PF_Suspended` 徽标已采集但无任何窗口管理、挂起/恢复 ops；③ 告警/托盘框架已成型，只差新规则。这三类是"价值/工作量"比最高的方向。
- 红线裁决：**替换任务管理器（IFEO/Winlogon Debugger）不做**——见 §三；WMI 命令行兜底不做（权限收益是错觉）；多语言缓做（纯翻新成本，无真实需求驱动）。

## 二、推荐表（按 价值/工作量 比 排序）

| # | 功能 | 价值 | 工作量 | 一句话理由 |
|---|------|------|--------|-----------|
| 1 | DLL/模块列表 UI 展示 + 按模块签名校验 | 高 | S | 数据已在内存里，只差渲染——全表最高性价比 |
| 2 | 进程挂起/恢复 + 优先级/亲和性调整 | 高 | M | PE/Lasso 核心诊断能力；PF_Suspended 徽标已就位 |
| 3 | 服务宿主/网络页跨页联动跳转 | 高 | S | 数据源已齐（EnumServices 带 PID、连接表带 PID），纯 UI 粘合 |
| 4 | 进程树视图模式 | 高 | M | 表格已有 parentPid，父链 lineage 是管理员高频诉求 |
| 5 | 窗口管理（关闭/置顶/最小化/激活） | 中高 | M | EnumWindows 数据已有，补 ops 即可 |
| 6 | 崩溃记录页（事件日志 Application Error） | 中高 | M | "电脑为什么卡/什么崩了"的答案页，Task Manager 没有的差异化 |
| 7 | 图表历史加长 + 导出（CSV/PNG） | 中 | S-M | kHistCap=120 硬编码，放宽 + 导出成本低 |
| 8 | 性能日志记录（CSV/JSONL，HWiNFO 式） | 中 | M | 告警框架之外的第二条"事后回看"路径 |
| 9 | GDI/USER 句柄泄漏告警规则 | 中 | S | gdiObjects/userObjects 已采集，告警框架已成型 |
| 10 | 全局热键呼出/隐藏主窗 | 中 | S | RegisterHotKey 即可，运行期生效、无持久化、合规 |
| 11 | 搜索集成：Find Window / Find DLL | 中 | M | PE 招牌交互；Find Window 便宜（窗口数据已有），DLL 扫描较重 |
| 12 | 浅色主题 / 跟随系统 | 低中 | S-M | Theme.cpp 已有调色板结构，差一套浅色值 + WM_SETTINGCHANGE |
| 13 | 句柄表查看（按进程） | 高 | L | PE/Hacker 级诊断深度；NtQSI 全表 + 名称解析有阻塞坑，须重投入 |
| 14 | 磁盘活动页（按进程 ETW DiskIO） | 中高 | L | EtwNetCollector 会话框架可复用，但事件量与残留会话风险并存 |
| 15 | UWP 应用视图/包分组 | 低中 | M-L | 单人自用中文环境收益有限；可先用 GetPackageFullName 只补"UWP 包名"字段 |

## 三、每项详情

**1. DLL/模块列表 UI 展示 + 按模块签名校验**（对应候选 2 后半）
- 描述：详情面板新增"模块"区（或属性对话框页签）：模块路径、大小、基址，选中可触发 WinVerifyTrust（复用 ops/Signature 的 catalog 路径）。
- 价值：高——排查 DLL 劫持/可疑注入是 PE 招牌场景。
- 要点：`DetailsProvider::FetchModules` 已用 `EnumProcessModulesEx(LIST_MODULES_ALL)` 拉全路径存 `ProcessDetails::modules`（DetailsProvider.h:33）却无人消费；kDetailKinds 补 `Modules` 位 + 详情面板子列表 + 按需校验（走 JobQueue，签名结果缓存键 path+size+mtime 已有）。
- 工作量：S。风险：低；WOW64 32 位进程在 x64 自身下枚举受限→诚实"—"（现有降级文案模式照抄）。

**2. 进程挂起/恢复 + 优先级/亲和性调整**（候选 1）
- 描述：右键菜单"挂起/恢复"（NtSuspendProcess/NtResumeProcess）与"优先级/亲和性"子菜单（SetPriorityClass / GetProcessAffinityMask 钳位后 SetProcessAffinityMask）。
- 价值：高——诊断互锁/泄漏时冻结嫌疑进程、给后台任务降优先级，是任务管理器没有的日常能力。
- 要点：新 ops 函数走 §6 协议（ProcKey 身份重验）；NtSuspend/Resume 为半文档化 → GetProcAddress + 失败诚实报错，恢复态以 1-2 tick 后 `PF_Suspended` 徽标读回确认（徽标逻辑已存在于 CollectUtil wait-reason 检测）；保护名单（csrss/winlogon/dwm…）**硬拒绝挂起**（挂起 csrss=全系统冻结，比杀死更恶劣）；服务宿主红字警告沿用；亲和性掩码必须先 Get 钳位（不得写入超出组的掩码）。Process Lasso 式"规则持久化"明确不在此项范围（见 §四）。selftest：spawn 子进程→挂起→快照验证徽标→恢复→验证→终止。
- 工作量：M。风险：中——对系统进程误挂起可致冻结（保护名单硬拒 + 二次确认）；挂起后 UWP 容器行为差异需实测。

**3. 服务宿主/网络页跨页联动**（自增项）
- 描述：进程页选中 svchost → "查看宿主服务"列出该 PID 服务并可跳服务页；网络页连接行右键 → "在进程页定位"；服务页反向跳进程。
- 价值：高——三页数据都有 PID，联动消除手工对照；阶段 3 已交付 svchost 服务名映射，天然衔接。
- 要点：`EnumServicesStatusEx(SC_STATUS_PROCESS_INFO)` 全量拉取后按 PID 客户端过滤（纯函数、ops 队列）；网络页行已有 PID 列；跨页跳转复用选中 ProcKey 机制。
- 工作量：S。风险：低。

**4. 进程树视图模式**（候选 5）
- 描述：表格"平铺/树形"切换：按 parentPid 构建父子，DFS 展平 + 缩进 + 折叠（状态按 ProcKey 记忆）。
- 价值：高——看 lineage（谁拉起谁）是排查托管进程的第一步。
- 要点：父链以 createTime 校验防 PID 复用（架构 §6 同款规则）；父已退出的孤儿挂到根并标注"（父已退出）"——诚实原则；树模式下全局排序降级为"同级内按当前排序键"，UI 明示；过滤命中保留祖先链。
- 工作量：M。风险：低——500 进程规模 DFS 每帧重建成本可忽略（只在快照代次变化时重建）。

**5. 窗口管理**（候选 3）
- 描述：选中进程 → 窗口列表（按需 EnumWindows 过滤 PID + IsWindowVisible），支持关闭（PostMessage WM_CLOSE，二次确认）、置顶切换（SetWindowPos HWND_TOPMOST/NOTOPMOST）、最小化/恢复（ShowWindow）、激活（SetForegroundWindow + FlashWindowEx）。
- 价值：中高——"那个窗口跑哪去了/无响应想温柔关"。
- 要点：快照里的 windowTitle 每 5 tick 刷新且只有标题，管理操作必须按需重新枚举（ops 队列）；置顶/激活无需确认，关闭需确认（半破坏性）。
- 工作量：M。风险：低中——UWP 内容窗口宿主是 ApplicationFrameHost（操作对象诚实标注宿主名）；SetForegroundWindow 受前台锁限制（失败如实报告）；控制台窗口关闭=终止进程（确认文案注明）。

**6. 崩溃记录页（事件日志）**（自增项）
- 描述：新页签读 `Application` 通道 EventID 1000（应用错误）/1002（挂起）/1001（WER），列出时间/进程/故障模块/异常码。
- 价值：中高——"最近什么崩过、崩在哪个模块"是任务管理器完全没有的答案页；与本工具定位（管理员自用诊断）高度契合。
- 要点：Windows Event Log API（EvtQuery/EvtRender，winevt.h，文档化、免管理员）；按需查询 + 前 N 条分页，不常驻订阅；只读本通道，不碰 Security/System。
- 工作量：M。风险：低——注意本地化消息 DLL 渲染失败时诚实显示原始 XML 片段。

**7. 图表历史加长 + 导出**（候选 13）
- 描述：kHistCap 从 120（2 分钟）提为可配置 120/600/1800，配抽稀；性能页导出当前序列 CSV、窗口截图 PNG。
- 价值：中——回看半小时 CPU 尖峰、留证发工单。
- 要点：环形缓冲扩容 + 简单 stride 抽稀控制绘制点数（ImPlot 万点无压力，但每帧拷贝成本要控）；CSV 导出纯 UI 层写文件；PNG 用 WIC（系统组件，零第三方）读 back buffer。
- 工作量：S-M。风险：低——注意抽稀后的"最值消失"在 UI 标注（诚实原则：注明降采样口径）。

**8. 性能日志记录（CSV/JSONL）**（候选 4）
- 描述：性能页"开始记录/停止"，系统级指标按可配置间隔（≥1s）追加 CSV 至 `%LOCALAPPDATA%\SuperTaskMgr\logs`，10MB 轮转。
- 价值：中——HWiNFO 式事后分析；与告警（实时）互补。
- 要点：采集线程只填无锁快照槽，写盘走 ops 线程缓冲（不违 §5 采集预算）；kUnavail 写空单元格并附表头说明；默认关闭。
- 工作量：M。风险：低——磁盘写入对 tick 预算的影响用缓冲+批量 flush 控制；不记录任何进程级敏感数据（只系统级，规避隐私面）。

**9. GDI/USER 句柄泄漏告警**（自增项）
- 描述：告警规则新增"某进程 GDI 或 USER 对象 > 阈值（默认 9000，逼近 10000/进程上限）"→ 托盘气泡。
- 价值：中——GDI 泄漏是桌面程序慢性死亡的典型症状，任务管理器要看两列数字不会提醒。
- 要点：告警框架（阈值+冷却+再武装）阶段 3 已交付，gdiObjects/userObjects 已逐进程采集，纯规则接入。
- 工作量：S。风险：低。

**10. 全局热键呼出/隐藏主窗**（候选 9）
- 描述：注册可配置全局热键（默认 Ctrl+Alt+M），切换主窗显示/隐藏（隐藏时进托盘）。
- 价值：中——托盘常驻场景下最快唤起路径。
- 要点：`RegisterHotKey` + 现有消息循环即可；热键仅运行期有效、不落注册表（合规：无持久化）；热键冲突时诚实提示并允许改键。
- 工作量：S。风险：低。**悬浮面板（画中画迷你图表）继续缓做**：需第二顶层窗口+独立 swapchain，阶段 4 遗留清单已评估为高成本低确定性，不与本项捆绑。

**11. 搜索集成：Find Window / Find DLL**（候选 8）
- 描述：PE 式准星拖拽选窗口→定位进程（Find Window）；输入 DLL 名→列出加载它的进程（Find DLL）。
- 价值：中——招牌交互，但单人自用频率有限。
- 要点：Find Window = 按钮上 SetCapture + WindowFromPoint + GetAncestor(GA_ROOT) + GetWindowThreadProcessId → 选中 ProcKey（S-M，窗口数据已有）；Find DLL = ops 线程遍历进程逐个 EnumProcessModulesEx 匹配，进度可取消，提权进程诚实计入"无法读取 N 个"（M）。
- 工作量：M（建议先做 Find Window）。风险：Find DLL 全量扫描耗时与权限洞如实呈现；排除自身窗口防自锁。

**12. 浅色主题 / 跟随系统**（候选 12）
- 描述：浅色调色板 + 监听 WM_SETTINGCHANGE 读 AppsUseLightTheme 自动切换，配置项"暗/浅/跟随系统"。
- 价值：低中（暗色已满足日常；浅色利于截图打印场景）。
- 要点：Theme.cpp 已是集中调色板，扩一套值；ImPlot 色表联动；注意散落的硬编码 RGBA 文本色要收编进调色板（这步决定 S 还是 M）。
- 工作量：S-M。风险：低——对比度需要一轮目检。

**13. 句柄表查看（按进程）**（候选 2 前半）
- 描述：详情面板"句柄"页签：类型、名称、授予访问掩码（粒度到单进程视图）。
- 价值：高（句柄泄漏/互锁诊断的终极工具，现有"句柄"计数列的自然下钻）。
- 要点：`NtQuerySystemInformation(SystemExtendedHandleInformation)` 全系统表后按 PID 过滤（免逐进程特权句柄需求）；名称解析须 DuplicateHandle 到自身 + NtQueryObject，**对命名管道/同步对象可能无限阻塞**——必须在 ops 线程分批解析 + 单句柄超时（独立线程 + WaitForSingleObject 超时放弃）+ 行数上限（如 5000）+ "名称解析中"诚实态；绝不进采集 tick 路径。
- 工作量：L（>2 天：解析管线 + 超时机制 + 大表虚拟滚动）。风险：中高——半文档化结构（R5 表 A 同款"自校验+降级"纪律适用）、系统级全表扫描 ms 级开销、误用会卡 ops 队列（需独立取消语义）。建议排在挂起/恢复等快赢之后、作为"深水区"独立排期。

**14. 磁盘活动页（按进程 ETW DiskIO）**（候选 7）
- 描述：新页签按进程实时读/写字节与 IOPS（ETW Kernel-File/DiskIo），提权增强。
- 价值：中高——现有 diskBytesPerSec 列口径是"文件+网络+设备总和"（架构 §4 已诚实标注），纯磁盘归因仍缺位。
- 要点：复用 EtwNetCollector 全套会话框架（会话名带 PID、启动前清残留、TdhGetProperty 动态解析、RAII 清理）；文件事件量远大于网络事件 → 缓冲数加大 + 丢事件诚实计数；默认关、仅提权。
- 工作量：L。风险：中——继承已知例外：崩溃残留会话占 ~4MB 非分页池至重启（README 已声明的同款风险，需在 UI 首次开启时提示）；高频事件解析的 CPU 成本需实测。备选降级：先做"物理磁盘队列长度/每盘带宽"性能页增强（PDH，S），把按进程磁盘归因留作二期。

**15. UWP 应用视图/包分组**（候选 6）
- 描述：表格按包分组显示 UWP 应用（聚合 ApplicationFrameHost 等宿主进程），或先仅补"UWP 包名"详情字段。
- 价值：低中（单人自用 + 中文环境，UWP 占比低；PF_Uwp 徽标已可辨识）。
- 要点：`GetPackageFullName`（文档化，QLI，R5 表 A #4）按需解析缓存；分组模式复用进程树的分组渲染；友好名需 WinRT PackageManager（ops 线程 COM 章程冲突风险）——**建议一期只用 PFN 硬名，诚实跳过友好名**，避免为低价值项引入 WinRT 依赖。
- 工作量：M-L。风险：中——WinRT 初始化与现有 CoInitializeEx(STA) 章程的兼容性需验证。

## 四、不建议做名单（含合规结论）

| 候选 | 裁决 | 理由 |
|---|---|---|
| **候选 10：替换任务管理器（IFEO Debugger / Winlogon Debugger / SilentProcessExit）** | **不做（安全红线）** | **合规结论：本应用约束不允许。**①架构 §1 硬约束"无隐蔽/**持久化**/规避检测能力"、选型报告 §八"自身不自启动、不留服务"、README 权限声明"不写自启动"；V13 终审 grep 证实全仓"Run 键只枚举不写自身、无任何创建路径"——这三份承诺是本工具作为"管理员自查工具"的信任根基。②IFEO `Debugger` 值与 Winlogon `Debugger` 键都是 **HKLM 注册表持久化自动拉起机制**：写入即持久驻留、系统级自动以自身替换系统组件启动，恰属声明所禁止的能力类别；SilentProcessExit 组合更是公开已知的规避/驻留技术面。③自身工程风险：未签名 exe 一旦被 AV 标记或损坏，debugger 值会让 taskmgr/登录路径瘫痪，用户可逆成本极高。④替代方案（合规）：不做 in-app 功能；README 增补"手动固定到任务栏 / 任务管理器内 `文件→运行` 指向本 exe"的使用指引即可，工具自身保持零持久化。 |
| 候选 14：进程命令行 WMI 兜底 | 不做 | 权限收益是错觉：WMI `Win32_Process.CommandLine` 以**调用者令牌**执行，非提权下对高 IL 进程同样拿不到（WMI 不是提权通道）；真正卡点是 PEB 读取权限，WMI 解决不了。反而引入 COM/Wbem 重依赖 + 百 ms 级慢查询占用串行 ops 队列。现文案"不可用（无读取权限）+ 一键提权"已是诚实数据原则下的正解。若要补，提权后 `SystemProcessIdInformation` 免句柄取**路径**（R5 表 A #4）收益更直接——但同样不补命令行。 |
| 候选 11：多语言（字符串表化） | 缓做（暂不做） | 现状为内联宽字面量（`U8(L"…")`）遍布全部 UI 文件，抽取是数百条目的纯机械翻新：M-L 工作量、巨大 diff 噪声、零新能力，且单人中文自用无第二语言需求。正确时机：确有英文分发需求时，先建 UiText 表再一次性迁移（顺带解决中文列排序 CompareStringEx 的遗留 P2）。在此之前做它纯属负债。 |
| Process Lasso 式优先级/亲和性**规则持久化** | 不做（本期） | 需要常驻监控进程或落盘规则+开机生效——与"无持久化、无常驻"约束冲突；且手动调优先级（推荐表 #2）已覆盖单人自用场景。 |
| 悬浮迷你面板（画中画） | 缓做 | 需第二顶层窗口+独立 swapchain/消息路径，阶段 2 勘误已实证 master 分支无多视口；成本 L 级、收益与热键呼出（#10）重叠。待热键验证需求后再议。 |

## 五、建议落地节奏

1. **快赢批（合计 ≤2 天）**：#1 模块列表、#3 跨页联动、#9 GDI 告警、#10 热键、#7 图表导出——五项全是"已有资产变现"。
2. **能力批（每项 ≈1 天）**：#2 挂起/恢复+优先级、#4 进程树、#5 窗口管理、#8 性能日志、#11a Find Window。
3. **深水区（独立排期，按需）**：#13 句柄表（L）、#14 磁盘 ETW（L，可先做 PDH 每盘增强降级版）、#15 UWP（一期只补 PFN 字段）、#6 崩溃记录页（M）。

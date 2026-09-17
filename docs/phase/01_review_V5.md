# 01_架构设计文档 审查 V5（维度：完备性 / 安全性 / 可测试性）

- 日期：2026-09-18 ｜ 审查人：独立审查 subagent V5（与其他审查者互不知情）
- 输入：01_架构设计文档.md（主对象）、00_技术选型评估报告.md（§五/§六/§八）、R5_Collection_APIs.md、R6_Sensors_Drivers.md
- 方法：需求→架构逐条落点对照 + 契约头文件（§4/§7）逐字段核对 + 资源/滥用面/可测试性专项检查

## 结论摘要

架构分层（app→ops→core / app→collect→core、三线程、SnapshotStore 无锁读）、提权矩阵、自校验门降级链、(PID,CreateTime) 身份模型质量较高。但**契约头文件在冻结前存在字段级缺口**（按进程 GPU、内存三项口径、ETW 关闭时的诚实显示），且**资源所有权章程完全缺失**——这两点必须在阶段 2 开工前修复，否则并行开发必然返工。

**统计：P0 × 3 ｜ P1 × 6 ｜ P2 × 13**

---

## P0（开工前必须修复）

### P0-1 契约自相矛盾：`ops ↛ collect` 但 ops 需要 collect 目录里的 ProcKey
- §2 规定 `ops ↛ collect`（操作层不依赖采集层），§3 把数据契约 `ProcessData.h` 放在 `src/collect/`，而 §3/§7 中 ops 层（ProcessOps/ProtectedList）全部以 `ProcKey` 为参数——stm_ops 静态库要么违规 include collect 头，要么被迫复制类型定义。
- 修正：把 `ProcessData.h`（至少 ProcKey/ProcInfo/Snapshot/SnapshotStore 声明）移入 `stm_core`（如 `src/core/contract/ProcessData.h`）或建 CMake INTERFACE 目标 `stm_contract` 供 collect/ops/app 共同包含；同步更新 §2 依赖图与 §3 目录注释。此项必须在写入契约头文件之前定案，否则阶段 2 三个模块第一天就会撞上。

### P0-2 数据契约字段缺口（冻结前不补，阶段 2/3 必返工）
1. **GPU 无按进程落点**：原始需求"进程监控（…GPU…）"。GpuCollector 用 GPU Engine 计数器（实例名 pid_<pid>，天然按进程），但 ProcInfo 无 `gpuPercent`/`gpuMemBytes` 字段，§8 进程页无 GPU 列，§8 性能页"四块"也没有 GPU 块——GPU 数据采集了却无处安放。修正：ProcInfo 增加 `double gpuPercent`、`uint64_t gpuMemDedicated, gpuMemShared`（PDH \GPU Process Memory 聚合），性能页改五块或性能页内加 GPU 卡片。
2. **内存三项口径未定义**：需求为"WS+私有+提交"。契约只有 `workingSet` + `privateBytes(=PrivateUsage, 注释"=commit for most")`——把"私有"与"提交"合一。若"私有"指任务管理器默认内存列口径（私有工作集 = \Process(*)\Working Set - Private），则该字段缺失；"提交大小"（=PrivateUsage）未单列；R5 #3 的 EX2 `SharedCommitUsage` 未提。修正：契约改为 `workingSet / privateWorkingSet / commitBytes` 三字段并注明各自 API 来源与口径，进程列与 tooltip 同步标注。
3. **ETW 关闭时 `netBytesPerSec = 0` 违反诚实显示规则**：§4 注释"else 0"与 §8"绝不显示 0 冒充"直接冲突，且 Snapshot/SystemInfo 无 `etwEnabled` 标志，UI 无法区分"未启用"与"真的是 0"。修正：未启用时置 NaN（或 -1）并在 Snapshot 增加采集能力标志位（etw/disk 口径），UI 按 §8 规则显示"—"。

### P0-3 树杀与保护名单的执行期语义未定义（安全关键）
§7 注释只说"Single hard gate: re-validates ProtectedList"，存在三个缺口：
1. **树成员级防线**：`TerminateTree` 的"硬拒绝"是否覆盖树中每个成员未定义。树中若含受保护进程（如 explorer 树下挂的系统组件），必须逐成员重验 + 跳过 + 在结果中如实报告"已跳过 N 个受保护进程"，而不是整树拒绝或整树放行。
2. **ops 层父链数据来源矛盾**：ProtectedList 用"名称+路径+父链识别"（如父链到 PID 4），但 ops ↛ collect，执行期（UI 确认后异步执行，快照已陈旧）ops 从哪取父链？必须规定：ops 内自含一次最小重枚举（NtQSI/Toolhelp，core 提供），执行期以枚举结果 + (PID,CreateTime) 双重重验目标身份（防确认到执行之间 PID 复用），保护名单判定不得使用 UI 传入的陈旧快照数据。
3. **API 签名不一致**：`ProtectedReason(uint32_t pid)` 只吃 PID，违背文档自己的身份原则；改为 `ProtectedReason(const ProcKey&)`，内部配合上述重枚举。
- 修正落地：§7 增补"破坏性操作执行协议"小节：① ops 线程执行时重新枚举目标身份（createTime 不匹配→放弃并报"进程已退出"）；② 树杀先以 CollectTreeMembers(k) 枚举（纯函数，供 selftest 无害测试），逐成员过 ProtectedList，输出"将杀/将跳过"清单（该清单回传确认框二次展示）；③ 自底向上执行，跳过项不中断。

---

## P1（阶段 2 内必须修复）

- **P1-1 资源所有权章程完全缺失**：句柄/COM/PDH/ETW/ImGui 五类资源的生命周期约定在架构中零着墨（WinVerifyTrust 的 STATEACTION_CLOSE 配对、PDH query 生命周期与实例重建策略、ETW 会话 StopTrace 时机、OpsWorker 线程 CoInitializeEx、ImGui context/D3D 设备释放顺序均未成文）。修正：§5 增补"资源所有权"小节 + 采用附 C 自检清单作为阶段 2/3 评审门禁。
- **P1-2 服务进程终止连坐语义缺失**：00 §五/R5 #7 要求"服务进程正确终止 = ControlService 停服后再杀"，架构未承接。PF_ServiceHost 目标直接 TerminateProcess 会连坐 svchost 同宿主的其他服务；且 PID→服务名映射（R5 #7b QueryServiceStatusEx）未进任何契约。修正：阶段 2 至少实现"确认框列出该 PID 宿主的全部服务名并红字警告连坐影响"，停服优先流程可排阶段 3（ServiceOps）；把 `servicesOnPid` 加进 DetailsProvider 清单。
- **P1-3 页签/详情无扩展点，阶段 3 并行必冲突**：§8 各页为硬编码文件，无 IPage 注册机制；DetailsProvider 写死六项。阶段 3 的 NetPage/StartupPage/ServicesPage/SensorsPage/FloatingWindow 全都要改 AppContext/main → 多 agent 改同一文件。最小改造（阶段 2 即落地）：`struct PageContext { SnapshotStore; JobQueue*; NotificationQueue*; Cfg; }; class IPage { const char* Name(); void Draw(const Snapshot&, PageContext&); };` + 静态注册数组；DetailsProvider 改 `Get(ProcKey, DetailKind)` 可扩展枚举。
- **P1-4 启动项禁用前自动备份要求丢失**：00 §八明确"禁用操作前自动导出原值备份（.reg/JSON）到日志目录，保证可逆"，架构 §7/§8/§9 均未承接。修正：写入 StartupOps 契约注释与 §9（备份文件命名/目录/写入时机：禁用动作同事务内先备份后改）。
- **P1-5 JobQueue/NotificationQueue 契约未成文**：三线程模型里只有名字。任务结构（目标 ProcKey、操作类型、回调/完成通知）、结果如何回 UI（NotificationQueue 元素类型）、**应用退出时在途破坏性任务的取消/等待策略**（绝不能退出时还在杀进程）均未定义——这是 collect/ops/app 三模块最大的相互等待点。修正：§5 给出两个队列的类型定义与关停协议（退出=停止接收新任务→等待当前任务完成或安全放弃→drain 通知）。
- **P1-6 首 tick 前 SnapshotStore::Get() 契约空洞**："never null after first tick"暗示之前可为 null，但 UI 启动即渲染进程页（首 tick 前有 ≥1s 窗口），空指针/空白路径未定义。修正：契约改为"构造时即 Set 一个空 Snapshot"，Get() 永不返回 null；procs 为空即显示加载态。

## P2（阶段 2/3 择机修复，附审查清单中带出）

- **P2-1 日志脱敏规则缺失**：DetailsProvider 抓命令行（常含 token/密码）与窗口标题，§9 未禁止入日志。补规则：日志只记操作对象 PID/进程名/路径/结果/HRESULT，禁记命令行、窗口标题、网络对端 IP 明细。
- **P2-2 "仅本机管理用途"未工程化**：§1 硬约束无"无监听套接字/无远程接口"声明。补：硬约束加一句 + 构建门禁：四个 target 一律禁链 ws2_32（iphlpapi 不开套接字，允许）；吊销检查默认离线（承接 00 §五）复述进 Signature 契约注释。
- **P2-3 session.json 内容未定白名单**：§6 只列了意图。明确白名单：页签 id、选中 ProcKey、窗口 RECT、排序/列宽、ETW 开关——禁写任何进程命令行/路径以外的用户数据；`RelaunchAsAdmin(args)` 参数内容限定为固定模式串（不含第三方输入）。
- **P2-4 磁盘口径标注未落契约**：`diskBytesPerSec`（PDH IO Data Bytes/sec = 文件+网络+设备总和）需在 §4 注释与列 tooltip 承接 00 §五"如实标注"要求。
- **P2-5 传感器诚实三分法未进 §8**：风扇/每核温度为"不可达（需驱动支持）"、热区/SMART 为"需管理员"、无硬件为"该项不可用"——§8 目前只有"—/需提权"两态。SensorsPage 验收须含"不可达态绝不显示 0/假值"断言（R6 §④）。
- **P2-6 ETW 按进程流量开关的隐私/性能说明缺失**：确认/设置文案须说明①捕获全机流量含其他用户会话、②事件率高（R5 #10b），默认关闭。落 §8/§9 config 注释。
- **P2-7 阶段 3 验收过粗**：§12"新增…页全可用"不可测。细化为可断言条目：网络页含 TCP+UDP 两表、PID→进程名列、OWNER_MODULE 空标注（非管理员）；启动项四来源齐全+禁用后备份文件存在；驱动页 24H2 无 SeDebug 时显示"需提权"而非空表；传感器不可达态展示正确。
- **P2-8 无 GUI 冒烟方案缺失**：补两件事：① `SuperTaskMgr.exe --smoke N`：完整初始化（D3D+字体+全部页签）渲染 N 帧后退出码 0；② stm_selftest 内用隐藏窗口跑 300 帧 ImGui+ImPlot（控制台进程可建 D3D11 设备），CI 可用。中文路径冒烟（§12 阶段4）同步加进 selftest。
- **P2-9 权限×版本测试矩阵未定义**：定义并进 `--json` 输出：功能集（枚举/路径/树杀/停服/SMART/热区/驱动枚举/ETW/启动项写）× {普通权限, 提权} × {23H2 本机, 24H2 前向兼容（EnumDeviceDrivers NULL 行为，R5 #13）}。
- **P2-10 父进程跳转边界未定义**：父进程不在当前快照（已退出）时显示"（已退出）"；跳转按 ProcKey 匹配快照，不得按裸 PID 跳（防复用误跳）。
- **P2-11 树杀可测性**：P0-3 的 CollectTreeMembers 设计为纯函数（不杀），selftest 可对真实进程树做"枚举+保护过滤"断言而无需真杀。
- **P2-12 §11.1"≥13 列"与 §8 列清单不一致**：§8 实际可数约 11 列。统一口径（列清单↔验收数字），否则阶段 4 验收争议。
- **P2-13 NtQSI 降级慢路径的能力差异未写进 UI 规则**：Toolhelp 慢路径无 CPU/IO/句柄数（R5 #1b），§8 需补"兼容模式下这些列显示—并提示兼容模式"，防止显示 0 冒充。

---

## 附 A：需求→架构落点对照（专项检查结果）

| 需求项 | 落点 | 判定 |
|---|---|---|
| CPU/句柄/线程/缺页/上下文切换/IO | §4 ProcInfo + §8 进程列 | ✅ |
| 内存 WS+私有+提交 | §4 仅 2 字段，私有/提交合一 | ❌ → P0-2.2 |
| GPU 总量 / GPU 显存按进程 | §4 仅系统级 GpuAdapterInfo | ❌ → P0-2.1 |
| 磁盘按进程 | §4 diskBytesPerSec（口径未标注） | ⚠️ → P2-4 |
| 网络 TCP/UDP 表 | §2 NetCollector（阶段3 NetPage 验收过粗） | ⚠️ → P2-7 |
| 按进程流量开关隐私/性能说明 | 缺 | ❌ → P2-6 |
| 父链展示/跳转 | §11.4 ✅；边界未定义 | ⚠️ → P2-10 |
| 路径/签名（诚实空值） | §4/§7/§8 | ✅ |
| 终止 单/树/SeDebug | §6/§7 | ✅（树杀语义 → P0-3） |
| 终止 服务进程 | 缺停服/连坐语义 | ❌ → P1-2 |
| 内存清理+诚实说明 | §7/§11.3 | ✅ |
| 启动项四来源+禁用备份 | StartupOps 阶段3；备份丢失 | ⚠️ → P1-4 |
| 服务与驱动（24H2 SeDebug） | §6 矩阵 ✅ | ✅ |
| 传感器（含风扇不可达诚实展示） | SensorsPage 阶段3；三分法缺失 | ⚠️ → P2-5 |
| 图表/悬浮窗/告警 | §8 | ✅ |
| 二次确认+保护名单双层 | §7/§8 ✅（成员级 → P0-3） | ✅/⚠️ |
| 全 Unicode | §3/§12 ✅ | ✅ |
| 文档化 API 优先+降级 | §4 自校验门 ✅（慢路径 UI → P2-13） | ✅/⚠️ |
| 高频采样不拖慢系统 / UI 不卡顿 | §5/§10 | ✅ |
| 依赖许可证注明 | §3 LICENSE + §12 交付 | ✅ |
| 仅本机管理用途 | 未工程化 | ⚠️ → P2-2 |

## 附 B：stm_selftest 用例覆盖清单（对照 §11/§12，供阶段 2/3 直接引用命名）

collect：
1. `collect.snapshot.completeness`——快照字段完整性、procs 升序、tickId/tickSec 一致（§11.6）
2. `collect.selfcheck_gate.verdict`——自校验门输出+降级标志置位逻辑（§11.6）
3. `collect.tick.budget`——500 进程 tick p50/p95 打印（§10）
4. `collect.identity.createTime`——PID 复用模拟：同 PID 异 createTime 视为新进程（R5 表B#10）
5. `collect.pdh.expand_and_map`——通配符展开+ID Process 实例映射+陈旧实例过滤（R5 #9）
6. `collect.gpu.virtual_adapter_filter`——本机断言仅 1 块真实适配器（§13）
7. `collect.net.owner_pid`——TCP/UDP 表非空且 PID 可映射（普通权限可跑）
8. `collect.gdi.self_stable`——自身 gdiObjects/userObjects/handles 千帧稳定（泄漏哨兵，见附 C）

ops：
9. `ops.protected.refuse_all`——PID 0/4 + 名单逐名拒绝，ProtectedReason 非空（§11.6；用纯判定函数注入 name/path/parentChain，无需真进程）
10. `ops.tree.members_and_filter`——CollectTreeMembers 对真实树枚举+保护成员过滤（P2-11，不杀）
11. `ops.terminate.denied_propagates`——非提权杀 ACCESS_DENIED 目标：err 出参含可读原因，不崩溃（§11.6）
12. `ops.trim.self`——对自身 EmptyWorkingSet（R5：无害）后 WS 下降可观测
13. `ops.purge.honest_error`——非提权调 PurgeStandbyList 得诚实错误（不真清）
14. `ops.elevate.handshake_race`——双实例并发启动互斥+会话恢复（§13）
15. `ops.singleinstance.relaunch`——ERROR_CANCELLED 静默回退路径

core / UI / 阶段 3：
16. `core.str.unicode_roundtrip`——中文/全宽/`\\?\` 长路径（§12 阶段4）
17. `core.cfg.json_roundtrip`、`core.log.rotation`
18. `ui.headless_frames`——隐藏窗口 300 帧 D3D+ImGui+全页签（P2-8）
19. `startup.backup_before_disable`——禁用前备份文件存在且可还原（P1-4，阶段 3）
20. `service.pid_map`、`driver.enum_24h2`、`sensors.no_fake_data`——不可达态值域断言（阶段 3，P2-7）

## 附 C：资源自检清单（阶段 2/3 评审直接使用）

句柄类：
- [ ] 所有 HANDLE 持有走 RAII（unique_handle+traits：OpenProcess/CreateToolhelp32Snapshot/CreateEvent/OpenEvent/mutex），禁止裸 CloseHandle 散落
- [ ] 破坏性操作路径上的 OpenProcess 失败分支无泄漏（错误提前 return 处逐一核对）
- [ ] selftest 用例 8：自身句柄数在 N 次操作后回落基线 ± 常数

PDH 类：
- [ ] PDH_HQUERY/PDH_HCOUNTER 所有权归 Collector 对象，析构 PdhCloseQuery；重建策略成文（实例集变化超阈值或每 M tick 全量重建，逐 tick 只读）
- [ ] PdhExpandWildCardPath 展开的计数器数组与格式分配（PdhFreeCounterPathWords/CoTaskMemAff 视 API）配对释放
- [ ] PDH 错误路径不半初始化（Add 失败即关 query）

ETW 类：
- [ ] StartTrace/StopTrace 严格配对；会话名唯一常量；启动时先探测并 Stop 同名残留会话（防上次崩溃遗留的内核会话常驻）
- [ ] ExitInstance/崩溃兜底（SetUnhandledExceptionFilter 尽力 StopTrace）；process retirement 时 CloseTrace+释放缓冲
- [ ] ETW 关闭时 UI 标志位同步（P0-2.3 的 etwEnabled）

COM 类：
- [ ] 使用 COM 的线程（OpsWorker：ITaskService；传感器：WMI）各自 CoInitializeEx 并成对 CoUninitialize；明示 apartment 选择（建议 MTA）并写进线程职责表
- [ ] COM 接口指针 RAII（Microsoft::WRL::ComPtr 或等价自写），AddRef/Release 手写配对为零
- [ ] WMI 安全释放：IWbemLocator/Services/Enumerator/SetProxy 批次释放

UI/设备类：
- [ ] 退出顺序：清 ImGui backend → ImGui::DestroyContext → 释放 RTV/交换链缓冲 → Release device/context（顺序写进 D3DRenderer 注释）
- [ ] WM_SIZE/WM_DPICHANGED 重建 RTV 时旧 RTV 释放（置 nullptr 防悬挂）
- [ ] msyh.ttc 字体文件句柄/映射缓冲释放；FreeType 若引入同检
- [ ] selftest 千帧后 gdiObjects/userObjects 稳定（GetGuiResources 吃自己的狗粮）

特权类：
- [ ] SeDebug/SeProfileSingleProcess 启用/释放配对（core::Privilege RAII），日志记录每次启用（§6 已有，核对实现）

## 附 D：阶段 2 三模块边界与相互等待点

前置（架构师，开工第 0 天，全部入仓）：修 P0-1 头文件位置；契约头终版（P0-2 字段补齐）+ ProcessOps.h/Elevate.h + core 全部头（Log/Str/Err/Cfg/Time/FsUtil/Privilege）+ 附 D 的 JobQueue/NotificationQueue 类型（P1-5）。

| 模块 | 可立即开工 | 等待点 |
|---|---|---|
| collect | core 头 + ProcessData.h 终版 | 无（第 1 周先交 SnapshotStore+CollectService 骨架+selftest 1-8，供 app-ui 用真快照） |
| ops | core 头 + ProcKey（移入 core 后）+ ProcessOps.h | 不等 collect；自含父链重枚举（P0-3.2）；selftest 9-15 无需提权即可跑 |
| app-ui | ProcessData.h + ProcessOps.h + PageContext/IPage（P1-3，需架构师先落） | 等 collect 骨架（第 1 周）；此前用 `--mock-feed`（内嵌 2 份静态快照轮替，开发专用，不进 Release）解阻塞 |

集成会合点：第 1 周末（collect 骨架×app-ui 表格）、第 2 周（ops 终止×确认框）、第 3 周（提权链路端到端 00 §十）。

## 附 E：阶段 3 接口扩展点判定

- 现状：ServiceOps/StartupOps/DriverOps/NetPage/SensorsPage 接口"阶段 2 末补齐"——三个阶段 3 模块开工即空转，且届时补齐会打断阶段 2 收尾。建议：接口在**阶段 2 初给出草签**（空实现+TODO，仅签名），阶段 2 末冻结语义；阶段 3 agent 先按草签写页与测试桩。
- 页签注册：P1-3 的 IPage+静态注册数组必须阶段 2 落地（阶段 2 自身两个页即首个用户，验证机制），阶段 3 五个页零改动 AppContext。
- DetailsProvider 泛化：改 `DetailKind` 可扩展枚举 + `Get(ProcKey, kind, out)`；阶段 3 追加 servicesOnPid（P1-2）、每进程连接列表（NetPage 联动）、传感器行（SensorsPage 联动）不动既有结构。
- SensorsPage 专用：补最小 `SensorValue { enum State { Ok, NeedsAdmin, NeedsDriver, NoHardware }; double value; }` 契约草签，承接 P2-5 的三分法，杜绝阶段 3 自造显示规则。

（完）

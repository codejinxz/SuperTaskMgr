# 01 架构设计文档审查 V4 —— 技术正确性与可实现性

- 审查者：subagent V4（与其他审查者互不知情）
- 日期：2026-09-18
- 审查对象：《01_架构设计文档.md》（对照《00_技术选型评估报告 v2》《R5_Collection_APIs.md》）
- 维度：数据契约字段语义、线程模型与预算现实性、提权协议竞态、接口完备性、UI 排序/选中语义、验收可测性
- 结论：**总体架构可行，但有 2 个 P0 必须先修**（树杀数量矛盾、提权握手事件生命周期竞态），否则并行开发会按错误契约写出难排查的实现。

---

## P0 —— 阻断实现（必须先修契约）

### P0-1 树杀确认框需要"将终止的进程数"，但全接口没有任何枚举子进程的方法，且 `ops ↛ collect` 分层禁止 UI 绕道采集层（§7/§8/§2）

矛盾链条：§8 要求"树终止显示将终止的进程数"→ 计数需要子进程列表 → §7 只有黑盒 `bool TerminateTree(const ProcKey&, std::wstring*)` → UI 自己算就要枚举进程，但 §2 规定 `ops ↛ collect`，UI 也拿不到 Snapshot 之外的实时子进程（用 1Hz 旧快照计数，确认框与实际执行之间会漏算新生的子进程；且 UI 层实现树遍历会把 ops 职责抄进 app 层）。并行开发时 DetailPanel/ConfirmDialog 的开发者和 ProcessOps 的开发者会各写一份枚举逻辑。

**修法（两段式契约，写入 §7）**：

```cpp
// ops/ProcessOps.h 追加
namespace ops {
// Phase 1: 计划（非破坏性、快、走 OpsWorker）。内部用 NtQSI/Toolhelp 现拍一份进程快照
// （OS API 直调不违反 ops↛collect——该规则禁的是对 stm_collect 库的依赖，见 §2 修订文字）。
// 平坦 BFS 收集后代：同一快照内建 pid→ProcInfo 映射，child.parentPid→parent 且
// parent.createTime==root.createTime 才算真后代（防父 PID 复用误收，R5 §7c）。
bool PlanTerminateTree(const ProcKey& root, std::vector<ProcKey>* out, std::wstring* err);

struct TreeResult { int planned = 0, terminated = 0, failed = 0; std::wstring firstError; };
// Phase 2: 执行时重新快照（子进程可能已变化），leaf-first、逐个 (pid,createTime) 再验证，
// 保护名单子进程跳过并计入 failed 原因，不因此中止整树。
bool TerminateTree(const ProcKey& root, TreeResult* out, std::wstring* err);
}
```

配套修订三处文字：① §2 分层规则补一句"ops 层允许直接调用 Win32/NtQSI 等 OS API 自行做最小枚举，仅禁止 include/链接 stm_collect"；② §8 确认框文案改为"预计终止 N 个进程（执行时可能变化）"，N 来自 PlanTerminateTree；③ 原 `TerminateTree` 签名（返回裸 bool）作废，以 TreeResult 版本为准。

### P0-2 提权握手的事件对象有内核生命周期竞态 + 5s 等待违反 <1s 启动预算（§6/§10/§13）

现协议："旧实例写 session.json → SetEvent(RelaunchHandshake) → 退出；新实例等待（互斥体获取成功 && 事件已触发，5s 超时）"。三个确定性问题：

1. **命名事件对象会被销毁**。内核命名对象在最后一个句柄关闭时即被删除。时序必然是：旧实例 CreateEvent→SetEvent→ExitProcess（句柄关闭）→UAC 同意→新实例才首次运行→OpenEvent 返回 ERROR_FILE_NOT_FOUND。因为 `runas` 下新进程要等 UAC 同意才创建，**新实例不可能在旧实例退出前持住事件句柄**，"事件已触发"条件在正常路径上就永远无法满足——按此实现的开发者最终只能删掉事件判断，握手形同虚设且难排查。
2. **旧实例在 SetEvent 前崩溃** → 事件永不触发 → 新实例空等满 5s 才继续，直接违反 §10 "启动 <1s"。
3. 条件顺序本身含未定义分支：超时后新实例是作为第二实例退出，还是无状态恢复继续启动？契约未说。

**修法（去掉事件，状态完成性由文件本身承载，互斥体释放即"旧实例已终结"信号）**：

```text
旧实例：SaveSessionState(state)  // 原子写：同目录临时文件 + MoveFileExW(MOVEFILE_REPLACE_EXISTING)
        → ExitProcess（关闭互斥体句柄即隐式释放）
新实例：CreateMutexW(Local\SuperTaskMgr.SingleInstance)
        → ERROR_ALREADY_EXISTS 则 WaitForSingleObject(互斥, 最多 1000ms)；
          仍拿不到 → 弹"已有实例运行"后退出（不等待事件）
        → 拿到互斥体 ⇒ 旧实例必已退出（或从未存在/已崩溃）
        → TryLoadSessionState()：文件存在、可解析、且 ts 距今 <60s 才恢复；否则全新启动
        （全部同步路径，无 5s 等待，满足 §10 启动预算；崩溃/超时/文件损坏均自然降级）
```

§6 文字相应改写，§13 "提权重启竞态"缓解条同步更新（selftest 用例：写 session.json 后立即读回校验原子性；普通二次启动并发拿互斥）。session.json 增加字段 `"ts"`（写入口径：Unix 毫秒）与 `"kind":"elevate-relaunch"`。

---

## P1 —— 应改（随实现前或第一阶段修正）

### P1-1 ProcKey 定义在 `src/collect/ProcessData.h`，ops 层要用却禁止依赖 collect（§3 vs §2）
stm_ops 不链接 stm_collect，ProcKey 是 ops 全部接口的入参类型，按现目录结构 ops 只能 include collect 头（破分层）或复制结构体（发散风险）。**修法**：`ProcKey`（可含 `ProcFlag` 若 ops 需要）移到 `src/core/ProcKey.h`（架构师所有），ProcessData.h include 之；§3 目录图与 §4 注明。ProcInfo/Snapshot 等大结构仍留 collect。

### P1-2 "排序仅用户触发，不随数据自动重排"与逐 tick 数据更新矛盾，且 const Snapshot 无法就地排序（§8/§4）
两种解读会实现出两个东西：(a) 排序键仅用户设定，但**每份新快照到达时必须按当前键重新计算行序**（否则 CPU 列数据变了行序不跟着变，违背任务管理器直觉）；(b) 若按字面"不重排"，行序永远停在用户点击那一刻。且 `Snapshot.procs` 是 `shared_ptr<const>`，UI 根本不能对其 sort。**修法（写入 §8）**：用户点击列头仅设定 `ImGuiTableSortSpecs`；每个新 tickId 到达时对**行索引数组**执行 `std::vector<uint32_t> order` 重建 + `std::stable_sort`（比较用原始字段值而非格式化字符串；数值列直接比值；名称列用 `CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE|NORM_LINGUISTIC_CASING)`）；平局以快照基序（pid 升序）收尾保证确定性、防抖动；顺序=过滤→排序；排序在快照到达时做一次，不在每帧做。§8 那句话改为："排序键仅用户触发；行序随新快照按当前键重算（stable，pid 升序破平局）"。

### P1-3 选中行跟踪与 DetailPanel 联动语义未定义（PID 复用场景）（§8/§4）
契约缺三条规则，开发者必然用行号或裸 PID 当选中键，PID 复用后 DetailPanel 会静默显示成另一个进程（对"终止/树杀"入口是安全问题）。**修法（写入 §8）**：① 选中状态 = `ProcKey`；每帧在新快照中解析：pid 二分定位后校验 createTime（快照按 pid 升序正是为此服务）；② 解析失败 → DetailPanel 冻结展示最后已知数据 + 顶部"进程已退出"横幅，菜单中破坏性项禁用，直到用户改选或按 Esc；③ 确认对话框打开瞬间**锁定当时解析出的 ProcKey 副本**，确认执行的是锁定的 key，而非点击确认时的"当前选中行"（防数据刷新后张冠李戴）。

### P1-4 DetailsCache / DetailsProvider / NotificationQueue 载荷无契约，VerifyFileSignature 依赖链差最后一环（§4/§7/§5）
§4 说慢字段"live in DetailsCache"，§7 说 DetailsProvider"按需、缓存、走 OpsWorker"，但三者的结构、线程归属、结果回传通道全部未定义——并行开发 DetailPanel 与 OpsWorker 会对不上。依赖闭环本身是对的（UI façade → JobQueue → OpsWorker → ops::VerifyFileSignature，UI 不直接调 WinVerifyTrust），缺的是回程：JobQueue 有去无回，NotificationQueue 没有载荷定义。**修法（架构师补一个头）**：

```cpp
// src/app/ui/DetailsProvider.h（架构师所有）
struct ProcessDetails {
  ProcKey key;
  std::wstring cmdline, userName, companyName, fileDescription, version;
  uint32_t gdiObjects = 0, userObjects = 0;
  ops::SigState sig = ops::SigState::NoCheck; std::wstring sigDetail;   // 失败原因可展示
  std::vector<ThreadRow> threads;    // tid/startAddr/state/waitReason/kernelTime/userTime
  std::vector<ModuleRow> modules;    // 阶段 3 填充
};
class DetailsProvider {
public:
  void Request(const ProcKey& k);                     // 幂等去重；签名类 job 派发 OpsWorker
  const ProcessDetails* Peek(const ProcKey& k) const; // UI 线程调用，未就绪返回 nullptr（mutex 内 250ms 级读）
  void Invalidate(const ProcKey& k);
};
// NotificationQueue 载荷：struct JobResult { uint64_t jobId; JobKind kind; ProcKey key; HRESULT hr; std::wstring err; };
```

签名结果缓存以 path+size+mtime 为键（防文件被替换后旧结论）。`VerifyFileSignature` 建议加出参 `std::wstring* detail`（区分 catalog 签名/无签名/校验失败）。

### P1-5 ProcInfo 字段语义修正（§4）
- **cpuPercent**：注释"0..100 normalized to all cores"方向正确，但 R5 给的公式是 `Δ(kernel+user)/Δwall×核数`（每核算法，可>100），开发者照 R5 抄会差 N 倍。契约改为显式公式：`cpuPercent = Δ(kernelTime+userTime) / (tickSec × 逻辑核数) × 100`，首 tick（无除数）为 0，`tickId` 变化但 ProcKey 不连续（新进程）时首样本置 0，结果 clamp 到 [0,100]（定时器抖动可致 >100）。
- **diskBytesPerSec**：现注释未标口径。R5 明确 PDH `\Process(*)\IO Data Bytes/sec` = 文件+网络+设备总和，**不是纯磁盘**。字段名会诱导 UI 当磁盘展示。**修法**：注释改为"IO Data Bytes delta（文件+网络+设备总和，非纯磁盘；UI 列名'磁盘/IO'+tooltip 口径声明）"；且**阶段 2 建议不用 PDH 每进程计数器**，直接用 NtQSI 自带的 ReadTransferCount/WriteTransferCount/OtherTransferCount 差分（同口径、免通配符展开、免实例漂移处理），PDH 只留系统级 `\PhysicalDisk`。
- **privateBytes**：注释"=commit for most"不准。PrivateUsage=**私有提交**；总提交=私有+共享提交（后者需 EX2.SharedCommitUsage，1703+）。改为"privateBytes = PrivateUsage（私有提交；不含共享提交，UI 不得标注为'提交'）"。
- **PF_AccessDenied**：NtQSI 快照路径探测不到拒绝（不开句柄）。语义定为"粘滞标志：本会话内任一按需操作（详情/终止尝试）遇 ERROR_ACCESS_DENIED 时置位，不随 tick 清除"；快路径不置位。
- **Snapshot 增加降级标志**：§4 要求 UI 显示"兼容模式"，但 Snapshot/SystemInfo 无字段。加 `bool degraded = false; std::wstring degradeReason;`（自校验门失败时置位）。
- 快照首 tick 的所有差分字段（cpu/disk/ctxsw/faults）=0，注释明示"无除数置 0，非真实速率"。

### P1-6 ProcKey 过期（进程已死/PID 复用）的再验证未写入 ops 契约（§7）
`TerminateProcessById(const ProcKey& k, ...)` 若实现者只 OpenProcess(pid) 就杀，PID 复用窗口内会杀错进程——这正是 ProcKey 存在的意义，契约却没把它变成义务。**修法（写入 §7 每个带 ProcKey 的函数）**："必须再验证身份：OpenProcess(pid) → GetProcessTimes 取 createTime → 与 key.createTime 比对（容差 ±1s，吸收 NtQSI/GetProcessTimes 舍入差；自校验门同口径）；不匹配 → 返回 false，err='进程已退出或 PID 已被复用'（专用 HRESULT 区分，供 UI 免提权提示）。TrimWorkingSet 同规则。"

### P1-7 15ms tick 预算：NtQSI+PSAPI 部分成立，PDH 实例漂移与 GPU 通配符是两个未闭环变量（§5/§10 vs R5）
量级核算：NtQSI 全量一次 0.1–1ms（R5#1）含 CPU/IO/句柄/线程/父链/WS/PrivatePageCount，快路径无需逐进程 OpenProcess；PDH 系统级（PhysicalDisk/Memory/GPU Adapter）每 tick CollectQueryData 合计约 1–3ms。**成立**，前提是采纳 P1-5（每进程磁盘走 NtQSI 差分）。风险点：① PDH `\Process(*)`/`\GPU Engine(*)` 实例集随进程生灭每 tick 漂移，R5#9b 证实"不存在实例也返回 SUCCESS"（陈旧实例零值坑），通配符重展开成本 R5 表B#8 明言未验证；GPU Engine 实例数≈进程×引擎，500 进程时未知。**修法（写入 §5）**：GPU/每进程类 PDH（若保留）以 2s 节奏采集并复用展开结果，仅在 pid 集变化超阈值时重展开；15ms 预算按"NtQSI+系统级 PDH"口径考核，GPU 不计入 tick 门禁（单列预算行）。

### P1-8 RelaunchAsAdmin 返回语义与 SingleInstance/SessionState 契约整体缺失（§7）
§7 只有一行 `bool RelaunchAsAdmin(const std::wstring& args)`，而 §6 协议 + §11.2 验收都依赖它和会话状态保存/恢复。**修法（补 §7）**：

```cpp
// ops/Elevate.h
// 返回 true = 新实例已启动，调用方必须尽快保存状态后退出；false = 用户取消/失败，调用方继续运行（静默回退）。
bool RelaunchAsAdmin(const std::wstring& args);
// ops/SingleInstance.h（新增，架构师所有）
struct SessionState { int tab; ProcKey selected; int sortCol; bool sortAsc; int winX, winY, winW, winH; uint64_t tsMs; };
bool AcquireSingleInstance(std::wstring* alreadyRunningMsg);  // Local\ 互斥体；见 P0-2 新流程
bool SaveSessionState(const SessionState&);   // 原子写（temp+MoveFileExW REPLACE_EXISTING）
bool TryLoadSessionState(SessionState*, uint64_t maxAgeMs);   // 仅在持互斥后调用；过期/损坏返回 false
```

### P1-9 JobQueue 串行饥饿：结论 = 阶段 2 可接受，但必须加两道护栏（§5/§7）
场景核算：破坏性操作均为 1 调用级（µs–ms）；唯一慢操作是 WinVerifyTrust（ms 级–秒级，若走网络吊销则更久），会阻塞排在后面的"终止"——用户感知为"确认后无响应"。单人工具 + FIFO 串行可以接受，**前提**：① 签名校验仅接受本地固定盘路径（`GetDriveTypeW==DRIVE_FIXED`；UNC/可移动盘返回 `SigState::NoCheck`+原因"不支持的网络/可移动路径"），杜绝网络超时占住 OpsWorker；② 同路径去重（缓存未命中才入队）；③ UI 对排队中 job 显示"排队/执行中"状态（NotificationQueue 已有通道）。若阶段 3 出现更多秒级操作（服务启停）再考虑分车道，本期不做。

### P1-10 自校验门覆盖面不足（§4）
门只比对"CPU/IO 时间 + 内存"未覆盖的偏移恰恰也在 Reserved 区：IO 字节数、HandleCount、线程数组（ContextSwitches/ThreadId/状态）偏移错了一样出假数据且 UI 无法察觉。**修法**：门扩展为五项交叉比对——CreateTime/内核用户时间↔GetProcessTimes；WS/PrivatePageCount↔GetProcessMemoryInfo；IO 读写字节↔GetProcessIoCounters；HandleCount↔GetProcessHandleCount；线程数与 TID 集合↔Toolhelp 抽 3 进程。任一超差整体降级（不做逐字段降级，避免半对半错更难排查）。

---

## P2 —— 建议

1. **三态可见性**：`netBytesPerSec`/`gdiObjects`/`userObjects` 用 0 同时表示"未测"和"真 0"，与 §8"绝不显示 0 冒充"冲突。建议 ProcInfo 增加一个 `uint32_t validMask`（或文档明确 UI 依据配置推断）。§11.1"≥13 列全量数据"与 §8 列清单（11 列）对不上，且阶段 2 无 ETW 时网络列必然全"—"，验收措辞改为"13 列（网络列阶段 2 显示 —）"。
2. **`\Memory\Hard Faults/sec` 路径未实证**：R5 表A#15 未列此计数器；实现时 PDH browse 确认，拿不到则降级 `\Memory\Pages Input/sec`（经典硬故障入页），§11.3 的"Hard Faults 抖动"验收随之标注口径。
3. **perCorePercent**：NtQSI SystemProcessorPerformanceInformation 的 KernelTime 含 Idle（同 GetSystemTimes，R5#15），必须减 Idle 后差分；写入 SystemCollector 实现注释与自校验门比对项。
4. **慢路径字段诚实显示**：Toolhelp 无 CPU/IO/句柄/上下文切换，§4 应注明慢路径下这些列显示"—"（现文档只说"仅保基础枚举"，UI 侧易漏）。
5. **采集间隔/暂停控制原语**：三种同步原语不含 UI→采集线程的控制通道。明确"间隔/暂停用 relaxed atomic 变量传递，不属于第四种阻塞原语"，防止开发者自造 condvar。
6. **`Local\` 命名空间语义**：按 LSA 登录会话隔离。同会话 runas 提权（默认同账户）没问题；若 runas 换了别的管理员账户 → 新登录会话 → 互斥体互不可见 → 同桌面出现双实例；且提权实例读不到对方 %LOCALAPPDATA% 的 session.json（良性，走全新启动）。文档化该取舍即可（选 `Global\` 会误伤多用户 RDP 场景，维持 `Local\` 合理）。
7. **windowTitle 节奏矛盾**：00 报告说"按需"，§4 把它列进每 tick 的 ProcInfo。建议定案：每 tick 由 EnumWindows 映射一遍（1Hz 成本可接受），或仅对选中/可见行取；二选一写入契约。
8. **日志口径**：§10"自身磁盘写 ≤1MB/h"与 §9"滚动 3×1MB"单位不一致，统一为"3×1MB 滚动上限 + 稳态写速 ≤1MB/h"。
9. **挂起/UWP 容器终止**（R5 表B#11）：阶段 2 在确认框对 Suspended 标志进程加一行风险提示，实测项留阶段 3。
10. **服务进程先停服再杀**（00 §五基线）：§7 无体现，建议注明"阶段 2 Terminate* 仅 TerminateProcess；服务感知终止随 ServiceOps（阶段 3）"，防开发者自行发挥 ControlService。
11. **26100+ SequenceNumber**：00 报告承诺前向兼容，ProcKey 无此字段。可接受，但契约加注释"未来版本 ProcKey 可能增补 sequence 字段，实现不得假设 sizeof(ProcKey)==12 做序列化"。
12. **图表窗口**：间隔可调（500–5000ms）时"120 点"不再是 120s；建议改"120s 时间窗、按当前间隔折算点数"。
13. **模态/交互期间抑制重排**：确认对话框可见或用户正拖动列宽/滚动时暂缓行序重算，下一 tick 恢复（防误点错行）。确认框本身已由 P1-3 锁定 ProcKey，此项为体验加固。

---

## 审查过但无问题

| 项 | 核对结论 |
|---|---|
| ProcKey=(pid,createTime) 身份设计；createTime 来源 | 成立。SYSTEM_PROCESS_INFORMATION 布局含 CreateTime（R5#1/#2 亦以此为身份基准）；Idle(0)/System(4) 的 createTime 为 0/boot 值，不影响（其在保护名单）。自校验门 1s 容差恰好覆盖 NtQSI vs GetProcessTimes 舍入差 |
| SnapshotStore mutex+shared_ptr @1Hz | 成立。临界区仅 shared_ptr 拷贝（µs 级）；写者单线程；UI 持旧快照期间引用计数保活，无 ABA。每 tick 新分配几十 KB 无压力 |
| 三线程划分与预算骨架 | UI/采集/操作职责正交；"UI 永不阻塞"由 JobQueue+NotificationQueue 达成（载荷契约缺失另计 P1-4） |
| pageFaultsPerSec 语义 | PageFaultCount 为累计软+硬缺页（文档化），差分口径正确；与 SystemInfo.hardFaultsPerSec（系统级硬故障）区分清楚 |
| handles/threads/gdi/user 字段 | NtQSI 直供 HandleCount/线程数；GDI/USER 走 GetGuiResources 按需（QLI），权限标注正确 |
| 提权功能×权限矩阵（§6 表） | 与 R5/00 的权限事实一致（OWNER_MODULE、24H2 EnumDeviceDrivers、HKLM 写、PPL 行为） |
| SeDebugPrivilege 按需启/用后即释 + 日志审计 | 与 00 §六一致，无新风险 |
| 保护名单双层防线 + ProtectedReason | PID 0/4 + 名称/路径/父链识别 smss/csrss/wininit/winlogon/services/lsass/Registry/Memory Compression/dwm 与 R5#7d 实测吻合（Registry 的 PID 220 是本机启动值，名单按名称识别正确，未写死 PID） |
| WinVerifyTrust 用法（VERIFY 配 STATEACTION_CLOSE、catalog、缓存、吊销离线） | 与 R5#5 一致（路径护栏见 P1-9） |
| EmptyWorkingSet 需 PROCESS_SET_QUOTA；PurgeStandbyList 需 SeProfileSingleProcessPrivilege | 与 R5#8/#8b 一致；"诚实说明代价"符合硬约束 |
| 错误/日志/配置（HRESULT 统一、err 出参、禁跨边界异常、tick 不逐条日志） | 自洽可测 |
| manifest asInvoker + PerMonitorV2 + longPathAware | 与 00 §六一致 |
| 目录结构/CMake 4 target/分层依赖方向 | 除 P1-1 的 ProcKey 归属外自洽；third_party /W0 隔离合理 |
| §10 预算表与 §11 验收对应 | 除 P0-2 启动 <1s vs 5s 等待、P2-1/2/8/12 列出的措辞与计数器口径外，各项均有可执行测法（selftest p50/p95、帧计时叠加、TM 对照） |
| msyh.ttc 运行时加载（不可再分发）与 1.92 动态字体 | 与 00 §七许可结论一致 |
| 120 点@1Hz ImPlot 滚动窗口、暗色默认主题 | 与 00/§8 一致（点数口径见 P2-12） |

---

## 统计

- P0：2（树杀数量矛盾 / 提权握手事件生命周期竞态）
- P1：10（P1-1 ~ P1-10）
- P2：13
- 审查过且无问题：15 项

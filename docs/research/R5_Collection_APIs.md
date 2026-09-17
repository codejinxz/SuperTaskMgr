# R5 调研报告：数据采集层 API 逐项查证（Windows 10 21H2+/11 x64）

**一句话结论**：普通权限可覆盖约 85% 采集（NtQuerySystemInformation 全量枚举 + PSAPI/PDH/iphlpapi），提权只为 SeDebugPrivilege（24H2 驱动枚举/跨会话查询）、服务启停、ETW 内核会话、OWNER_MODULE 完整模块名而存在；最危险的不是权限而是"半文档化字段偏移"与"PDH 通配符/实例名"两大坑。
本机实测环境：Win11 23H2 (22631)、zh-CN、16 逻辑核、非管理员。【本机实测】标注项均在本机运行验证。

## 表A：功能 → API → 状态/权限/开销/版本/坑

| # | 功能 | 候选 API | 文档化状态 | 所需权限/特权 | 单次开销量级 | 最低版本 | 已知坑 |
|---|------|----------|-----------|---------------|-------------|----------|--------|
| 1 | 进程枚举(全量) | NtQuerySystemInformation(SystemProcessInformation=5) | 半文档化：函数+结构布局已上 Learn(2025 更新)但页首警告"可能变更"，CPU/IO 时间字段在 Reserved1[48]/Reserved7[6] 内未文档化 | 无特权，普通用户可调 | 一次调用全量，~0.1-1ms、数百 KB 缓冲(按 ReturnLength 重试) | XP+ | 须 LoadLibrary 动态加载；CPU/IO 偏移靠 NtDoc/GeoffChappell；Win11 26100.4770+ 另有 SystemBasicProcessInformation(带 SequenceNumber 防PID复用) |
| 1b | 进程枚举(兜底) | CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) | 文档化 | 无特权 | ms 级，快照拷贝 | XP+ | 仅 PID/父PID/线程数/EXE 名；无 CPU/IO/句柄数；快照非原子一致 |
| 1c | 进程枚举 | K32EnumProcesses | 文档化 | 无特权 | 最轻，仅 PID 数组 | XP+ | 只有 PID，其余信息需二次打开进程 |
| 2 | CPU% | GetProcessTimes 或 NtQSI 快照差分 | 公式为工程共识；GetProcessTimes 文档化 | QLI(文档化) | 每进程 1 次系统调用 | XP+ | 公式=Δ(内核+用户)/Δ墙钟×逻辑核数；首样本无除数；PID 复用须以 CreateTime(NtQSI)/GetProcessTimes 为进程身份；>100% 显示口径自选(×核 或 Task Manager 式归一化) |
| 3 | 内存三项(WS/私有/提交) | GetProcessMemoryInfo + PROCESS_MEMORY_COUNTERS_EX(.PrivateUsage)/EX2(.SharedCommitUsage) | 文档化 | QLI(Win7+；2003 需 QI+VM_READ) | 每进程 1 调用，~µs 级 | XP(EX2=Win10 1703+) | WorkingSetSize=工作集，PrivateUsage=私有提交(≈提交大小)；PPL 进程 QLI 仍可打开可读 |
| 4 | 映像路径 | QueryFullProcessImageNameW(winbase.h) | 文档化 | QLI 即可；中等 IL 对提权进程亦可 | 每进程 1 调用 | Vista+ | PPL/受保护进程非管理员下失败(本机实测 Get-Process.Path 为空)；兜底：NtQSI ImageName 免句柄必得名字；SystemProcessIdInformation(88/0x58) 免句柄取全路径=未文档化 |
| 4b | UWP 包名 | GetPackageFullName(appmodel.h) | 文档化 | QLI | 每进程 1 调用 | Win8+ | 仅 UWB/打包进程有意义，Win32 进程返回失败 |
| 5 | 签名校验 | WinVerifyTrust + WINTRUST_DATA | 文档化：VERIFY/CLOSE 配对为结构页原文("must be specified for every use of VERIFY") | 无特权 | 每文件 ms 级；首次可含证书链/吊销检查 | XP+ | 必须配 WTD_STATEACTION_CLOSE 否则泄漏；系统文件多为 catalog 签名→用 WTD_CHOICE_CATALOG+WINTRUST_CATALOG_INFO(文档化)；建议 WTD_UI_NONE+控制吊销网络；批量校验须自建结果缓存 |
| 6 | WOW64 判定 | IsWow64Process2 | 文档化 | QI 或 QLI | 每进程 1 调用 | Win10 1709+ | 非 WOW64 时 pProcessMachine=IMAGE_FILE_MACHINE_UNKNOWN；比 IsWow64Process 多给进程/宿主架构 |
| 6b | PPL 可见性 | 枚举可见、句柄受限 | 文档化(process-security-and-access-rights)：对受保护进程不可授予 PROCESS_TERMINATE/VM_READ 等 | — | — | Win8.1+ | lsass 等可枚举(名/父链可查)但 OpenProcess 高权位 ACCESS_DENIED——天然防误杀 |
| 7 | 终止进程 | TerminateProcess | 文档化 | PROCESS_TERMINATE；跨权限进程需启用 SeDebugPrivilege(LookupPrivilegeValue+AdjustTokenPrivileges，文档化流程) | 1 调用 | XP+ | PPL/关键进程失败 ERROR_ACCESS_DENIED；杀 csrss/wininit 类触发系统崩溃(工程共识)；服务进程"正确终止"=ControlService 停服后再杀 |
| 7b | PID→服务 | QueryServiceStatusEx(SC_STATUS_PROCESS_INFO)→SERVICE_STATUS_PROCESS.dwProcessId；批量用 EnumServicesStatusEx(SERVICE_WIN32,SC_STATUS_PROCESS_INFO) | 文档化 | SERVICE_QUERY_STATUS(普通用户可) | 全服务 1 次批量调用 | XP+ | dwProcessId 仅在 RUNNING/PAUSE_PENDING/PAUSED/CONTINUE_PENDING 有效，STOPPED 永远无效(文档原文)；仅 SERVICE_WIN32 有 PID，驱动无 |
| 7c | 树杀 | 父链快照自底向上(NtQSI InheritedFromUniqueProcessId/Toolhelp) 或 Job 对象(IsProcessInJob/AssignProcessToJobObject/TerminateJobObject) | 均文档化；组合策略为工程共识 | 同上 | 快照 O(N) | XP+(嵌套 Job=Vista+) | Job 收编已有树可能撞上进程已在 Job(嵌套 Job 缓解)；父已死时父 PID 会被复用→必须配合进程身份(CreateTime)核对；先孩子后父亲防重生 |
| 7d | 误杀防护 | 黑名单(PID 0,4 + Registry/Memory Compression/smss/csrss/wininit/winlogon/services/lsass/dwm) + 父链(PID 4) + PPL 拒绝 | 名单为工程共识(无官方清单)；PPL 行为文档化 | — | — | — | 本机实测：Idle=PID0、System=PID4、Registry=220、Memory Compression 可见但 Path 空；dwm 可杀会自动重启；csrss 双实例(会话0+会话1) |
| 8 | 内存清理(单进程) | EmptyWorkingSet / SetProcessWorkingSetSize(h,-1,-1) | 文档化 | QI 或 QLI + PROCESS_SET_QUOTA | 每进程 1 调用 | XP+ | 代价：工作集页被换出至 standby/pagefile，进程再次访问触发大量缺页→可能引发磁盘 IO 高峰(文档未量化，工程共识：对大 WS 进程慎用，对自身 -1,-1 无害) |
| 8b | 清理待机列表 | NtSetSystemInformation(SystemMemoryListInformation=0x50, MemoryPurgeStandbyList=4) | 未文档化(Learn 仅有稀疏 NtSetSystemInformation 页；枚举值社区/NtDoc) | SeProfileSingleProcessPrivilege(须管理员启用) | 1 调用，瞬时 | Vista+ | 只清 standby 缓存(文件缓存/可回收页)，不清进程私有内存；效果=可用内存瞬时回升，之后重读文件会再次慢(诚实表述)；命令值：4=MemoryPurgeStandbyList，5=MemoryPurgeLowPriorityStandbyList |
| 9 | 按进程磁盘(近似) | PDH \Process(*)\IO Data Bytes/sec + \Process(*)\ID Process 映射 | 计数器半文档化(Learn 无独立对象页；MSSQLSERVER_833 页引用"Process: IO Data Bytes/Sec") | 无特权 | 每秒轮询，1 查询全实例 | Vista+ | 注意：这是文件+网络+设备 IO 总和，非纯磁盘；实例名有 #N 后缀歧义→用 ID Process 计数器映射 PID(本机实测 Add 成功)；PdhAddEnglishCounterW 对通配符路径不展开(文档化)，须 PdhGetCounterInfo→PdhExpandWildCardPath→逐实例 Add |
| 9b | 本地化对策 | PdhAddEnglishCounterW | 文档化(明细：PdhAddCounter 路径必须本地化，English 版语言中立) | 无特权 | 同上 | Vista+ | 【本机实测】zh-CN 22631 下全部 11 条英文路径 Add 成功且采集成功；本机 Perflib 仅有 009 无 2052 表→本机名即英文名；但代码仍须用 English API 保证其他机器；不存在实例时也返回 ERROR_SUCCESS(陈旧实例坑) |
| 9c | 按进程磁盘(精确) | ETW 内核会话(Microsoft-Windows-Kernel-File/DiskIo) | 文档化(ETW 框架) | 启动内核/系统记录器会话需管理员(SeSystemProfilePrivilege) | 每 IO 一事件，高频；需解析缓冲 | Vista+ | 仅在提权实例启用；事件量大须配缓冲与过滤 |
| 10 | 按进程 TCP/UDP | GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)/GetExtendedUdpTable(UDP_TABLE_OWNER_PID) | 文档化 | 普通权限可 | 每次全表，~ms | Vista+(IPv4 BASIC 表 XP) | 【本机实测】netstat -ano(=OWNER_PID) 非管理员成功；OWNER_MODULE 类+GetOwnerModuleFromTcpEntry：文档原文——非管理员调用成功但系统保护连接(System32 内)的模块名/路径返回空串，需管理员+manifest requireAdministrator 才完整(netstat -anob 本机实测"请求的操作需要提升"佐证) |
| 10b | 按进程流量 | ETW Microsoft-Windows-Kernel-Network | 文档化(Provider)；无 per-process PDH 网络计数器 | 管理员 | 每 send/recv 一事件，量最大 | Vista+ | 粗替代：Process IO Data Bytes/sec 增量(含磁盘，需标注口径)；建议提权实例才开 |
| 11 | 按进程 GPU | PDH \GPU Engine(*)\Utilization Percentage(engtype_3D 等) + \GPU Process Memory(*)\Dedicated/Shared Usage | 半文档化：计数器存在于 Perflib，实例名格式(pid_<pid>_luid_.._phys_..)未正式文档化 | 无特权 | 每秒轮询全实例 | Win10 1803+(PerfLib 见 GPU Engine) | 【本机实测】22631 非管理员下 Utilization Percentage、Dedicated/Shared Usage 全部 Add 成功；同一 PID 多引擎多实例需聚合；实例名解析取 pid 字段映射 |
| 12 | 服务管理 | EnumServicesStatusEx+QueryServiceConfig；StartService/ControlService；EnumDependentServices | 文档化 | 读：SERVICE_QUERY_STATUS/CONFIG(普通可)；启停：SERVICE_START/STOP(多数系统服务需管理员) | 枚举 1 次全量 | XP+ | svchost 共享进程：同组服务 dwProcessId 相同即映射；svchost -k 分组表在 HKLM\...\CurrentVersion\Svchost(未文档化但稳定)；停服务须先递归停依赖(EnumDependentServices 文档化模式) |
| 13 | 驱动列表 | EnumDeviceDrivers + GetModuleFileNameEx(基址→路径) | 文档化 | XP-23H2 普通权限；**Win11 24H2 起文档化要求 SeDebugPrivilege，否则调用成功但地址全 NULL(文档原文)** | 1 调用全量，~ms | XP+ | 24H2 变化必须处理(降级提示"需提权"或走 NtQSI SystemModuleInformation=未文档化)；驱动签名校验同 #5(catalog) |
| 14a | 启动项-注册表 | HKLM/HKCU\Software\Microsoft\Windows\CurrentVersion\Run(+RunOnce)；64 位下另读 HKLM\Software\WOW6432Node\... | 键路径文档化(Run and RunOnce Registry Keys 页)；Wow6432Node 重定向为工程共识 | HKLM 写需管理员；HKCU 普通 | 注册表查询，µs | 全版本 | HKLM RunOnce 仅管理员登录执行(文档原文)；禁用状态在 Explorer\StartupApproved\Run(见 14a2) |
| 14a2 | 启动项禁用态 | StartupApproved\{Run,Run32,StartupFolder} 二进制值 | **未文档化**(官方无页)；社区一致+本机佐证 | HKCU 普通/HKLM 管理员 | — | Win8+ | 12 字节：首 DWORD 小端，低字节奇=禁用(0x03/0x07)、偶=启用(0x02/0x06)；字节4-11=状态变更时刻 FILETIME。【本机实测】doubao=03000000E39979939A18DB01(0x03=禁用+2026 年 FILETIME)吻合；写入禁用值后的 Explorer 兼容性→表B |
| 14b | 启动文件夹 | SHGetKnownFolderPath(FOLDERID_Startup / FOLDERID_CommonStartup) | 文档化 | 普通 | µs | Vista+ | 【本机实测】%APPDATA%\...\Startup 与 C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Startup；禁用态同样记在 StartupApproved\StartupFolder |
| 14c | 启动项-计划任务 | ITaskService→GetFolder("\\")→GetTasks(TASK_ENUM_HIDDEN)；LogonTrigger 过滤 | 文档化 | 枚举普通用户可；修改他人/系统文件夹任务需管理员 | COM 枚举，百级任务 ~10ms | Vista+ | RegisteredTask.Enabled 文档标注 **Read/write**(脚本对象)；C++ IRegisteredTask 是否有 put_Enabled 待验证(否则取 Definition 改 Settings 后 RegisterTaskDefinition 重注册)；也可 schtasks /Change /DISABLE |
| 14d | 启动项-UWP | HKCU\Software\Classes\Local Settings\...\AppModel\SystemAppData\<PFN>\<TaskId>\State | **未文档化**；对应 UWP 枚举 StartupTaskState(Disabled=0/DisabledByUser=1/Enabled=2/DisabledByPolicy=3/EnabledByPolicy=4) API 文档化但注册表映射非官方 | 普通 | — | Win10+ | 【本机实测】87 包中 1 条：Windows Terminal StartTerminalOnLoginTask State=0(未启用)吻合枚举值；直接改 State 是否生效待验证 |
| 15 | 系统总量 CPU/内存/磁盘 | GetSystemTimes；GlobalMemoryStatusEx；PDH \PhysicalDisk(*)\Disk Bytes/sec、\LogicalDisk(*)\% Free Space | 文档化 | 无特权 | 1 调用/µs | XP+(PDH Vista+) | 【本机实测】PDH 英文路径 Add 成功；GetSystemTimes KernelTime 含 Idle(算总 CPU% 时要减或用 IdleTime)；% Disk Time 语义可>100%慎用；dwMemoryLoad 为百分比快照 |

## 表B：待验证清单（阶段 1 需写代码裁决）

| # | 待验证项 | 裁决方法 |
|---|---------|---------|
| 1 | StartupApproved 写入 odd DWORD 禁用后任务管理器/Explorer 是否正确识别；时间戳 8 字节可否置 0；Run32(32位)/StartupFolder 子键行为差异 | 写 HKCU 值→开任务管理器观察开关状态→还原 |
| 2 | UWP StartupTask State 全枚举(0-4)语义与直接改写的生效性 | 对照 UWP StartupTaskState 枚举；改 State=2 后重启观察 |
| 3 | NtQSI SYSTEM_PROCESS_INFORMATION 中 CPU/IO 时间社区偏移在 21H2-23H2 x64 的正确性 | 与 GetProcessTimes/GetProcessIoCounters 逐进程交叉比对 |
| 4 | SystemProcessIdInformation(88/0x58) 免句柄取路径：对 PPL/系统进程是否可行、缓冲两次调用协议 | 小程序对 lsass/smss/dwm 实测 |
| 5 | IRegisteredTask(C++) 是否有 put_Enabled；无则重注册禁用流程；非管理员枚举 \Microsoft 文件夹的 Access Denied 范围 | COM 代码实测 |
| 6 | EnumServicesStatusEx 对 START/STOP_PENDING 服务 dwProcessId 无效性；"先停服务再杀树"顺序与超时 | 启停一个可选服务实测 |
| 7 | EmptyWorkingSet 对大 WS 进程的缺页/IO 代价；MemoryPurgeStandbyList 在 SeProfileSingleProcessPrivilege 启用后非管理员 token 是否可调 | 性能计数器(缺页/s)+一次性实测 |
| 8 | GPU Engine 实例通配符展开成本(800 进程时实例数)与多 GPU/LUID 聚合；无 GPU/远程会话时计数器缺失降级 | PdhExpandWildCardPath 压测 |
| 9 | ETW 内核会话(Kernel-Network/磁盘)非管理员启动的失败码；最小缓冲与事件率实测 | StartTrace 两种权限各跑一次 |
| 10 | PID 复用竞态：快照→OpenProcess 间进程退出的处理；以 CreateTime 为身份的比对逻辑；26100+ SequenceNumber 前向兼容 | 每秒轮询 24h 无异常 |
| 11 | TerminateProcess 对 UWP 容器进程(ApplicationFrameHost 等)与挂起进程的副作用 | 实测+黑名单补充 |
| 12 | typeperf -q 在本机仅 2812 行且无 Process/GPU 对象(本机实测)，枚举计数器须走 PdhExpandWildCardPath 或注册表 009 索引——根因待查 | 对照 PdhExpandWildCardPath 输出 |
| 13 | GetPackageFullName 所需访问权与对 Win32 进程的错误码 | 实测 |
| 14 | Memory Compression 是否出现在 Toolhelp32 快照(本机实测 Get-Process/NtQSI 可见) | 双 API 对照 |

## 要点备注

1. **架构主线**：普通权限层用 NtQSI(SystemProcessInformation) 每秒 1 次全量快照(枚举+CPU+内存+句柄+线程+父链一次拿全)，免 800 次 OpenProcess；PSAPI/PDH/iphlpapi 补细节。提权单实例只做：SeDebug(24H2 驱动枚举/跨会话)、服务启停、ETW、StartupApproved HKLM/计划任务系统文件夹/UWP 之外的写入。
2. **CPU% 三坑**：首样本除零；PID 复用→以 CreateTime 为身份(NtQSI 内嵌，Win11 26100.4770+ 官方提供 SequenceNumber)；>100% 显示口径需产品决策。
3. **PDH 是本地化重灾区但已被官方 API 化解**：全程 PdhAddEnglishCounterW；通配符必须手动展开(PdhGetCounterInfo→PdhExpandWildCardPath→逐实例 Add)；不存在实例也返回成功→需按 ID Process 计数器重新映射实例↔PID。本机 22631 zh-CN 无 2052 本地化表(仅 009)，typeperf -q 输出不全，均以实测为准。
4. **诚实声明**：PDH "IO Data Bytes/sec" 与 GetProcessIoCounters 均为文件+网络+设备总和，进程级"纯磁盘"只有 ETW 精确；MemoryPurgeStandbyList 只清 standby 缓存，不清理进程内存。
5. **Win11 24H2 两大变化**：EnumDeviceDrivers 无 SeDebugPrivilege 返回全 NULL(文档化)；SystemBasicProcessInformation/SequenceNumber 新增(文档化)——目标 21H2-23H2 但必须前向兼容。

## 参考链接

- NtQuerySystemInformation(含 SYSTEM_PROCESS_INFORMATION 布局/SystemBasicProcessInformation)：https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation
- CreateToolhelp32Snapshot：https://learn.microsoft.com/en-us/windows/win32/api/toolhelp32/nf-toolhelp32-createtoolhelp32snapshot ；EnumProcesses：https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumprocesses
- GetProcessMemoryInfo：https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo
- QueryFullProcessImageName：https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamea
- 进程安全与访问权(含 PPL 限制)：https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights
- WinVerifyTrust：https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust ；WINTRUST_DATA(VERIFY/CLOSE/CATALOG)：https://learn.microsoft.com/en-us/windows/win32/api/wintrust/ns-wintrust-wintrust_data
- IsWow64Process2：https://learn.microsoft.com/en-us/windows/win32/api/wow64apiset/nf-wow64apiset-iswow64process2
- TerminateProcess：https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess ；特权启用：https://learn.microsoft.com/en-us/windows/win32/secauthz/enabling-and-disabling-privileges-in-c--
- QueryServiceStatusEx(dwProcessId 有效性)：https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-queryservicestatusex ；EnumServicesStatusEx：https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-enumservicesstatusexa
- SetProcessWorkingSetSize：https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-setprocessworkingsetsize ；EmptyWorkingSet：https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-emptyworkingset ；NtSetSystemInformation(稀疏)：https://learn.microsoft.com/en-us/windows/win32/sysinfo/ntsetsysteminformation
- PdhAddEnglishCounterW(含通配符流程)：https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw
- GetExtendedTcpTable：https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedtcptable ；GetOwnerModuleFromTcpEntry(非管理员空串)：https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getownermodulefromtcpentry
- EnumDeviceDrivers(24H2 SeDebugPrivilege)：https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumdevicedrivers
- Run/RunOnce 键：https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys ；Known Folders：https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid
- RegisteredTask(Enabled Read/write)：https://learn.microsoft.com/en-us/windows/win32/taskschd/registeredtask ；StartupTaskState 枚举：https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.startuptaskstate
- GetSystemTimes：https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes ；GlobalMemoryStatusEx：https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex
- 社区佐证：StartupApproved 位含义 https://stackoverflow.com/questions/78451156 与 http://windowsir.blogspot.com/2022/07/startupapprovedrun-pt-ii.html ；NtDoc https://ntdoc.m417z.com/ntquerysysteminformation

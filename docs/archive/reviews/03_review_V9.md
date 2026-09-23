# 阶段 3 审查报告 —— V9（正确性 + 安全）

- 审查人：V9（独立审查，与其他审查者互不知情）
- 范围：`src/ops/ServiceOps.cpp`、`src/ops/StartupOps.cpp`、`src/ops/DriverOps.cpp`（对照同名 .h）；`src/collect/NetTables.cpp`（含 EtwNetCollector）、`src/collect/Sensors.cpp`、`src/collect/CollectService.cpp` ETW 接线；对照 `docs/phase/01_架构设计文档.md` §6/§7/§9 与 `docs/research/R5_Collection_APIs.md`（14a-14d）。
- 方法：五个目标文件全文精读（含契约头、core/ProtectedList、HandleGuard、FsUtil、Err 交叉核对）+ RegisterTaskDefinition 官方文档核对 + 运行既有产物：`stm_selftest.exe` **40/40 通过**（非管理员沙箱）、`SuperTaskMgr.exe --smoke 120` 退出码 0。未构建、除本报告外未修改任何文件。

## 统计

| 级别 | 数量 |
|---|---|
| P0 | 3 |
| P1 | 2 |
| P2 | 10 |

注：三个 P0 全部集中在 Sensors.cpp 的磁盘/温度读数路径——`sensors_honest` 自测断言过弱（只断言 Ok⇒值>0），恰好放行全部三处（垃圾值被合理性钳位抑制为 NoHardware、幻影盘 health 非空），故 40/40 与本结论不矛盾。

---

## P0 发现

### P0-1 ReadDisks 把 INVALID_HANDLE_VALUE 当作有效盘符 → 每台机器虚构约 30 块"幻影磁盘"
- 位置：`src/collect/Sensors.cpp:652-654`
```cpp
const HANDLE h0 = ::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, ...);
if (!h0) continue;  // absent drive number; keep scanning (gaps are legal)
```
- 证据：Win32 `CreateFileW` 失败一律返回 `INVALID_HANDLE_VALUE`（-1），**永不返回 NULL**。`!h0` 对 -1 求值为 false → 不跳过，继续对无效句柄执行 `IOCTL_STORAGE_QUERY_PROPERTY`（必失败）、`d.health="未知"`、`d.busType=未知` 后 `disks->push_back`。n=0..31 循环下，双盘机器会产出约 30 个 model/serial 为空、总线/健康全"未知"的假磁盘行进入 UI；`core/HandleGuard.h` 的 `HandleCloser` 也只判 `if (h)`，随后对 -1 做 `CloseHandle`。三处 IOCTL/句柄操作全空转。
- 修法：`if (h0 == INVALID_HANDLE_VALUE) continue;`（n 段与 GENERIC_READ 重试段同改）；并按资源章程让 `HandleCloser`/`UniqueHandle` 将 `INVALID_HANDLE_VALUE` 规范为空。selftest `sensors_honest` 增补断言：disks 数 ≤ 存在的 PhysicalDrive 数（如 `GetLogicalDrives`/`QueryDosDevice` 交叉）。

### P0-2 NvmeHealth 两处字段偏移均错：温度读的是"高位字节+备用余量"，通电时长读的是"已读数据量"
- 位置：`src/collect/Sensors.cpp:553-560`
```cpp
*critWarn = log[0];
const uint16_t tempK = static_cast<uint16_t>(log[2] | (log[3] << 8));
*pctUsed = log[5];
for (int i = 7; i >= 0; --i) hours = (hours << 8) | log[32 + i];  // power on hours @32
```
- 证据：`IOCTL_STORAGE_QUERY_PROPERTY`(StorageDeviceProtocolSpecificProperty, NVMeDataTypeLogPage, 0x02) 返回的是**原样 512 字节 NVMe SMART/Health 日志页**（微软"Working with NVMe drives"文档）。NVMe 规范布局：字节 0=Critical Warning；**字节 1-2 = Composite Temperature（小端 K）**；字节 3=Available Spare；字节 5=Percentage Used；**字节 32-47 = Data Units Read（128 位）**；**字节 128-143 = Power On Hours**。代码温度取 `log[2]|log[3]<<8`（温度高位字节 | 备用余量%），通电时长取 32-39（实为 Data Units Read 低 64 位）。后果分三档：①健康盘（Available Spare=100）→ tempK≈0x64xx≥25600K，被 100-700 钳位拒绝 → 正常传感器被显示为 NoHardware；②Available Spare=1-2% 的盘 → tempK 落在 512-700 → 显示 239-427°C 的**伪造 Ok 温度**；③"通电时长"= 已读数据单元数（TB 级读量→数百万"小时"），NVMe 分支无上限钳位，直接以真实值名义显示。`critWarn`(log[0]) 与 `pctUsed`(log[5]) 恰好正确，说明作者掌握布局起点但偏移抄错。
- 修法：`tempK = log[1] | (log[2] << 8)`；`hours` 改为小端读 `log[128+i]`（i=0..7），并对 hours 加与 ATA 分支同款的合理性上限；selftest 对本机 NVMe 输出实际温度/时长并与 `Get-PhysicalDisk` 人工比对一次。

### P0-3 AtaSmart 属性原始值整体偏移一字节：温度显示"历史最低温"，通电时长被 256 倍移位
- 位置：`src/collect/Sensors.cpp:594-609`
```cpp
const BYTE* p = s + 2 + a * 12;   // id(1)+flags(2)+value(1)+raw(8)
...
if (id == 194 || (!haveTemp && id == 190)) { t = static_cast<double>(p[5]); ... }
else if (id == 9 ...) { raw = p[5] | p[6]<<8 | ... | p[10]<<40; }
```
- 证据：SMART 属性 12 字节布局为 id(1)+状态标志(2)+归一化值(1)+raw(8)，即 value=p[3]、**raw[0]=p[4]**。代码取 `p[5]`=raw[1]：对属性 194/190（Temperature/Airflow），主流厂商 raw[1] 是**当次会话历史最低温**——一个貌似合理但错误的值以 Ok 状态显示（比 NVMe 的显式荒谬更危险，无法被钳位拦截）；通电时长取 raw[1..6]=hours>>8，结果被 256 倍移位，多数被 `<200000*24` 钳位拒绝（侥幸诚实不可用），少数落进区间成为错误值。
- 修法：温度 `p[4]`（raw[0]）、时长 raw 取 `p[4]..p[9]`；可加"attr 194 raw[0]==0 时回退 raw[1]"的厂商差异处理并注释出处。

---

## P1 发现

### P1-1 计划任务禁用回退路径（RegisterTaskDefinition+TASK_UPDATE）可能静默改坏任务：登录类型硬编码、安全描述符重置、注册触发器即发、无 XML 备份
- 位置：`src/ops/StartupOps.cpp:708-740`
- 场景：`put_Enabled` 失败（少见但非零概率）即走回退：`RegisterTaskDefinition(name, def, TASK_UPDATE, empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, sddl(VT_EMPTY), ...)`。
- 证据（对照 Learn 官方文档，2024-02 版）：
  1. `logonType=TASK_LOGON_INTERACTIVE_TOKEN` 语义是"任务只在已有交互会话中运行"；文档 Remarks 明确"group + INTERACTIVE_TOKEN **能注册成功但任务不会运行**"，SYSTEM 账户任务本应 `TASK_LOGON_SERVICE_ACCOUNT`、S4U/PASSWORD 任务各有对应值——硬编码会把以其他凭据注册的任务**静默改成永不运行**（这正是"禁用"想要的表象，但**启用操作也同样回退**，会把用户想恢复的任务改成不可运行，且无任何报错）。
  2. `sddl` 传 VT_EMPTY：参数文档定义为"与注册任务关联的安全描述符"，未承诺 TASK_UPDATE 时保留原 SD，社区普遍反馈会重置为默认 ACL——自定义过 ACL 的任务（如限制仅某组可执行）权限被改写。
  3. TASK_UPDATE 文档原文："When a task with a registration trigger is updated, **the task will execute after the update occurs**"——对带注册触发器的任务执行"禁用"反而可能当场把它跑起来。
  4. 该写路径**没有任何备份**（任务 XML 未落盘），违反 §9 备份可逆精神；契约头也只给 RegRun/文件夹/UWP 规定了备份。触发器/操作/主体取自现任务定义所以保留，但上述四点都是丢失/破坏面。
- 修法：回退前 `task->get_Xml` 备份到 startup_backup；从 `def->get_Principal()->get_LogonType()` 取实际登录类型透传（GROUP/S4U/PASSWORD/ SERVICE_ACCOUNT 直接诚实报"不支持自动切换，请手动处理"而非硬注册）；确认后再 TASK_UPDATE。或在 UI 对该回退路径加独立红字确认。

### P1-2 启动项备份文件按 id 固定命名 → 第二次切换即覆盖最初备份，可逆链断裂
- 位置：`src/ops/StartupOps.cpp:251`（`path = dir + L"\\" + SanitizeId(id) + L".txt"`），`WriteBackup` 每次整文件重写
- 场景：禁用（备份=原始值，可能含"原值不存在"标记）→ 启用（备份被覆盖为"我们的禁用值"）→ 原始状态永久丢失。若原始值是带时间戳的 0x06 启用态，此后无法恢复；"原值不存在"场景也无法再证明（恢复时残留一个 0x02 值，资源管理器显示"已启用（程序修改）"而非默认态）。§9"备份可逆"要求的是对原始状态可恢复，而非只对上一次状态。
- 证据：`startup_toggle_roundtrip` 自测依赖 `NamesDiff`+删除全部新增备份文件才能自清理，正说明文件名无版本维度。
- 修法：文件名追加时间戳/序号（`<id>_<FILETIME>.txt`）或改为追加式日志（一次切换一行），保留完整切换历史；UI"可随时恢复"文案（Pages3.cpp StartupPage）才有支撑。

---

## P2 发现

### P2-1 DriverOps 双前导反斜杠的 SystemRoot 规范化产出伪造路径
- 位置：`src/ops/DriverOps.cpp:47-54`。`p="\\SystemRoot\\..."`（单前导，实测形态）时 `find('\\',1)=11` 正确；但若出现 `\\SystemRoot\\...`（双前导，代码显式处理的分支），`find('\\',1)=1`，结果为 `C:\WINDOWS\SystemRoot\system32\drivers\x.sys`——一个不存在却貌似合理的路径，后续签名校验会给出误导性结论。当前主流 API 不产双前导形态，属潜伏缺陷。修法：先剥 1-2 个前导反斜杠再定位 marker 之后的反斜杠。

### P2-2 ApprovedStateAny 跨 Hive "任一禁用即禁用" 造成同名值互相污染与切换后显示不回弹
- 位置：`src/ops/StartupOps.cpp:204-217`。HKCU Run 与 HKLM Run 可合法存在同名值（如 OneDrive）；用户在任务管理器禁用 HKCU 项后，本工具枚举的 HKLM 同名项也显示禁用；提权切换 HKLM 项（写 HKLM StartupApproved）后显示仍为禁用（HKCU 禁用值仍在）→"切换看似无效"。语义未文档化（R5 14a2），当前取保守方向（不会假报启用），但建议判定仅取该项自身 Hive，或切换时双 Hive 处理并在文案说明。

### P2-3 UWP EnabledByPolicy(4) 被显示为"禁用"且可切换
- 位置：`src/ops/StartupOps.cpp:524-526`（`it.enabled = stateVal == 2`、`canToggle = true`）。R5 14d：4=EnabledByPolicy（策略启用、用户锁定），3=DisabledByPolicy。当前把 4 当禁用并把切换放行，写 0/2 后策略可能回写，用户看到"切换无效"。建议 3/4 显示为策略锁定态（启用/禁用如实），禁用切换按钮或注明实验性。

### P2-4 ReadCpuTemp 把 WMI 通用失败折叠为 NoHardware
- 位置：`src/collect/Sensors.cpp:317-324`。`WmiQuery` 返回 `!ok && !denied && !notSupported`（如 WMI 服务损坏、`WBEM_E_FAILED`）时 rows 为空 → 走 `notSupported || rows.empty()` 分支，文案"本机无 ACPI 热区传感器"——故障被谎报为无硬件（违反诚实三分法精神；NeedAdmin 判定本身无误）。建议增加"查询失败(hr=…)"独立文案；磁盘路径已正确区分（631 行）。

### P2-5 CStrFromDesc 栈缓冲越界读（设备可控偏移）
- 位置：`src/collect/Sensors.cpp:516-525`。`off < 2048` 只约束起点，循环最多再读 1024 字节 → 最大读至 `qbuf[2047+1023]`，越界最多 1023 字节；偏移来自存储设备描述符（恶意/畸形 USB 设备固件可控）。修法：`for (size_t i = 0; i < 1024 && off + i < 2048 && s[i]; ++i)`，并先校验 `ret >= desc->Size`。

### P2-6 EtwNetCollector：崩溃孤儿会话、parseFails_ 不可观测、无运行时事件率监控
- 位置：`src/collect/NetTables.cpp:294-455`、`CollectService.cpp:310-324`。
  1. 进程崩溃且**从未再启动**时，同名清理无法覆盖，实时会话（64×64KB 内核缓冲）残留至重启或 PID 复用后被同名启动清理；章程的"会话名含 PID"只是部分缓解，建议文档声明 + 评估调低 MinimumBuffers 以缩小残留足迹。
  2. `parseFails_` 无 getter；若 `size`/`pid` 载荷名在未来系统漂移，全部事件进 parseFails_，特性静默失效而 `CAP_NET_ETW` 照常置位、UI 永远"—"。建议在 `Running() && totalEvents_==0 && parseFails_>0` 持续 N tick 时打 Warn 或在快照 caps 旁暴露计数。
  3. Kernel-Network 是"量最大"的事件流（R5 #10b），逐事件两次 TdhGetProperty+互斥+map；selftest 探针仅打印一次事件率（非管理员下被跳过），运行期无事件率/CPU 自监控与上限。建议增加事件率日志与阈值告警（信息用，不强求限流）。

### P2-7 ServiceOps 保护门的伪 pid=1 技巧盲区评估（结论：可接受，补两条加固）
- 位置：`src/ops/ServiceOps.cpp:53-55`。门只按服务键名匹配受保护镜像名（能拦下字面命名 lsass/services 的服务）；svchost 组内具体服务不参与判定。评估：真正不可停的核心服务（RpcSs/DcomLaunch/EventLog/SamSs 等）由 SCM 自身拒绝（ERROR_INVALID_SERVICE_CONTROL），故 StopServiceByName 达不到崩溃级破坏；剩余风险是 stopDependents 级联造成的服务面瘫痪（逐依赖已过同名门）。建议：① `ERROR_INVALID_SERVICE_CONTROL` 单列更明确文案（当前落通用错误）；② 目标服务 pid 承载多服务时在确认框提示。

### P2-8 selftest 往返测试在注册表留下残留
- 位置：`src/selftest/ops3_test.cpp:273-280`。清理删除了临时 Run 值与备份文件，但 `startup_toggle_roundtrip` 的启用步骤写入的 `HKCU\...\Explorer\StartupApproved\Run\STMTest3`（12 字节 0x02）及可能新创建的 StartupApproved\Run 键未清理，属注册表残留（不影响真实项，方向安全）。修法：cleanup 中 `RegDeleteValueW(approvedKey, L"STMTest3")`。

### P2-9 启动文件夹只枚举 .lnk，漏掉 shell 同样会执行的 .exe
- 位置：`src/ops/StartupOps.cpp:344-346`。与冻结契约"(.lnk files)"一致，但资源管理器对启动文件夹中的 .exe 同样会执行，任务管理器启动页会显示——完整性缺口，建议向架构师登记（阶段 4 或契约变更）。

### P2-10 计划任务非 EXEC 动作早退 + 其他小项
- 位置：`src/ops/StartupOps.cpp:375`（首个 EXEC 动作 `get_Path` 失败即 `return {}`，不再尝试后续动作——命令列显示诚实为空，影响仅展示）；`StartupOps.cpp:581-599`（RegCreateKeyExW 在备份写盘**之前**创建 StartupApproved 键，备份失败时残留空键，无值写入，可接受但与"备份先行"字面不符）；`CollectService.cpp:196` 与 ETW 差分块之间 `sysCol.Collect` 在锁外，无问题（列出仅为说明已核对锁序）。

---

## 已查无问题清单

- **StartupApproved 二进制写法**与 R5 14a2 一致：禁用=0x03+当前 FILETIME、启用=0x02+零时间戳，12 字节；写入后附"实验性（R5 表B#1）"WARN 日志；RunOnce 复用 StartupApproved\Run 叶子符合社区/取证认知。
- **备份先行在 StartupApproved 与 UWP 两条写路径真实成立**：`WriteBackup` 失败（建目录/写文件）→ 立即返回 false、不执行任何值写入；错误文案含"已拒绝写入"。
- **HKCU/HKLM 写权限判定**：HKLM Run/RunOnce/Run32、公共启动文件夹项 canToggle=IsProcessElevated()；UWP/HKCU/用户启动文件夹恒可切；提权判定在枚举与切换两端一致。
- **ResolveValueSplit** 从右向左对活键重查，能正确处理含 `\` 的值名；解析失败诚实报"值已不存在"。
- **ToggleUwpState 的 id 校验**（`std::size(kUwpBase)-1` 落在反斜杠）数学上正确；备份先行、失败拒绝写；State 0/2 写法与 R5 14d 映射一致（除 P2-3 的策略态显示）。
- **往返测试不污染真实项**：使用临时 HKCU Run 值 STMTest3，备份可自动清理（残留见 P2-8）。
- **计划任务枚举**：递归深度限 8、TASK_ENUM_HIDDEN、1-based VARIANT 索引正确、get_Definition 失败诚实跳过；COM `ComStaGuard`（SUCCEEDED 配对）与 ComPtr/BstrGuard 全部早退安全，每次调用自配对。
- **ServiceOps**：pending/stopped/pid 归 0 与 R5 7b 文档化有效态集合逐项一致；UI 对 pid 0 显示"—"（ServicesPage），不显示"系统"，无误导；sharedProcess 判定（同 pid 服务数>1 才标共享）正确；依赖停止 leaf-first 且逐依赖重过保护门、深度 16 防环；`WaitForState` 轮询/超时正确、全部 SC_HANDLE RAII；ALREADY_RUNNING / SERVICE_NOT_ACTIVE 幂等成功；服务已停的重复停止为幂等成功。
- **StartServiceByName**：DISABLED/ACCESS_DENIED 诚实出中文错误；启动后等待 RUNNING 15s。
- **DriverOps 24H2 判据**：全 NULL（含空数组 vacuous allZero——已启动系统必有驱动，不会误伤正常空结果）→ 提权重试一次 → 仍全空才降级"需要管理员"，SeDebug 用后即释且有日志；本机 22631 非管理员 226/226 驱动全部解析到路径，selftest 通过。
- **NetTables**：32KB 起步、ERROR_INSUFFICIENT_BUFFER 增长重试（8 次、size 停滞时翻倍兜底）；`NetPort` 手工 ntohs 正确；InetNtopW 动态绑定、IPv6 RFC5952、回退全组十六进制；TCP/UDP v4+v6 四表齐全；UDP state=0 由 UI 特判显示"—"（NetPage 317-321 行），`TcpStateLabel` 对 UDP 输入不会被调用且未知态回退 hex，契约一致。
- **EtwNetCollector 生命周期**：Start 前以 ControlTraceW(STOP) 清同名残留；Stop 先置 session_=0 保证幂等；ControlTraceW 在锁外执行、consumer join 后再 CloseTrace，无死锁路径（OnEvent 需要 mu_ 完成）；锁序恒为 Impl::mu_→collector::mu_；消费线程 OpenTrace 失败路径不阻塞 Stop；析构兜底 Stop；selftest 析构孤儿会话检查通过。
- **非 admin 行为**：StartTraceW 失败诚实禁用、`netEtwEnabled` 不置位、caps 无 CAP_NET_ETW、netBytesPerSec 保持 kUnavail（selftest 显式断言通过）。
- **netBytesPerSec 回填**：首 tick（prev 空）跳过、第二 tick 起差分；`bytes_` 只增不清 → pid 本 tick 无新事件仍出现 → 真实 0 B/s（"0 仅表示真实为零"）；counter 回退分支（now<was）实际不可达，防御性合理；pid 复用近似已在代码注释声明。
- **CollectService ETW 接线**：默认 OFF；SetNetEtwEnabled 在 Impl::mu_ 内启停（Start/Stop 不回调服务，锁序安全）；失败不半启用；退出顺序 worker join→Impl 析构→ETW Stop 正确。
- **Sensors 其余路径**：CallNtPowerInformation ProcessorInformation 布局 static_assert(24)、CurrentMhz==0 跳过不造假；NVML 绝对路径加载、init/shutdown/FreeLibrary 配对、tempC>0 门；风扇恒 NeedDriver；ReadSensors 全程 try/catch 不抛；WMI `CoInitializeEx` ownInit/RPC_E_CHANGED_MODE 处理正确，RPC_E_CHANGED_MODE 不配对 CoUninitialize 是对的；MSAcpi 非管理员→NeedAdmin 判定正确。
- **既有产物**：`stm_selftest.exe` 40/40 通过（含 services/startup/drivers/net/sensors/etw 全部阶段 3 用例）；`SuperTaskMgr.exe --smoke 120` 退出码 0。

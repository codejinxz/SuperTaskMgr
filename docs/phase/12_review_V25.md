# 终审评审 V25 — 连接级网络监视器（NetMonitor / NetTables / DrawNetMon）

- 评审人：V25（独立终审，与其他评审者互不知情）
- 日期：2026-09-20
- 对象：`src/collect/NetMonitor.cpp`、`src/collect/NetTables.cpp`（EtwNetCollector 扩展）、
  `src/collect/CollectDetail.h`（DiffConnSnapshots/EventRing/collector 契约）、
  `src/app/ui3/Pages3.cpp`（DrawNetMon 区）、`src/app/ui3/NetMonUi.h`
- 环境：Windows 11 23H2（10.0.22631），本 shell 为管理员（elevated=1）。
  **重要修正**：本轮实跑中 Kernel-Network 实时事件流**并非 0**——selftest 探针实测到
  7 个事件、4 个端点行（见「实跑记录」），因此端点聚合按实测判定，非仅代码审查。

## 统计

| 级别 | 数量 |
|---|---|
| P0（必须修） | 3 |
| P1 | 2 |
| P2 | 6 |

实跑：stm_selftest.exe **127/127 通过**；`--smoke 150` rc=0；`--autotest` 六条
（kill/tree/startup/dialogclick/about/wallpaper）结果日志全 PASS（kill 一次 rc=1 但日志
PASS，见实跑记录备注）。

---

## P0（必须修）

### P0-1 连接事件差分：Tcp4 事件被 Tcp6 调用整体清空 —— IPv4 事件流恒为空

- 位置：`src/collect/NetMonitor.cpp:109`（`events->clear()`）与 `src/collect/NetMonitor.cpp:556-557`（调用点）。
- 证据：
  ```cpp
  std::vector<ConnEvent> evs;
  DiffConnSnapshots(prev, cur, now, ConnProto::Tcp4, &evs);
  DiffConnSnapshots(prev, cur, now, ConnProto::Tcp6, &evs);   // 进入函数即 events->clear()
  for (ConnEvent& e : evs) { ... ring.Push(...); }            // 只剩 Tcp6 事件
  ```
  `DiffConnSnapshots` 的契约是「清空后写入」（NetMonitor.cpp:108-109），全仓库仅此一处
  运行时调用（grep 证实，另为 selftest 单独调用）。两次调用复用同一 `evs`，第二次调用把
  第一次的 Tcp4 结果**全部丢弃**。主页网络 IPv4 连接（现实流量的绝对主体）的新建/断开/
  状态变化**永远不会出现在事件流里**；纯 IPv6 机器才会看到事件。UI 的「暂无匹配的事件」
  提示会误导用户以为无连接变化。
- 为何 127/127 仍绿：`netmon_diff_pure`（netmon_test.cpp:174-239）只测纯函数本身（每次
  用独立 vector），不覆盖 PollLoop 的复用调用。
- 修法（任选其一）：
  1. 调用方用两个 vector（或 `evs` 换成 `append` 语义容器）后合并；
  2. 把 `DiffConnSnapshots` 语义改为**追加**（去掉 `events->clear()`，注释注明），
     selftest 中 `evs.clear()` 的显式清理已兼容。
  建议同时补一条集成单测：同一 evs 连续差分 Tcp4+Tcp6，断言两族事件都保留。

### P0-2 EtwNetCollector::Start() 无条件硬编码会话名，SetSessionName 是死代码 —— 两个 ETW 功能互相挤掉、静默失效（违反「互不挤掉」章程）

- 位置：`src/collect/NetTables.cpp:401`（`sessionName_ = Fmt(L"SuperTaskMgr-Net-{}", pid)`，
  无条件赋值）；`src/collect/NetMonitor.cpp:473`（`etw.SetSessionName(L"SuperTaskMgr-NetMon-...")`）；
  `src/collect/CollectService.cpp:87,348-361`（CollectService 的 `netEtw` 走默认名）。
- 证据：NetMonitor.cpp:471-474 明文契约「与 CollectService 的 L"SuperTaskMgr-Net-<pid>"
  错开，互不挤掉」，CollectDetail.h:324-326 也写明 SetSessionName 的用途。但 Start() 第一行
  就把名字覆写回 `SuperTaskMgr-Net-<pid>`。而这两个开关**同在「网络」页**：
  - 工具栏「按进程流量（ETW）」→ CollectService::SetNetEtwEnabled（Pages3.cpp:998-1012）；
  - 实时监视区「按远程端点聚合（需管理员）」→ NetMonitor 的 etw.Start()（Pages3.cpp:391）。
  两者都用 `SuperTaskMgr-Net-<pid>`。后启动者经「启动前清残留同名会话」（NetTables.cpp:403-410）
  **把先启动者的会话杀掉**：CollectService 的 consumer 的 ProcessTrace 返回、字节累计冻结，
  但其 `session_` 句柄非 0 → `Running()` 仍 true（NetTables.cpp:623-626）→ 每进程网速静默
  变 0 而设置勾选仍显示开启；反向顺序则 Top 远程表冻结在旧值而 `trafficOn_` 仍 true。
  两条路径都不报警、不回滚——诚实性违约。
- 连带后果：selftest 的孤儿检查查的名字从未被创建（见 P1-2），全绿是**空转的绿**。
- 修法：
  1. Start() 改为 `if (sessionName_.empty()) sessionName_ = Fmt(...)`，并在构造函数给默认名；
  2. 修正 selftest 的两处期望名（P1-2）；
  3. 建议给 NetMonitor 增加会话健康检测（`trafficOn_ && !etw.Running()` 时黄色提示），
     防其他外部 STOP 情形再度静默。

### P0-3 ETW 远程端口未做网络字节序换算 —— Top 远程表端口/服务名整列错误（实测证实）

- 位置：`src/collect/NetTables.cpp:533,582`（`k.port = static_cast<uint16_t>(rportRaw & 0xFFFFu)`，
  无 `ntohs`/换序）；显示与服务名查表在 `src/collect/NetTables.cpp:236-242`。
- 证据（本机实测，`stm_selftest.exe` 的 `netmon_etw_remote_probe` 输出）：
  ```
  原始样本：sport=40661(0x9ED5) dport=40917(0x9FD5) direction=0 proto=0 saddrFam=2 daddrFam=2
    112.90.80.96:47873  pid=4(System) in=0 out=164
    127.0.0.1:40917  ... 127.0.0.1:40661  ...  192.168.3.20:41667 ...
  ```
  47873 = 0xBB01，字节换序后即 **443 (0x01BB)**——系统进程对远端 :443 出站是教科书式流量；
  40917/40661/41667 换序后为 54687/54622/50082，全部落入 Windows 动态端口段（49152-65535），
  原值则大多不在。Microsoft-Windows-Kernel-Network 的 sport/dport 为**网络字节序**，代码按
  主机序直取，导致：Top 表远程端口整列错误、`ServiceNameForPort` 几乎全空（443→47873 查不到
  HTTPS）、用户看到的是错误数据（非「缺数据」，是**错数据**）。探针里 `LoopbackEcho` 已算出
  换序后的监听端口（netmon_test.cpp:134 `Swap16`）却没有用它断言端点表——单测漏钉此点。
- 修法：`k.port = Swap16(rportRaw & 0xFFFFu)`（与 NetTables.cpp:68 NetPort 同法）；并在
  `netmon_etw_remote_probe` 加断言：端点表中必须出现换序后的回显监听端口，钉死字节序。

---

## P1（应修）

- **P1-1 连接表部分失败导致基线被残表替换 → Closed/New 事件风暴**
  `src/collect/NetMonitor.cpp:548`：`if (cur.empty() && !serr.empty()) continue;` 只拦截
  「全失败」。四族中任一族 GetExtended*Table 失败（如重试 8 次后仍 INSUFFICIENT_BUFFER、
  内存压力）时 `cur` 非空照常差分：该族全部存量连接被报 Closed，下一拍恢复时再全量报 New。
  事件流把采集故障伪装成网络活动。修法：`!serr.empty()` 时一律保持基线（continue），
  或按族维护基线、只差分成功的族。

- **P1-2 selftest 孤儿会话检查的会话名与实际创建名不符（空断言）**
  `src/selftest/netmon_test.cpp:411-412,464-471`（期望 `SuperTaskMgr-NetMon-<pid>` /
  `SuperTaskMgr-Dns-<pid>`）、`:482,530`（期望 `SuperTaskMgr-NetProbe-<pid>`）。
  由于 P0-2，ETW 实际永远用 `SuperTaskMgr-Net-<pid>`：NetMon/NetProbe 两个名字的
  `SessionExists` 恒 false，孤儿清理回归根本没被测到。修 P0-2 后此检查才生效；
  另建议断言 `SuperTaskMgr-Net-<pid>` 也不残留（防默认名泄漏）。

---

## P2（建议）

- **P2-1 `proto`/`direction` 字段在本机清单解码中不存在，端点桶 proto 恒 0、UDP 服务名查 TCP 表**
  实测：`proto=0`、`direction 字段命中 0/7`（Win11 23H2）。`NetTables.cpp:535,583` 使
  `k.proto=0`，`NetTables.cpp:242` 的 `ServiceNameForPort(port, proto==IPPROTO_UDP)` 恒按
  TCP 表（UDP :123/:1900 等服务名错/空）。且 FieldStats（CollectDetail.h:337-343）没有
  protoHit/protoMiss，缺失不可观测。修法：补计数 + proto==0 时双表查名或标「—」。
- **P2-2 Top 排序平局键不完备**：`NetTables.cpp:248-256` 平局只按 (remote,port,pid)，
  漏 proto/family；0 字节事件（recv 0=EOF）可造成两组全同键值，顺序退化为 unordered_map
  迭代序（非确定）。补 proto、family 入比较器即可。
- **P2-3 Stop() 对 ControlTrace STOP 失败无超时防线**：`NetTables.cpp:614-618`、
  `NetMonitor.cpp:424-428` 先 STOP 再 `consumer_.join()`；若 STOP 失败（句柄/权限/被外部
  占用）ProcessTrace 不返回 → join 永久阻塞 → jobs 线程挂死、UI 开关卡在「正在切换」。
  建议有限次按名重试 STOP + 有界等待（如 3s）后放弃并记日志。
- **P2-4 DNS 丢弃不可观测**：`pending_` 满静默丢（`NetMonitor.cpp:408` /
  `CollectDetail.h:404`），`dnsRing.Dropped()` 从未被读出展示。连接事件有 DroppedEvents
  黄字，DNS 没有。建议在 DNS 区补一行丢弃提示（诚实口径与连接事件对齐）。
- **P2-5 实时会话无积压**：StartTraceW 成功与消费线程 OpenTrace/ProcessTrace 之间的窗口
  内事件永久丢失（实测：64 轮回显仅收到 7 个事件）。「字节为自启用起累计」在头几秒偏低。
  属 ETW 实时模式固有语义，建议在 Top 区文案补「启用瞬间可能有少量遗漏」。
- **P2-6 提示文案折叠不可见**：诚实边界声明、丢弃黄字、DNS 自动禁用提示都在
  `CollapsingHeader("实时监视")` 返回之后（Pages3.cpp:350-364）；折叠时后台照常记录/丢弃
  用户无感知。可接受，但至少丢弃计数建议在折叠态也有摘要。

---

## 已查无问题清单（证据 = 代码审读 + 单测/实跑）

1. **DiffConnSnapshots 纯函数语义**：New/Closed/StateChanged 判定、UDP 族零事件、监听消失
   不产 Closed（LISTEN 跳过，NetMonitor.cpp:157）、Closed 按旧状态标注、unixTime 写入、
   processName 留空 —— netmon_diff_pure PASS；仅调用点有 P0-1。
2. **EventRing**：满时覆盖最旧 + Dropped 计数、Drain 保序取空、Clear 重置计数、容量 0 防御、
   互斥完备 —— netmon_event_ring_overflow PASS；Push 恒不阻塞轮询线程（保新弃旧成立）。
3. **QuadKeyHash / EndpointKeyHash**：全字段逐字节 FNV-1a，无填充读取、无漏混字段。
4. **TopRemoteFromEndpoints**：总量降序、topN 截断、topN=0/1、service/pid 填写契约
   （进程名留空给调用方）—— netmon_top_remote PASS（平局键不完备见 P2-2）。
5. **ServiceNameForPort**：TCP/UDP 分表（123/TCP 空、53 双栈 DNS 等）—— PASS。
6. **ProcNameCache**：5s TTL、失败不重锤（失败也推进 stamp）、查不到返回空 —— 读后无问题。
7. **AddrProp 启发式**：4/16/28 字节三分支、sockaddr_in(+4)/in6(+8) 偏移正确、size≤28 上限
   —— 边界内诚实（误判概率已在注释声明）。
8. **DnsCollector 生命周期**：启动前清残留同名会话、EnableTraceEx2 失败即停会话、消费线程
   创建失败回滚、Stop 幂等（先置 session_=0）、join 后才 CloseTrace(session)、
   openTrace 由消费线程自关 —— 本机实跑 Start/Stop ×2 无残留。
9. **DNS 自动禁用**：blind（raw≥32 且 decoded==0）/dead（OpenTraceW 失败）两路均触发，
   停会话 + dnsOn=false + dnsLastError，再开重试；DrainDns 每帧调用的契约被 UI 遵守
   （Pages3.cpp:309）。本机实测解码成功（2/14，含主动 DnsQuery 触发）。
10. **DNS 去重**：2s 窗口过期清理、同 (pid,query) 去重、Query+Response 双事件合一。
11. **锁序**：Impl::mu → ring/dns.mu_/names.mu_/etw.mu_ 单向，无反向获取；PollLoop 的 cv
    wait 释放 mu，表快照期间 UI 至多阻塞毫秒级；SetRemoteTraffic 持 mu 调 Start 不碰
    Impl 其他共享态 —— 无死锁路径。
12. **×5 开关幂等**：全新实例 ×5 + 单实例三路混合 ×5（本机管理员实跑，ETW/DNS 真实启停）
    —— netmon_lifecycle_toggle PASS；析构（Impl::~Impl → 三路全停）随每轮实例验证。
13. **UI 诚实读回/回滚**：PollMonToggles 先于读回执行（同帧回滚）；无在途开关时三开关以
    采集器真实状态为准；BeginDisabled 防在途重复点击；jobs 队列不可用两分支均回滚 + toast；
    DNS 自动禁用提示置位/清除逻辑正确。非管理员下 Traffic/DNS 失败回滚路径成立
    （SetRemoteTraffic false→诚实保持禁用）。
14. **暂停显示语义**：Drain 每帧照常（折叠/暂停均不漏），仅冻结视图重建；恢复置 ~0 强制
    重建补齐；evAll_/dnsAll_ 容量裁剪从尾部（弃旧）正确；「已暂停（后台仍在记录）」黄字在位。
15. **过滤纯函数**：进程/远程子串（预小写）、远程匹配 "ip:port" 全文且空远端不匹配、协议
    下拉、kindMask 位与（全空=全滤掉）、多条件 AND —— netmonui_event_filter PASS。
16. **CSV**：RFC 4180 转义（逗号/引号/CR/LF，引号翻倍）、表头 9 列钉死、deque/vector 同构、
    BOM 由写入方补、打开失败/建目录失败（EnsureDir 空串契约已核实 FsUtil.cpp:26-37）/
    flush 后 good() 检查齐全；导出内容 = 当前过滤视图（tooltip 声明一致）。
17. **ListClipper**：Begin(行数)+Step+DisplayStart/End、ScrollY 子区域 300px、PushID(r)
    配对 —— 大列表只画可见行，容量 8192 上限与 collect 层同口径。
18. **安全与隐私**：不抓载荷（仅元数据）且 UI 明示边界文案（Pages3.cpp:353-354）；CSV 仅含
    本机连接元数据（IP/端口/进程/PID），落 %LOCALAPPDATA%\SuperTaskMgr\captures\，域名不入
    CSV；会话名带 pid、析构全停、启动前清残留 —— 无孤儿、无越权写入。
19. **smoke/autotest**：--smoke 150 rc=0（stm.log elevated=1, smoke=150）；--autotest 六条
    结果日志全 PASS。备注：kill 模式首轮 shell 返回码 1 但结果日志 PASS（复跑两次 rc=0），
    疑似子进程销毁与退出码采集的竞态，非本评审对象，建议后续轮核实。

## 结论

核心差分纯函数、环形缓冲、DNS 采集器生命周期、UI 纯函数与诚实读回质量良好；但
**三个 P0 恰好分布在三条数据通路的「最后一步」**：IPv4 事件在入环前被清空（P0-1）、
ETW 会话名被硬编码回默认导致两功能互杀（P0-2）、远程端口缺字节序换算导致整列错值（P0-3，
实测证实）。三者都有「单测各测各的、集成点无人断言」的共同成因，修复时应同步补集成级
断言（PollLoop 双族差分、真实会话名孤儿检查、回显端口字节序钉死）。

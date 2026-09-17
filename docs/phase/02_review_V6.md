# 阶段 2 审查报告 — V6（正确性维度）

审查人：subagent V6（与其他审查者互不知情）。审查基线：工作区当前代码，未做任何构建/修改。
范围：src/collect/（7 文件）、src/ops/ProcessOps.cpp、Signature.cpp、DetailsProvider.cpp（含 .h 对照）、src/app/ui/Pages.cpp、SortKey.h、UiText.h、VersionInfo.h；对照 docs/phase/01_架构设计文档.md §4/§6、docs/research/R5_Collection_APIs.md。

## 结论

- **P0（错误行为/崩溃）：0 条。**
- **P1（应修）：3 条。**
- **P2（建议）：10 条。**
- `build/Release/stm_selftest.exe` 实测：**24 通过 / 0 失败**（collect_snapshot_integrity、collect_selfcheck_gate、collect_privatews_gate、collect_gpu_adapters_filtered、ops_identity_reverify_dead_pid、ops_tree_plan、ui_sort_* 等全绿）。

---

## P1（应修）

### P1-1 进程过滤：关键字未做小写化，含大写字母的搜索必然失配
- 位置：`src/app/ui/Pages.cpp:589-601`（`ContainsLower` / `MatchesFilter`）
- 问题：`ContainsLower(hay, needle)` 只把 hay（进程名）逐字符 `towlower`，needle（`filterWide_`）原样参与 `find`。用户输入 "Chrome"、"EXPLORER" 等任何含大写的词都无法匹配 "chrome.exe"。
- 证据：`lower.find(needle)`（Pages.cpp:594）中 needle 未预处理；`UpdateFilter()`（Pages.cpp:581-587）也只是 Utf8→Wide 转换，无大小写归一。函数名 ContainsLower 与"大小写不敏感过滤"的意图自证。
- 修法：进入 `ContainsLower` 前对 needle 做同样的 `towlower` 归一（或在 `UpdateFilter` 里一次性生成 `filterWideLower_`，并同时小写化后用于名称比较；PID 比较不受影响）。

### P1-2 网络速率：接口集合变化（VPN 断开/网卡热拔）时无符号下溢，产生天文数字假速率
- 位置：`src/collect/SystemCollector.cpp:104-118`（`CollectNet`）
- 问题：`recv`/`send` 是"当前 Up 且非回环接口的 InOctets/OutOctets 累加之和"。任一接口断开（VPN 切换、Wi-Fi 漫游、USB 网卡拔出）都会使总和变小，`recv - prevRecv_`（uint64）下溢成 ~1.8e19，除以 elapsed 后得到假速率并在 UI 显示一个 tick 以上。
- 证据：`out->netRecvBps = static_cast<double>(recv - prevRecv_) / elapsedSec;`（SystemCollector.cpp:113-114）无任何回绕/倒退防护；VPN 连接/断开是常见操作，触发概率高。
- 修法：delta 前加倒退守卫：`if (recv < prevRecv_ || send < prevSend_) { /* 计数器基线重置：本 tick 跳过，仅更新基线 */ }`，保持 `netRecvBps/netSendBps` 为 kUnavail 一个 tick。

### P1-3 GPU 虚拟适配器过滤信号 3：多显卡机器上完全空闲的真实独显会间歇性从 sys.gpus 消失
- 位置：`src/collect/GpuCollector.cpp:204-224`（kept 过滤规则 3）
- 问题：规则 3 在 `anyEngine && !singleGpu && !primary && engineLuids 不含该 LUID` 时剔除适配器。\GPU Engine 实例只在"有进程正在使用该适配器"时存在；Optimus/双 GPU 机器上空闲 dGPU（桌面合成在 iGPU 上、无 CUDA/GL 进程持有它）当 tick 恰好没有任何引擎实例时被剔除，一旦有进程触碰又重新出现 —— 适配器列表反复闪烁、"真实适配器被误杀"正是本条规则的风险面（文件头注释已自知保守，但保守条件仍不足以覆盖"纯空闲 dGPU"这一常见稳态）。
- 证据：`engineLuids` 只由**当次** collect 的实例名解析累积（GpuCollector.cpp:172-181），无任何时间维度记忆；规则在 `!pdhFailed_ && anyEngine && !singleGpu && !a.primary` 时生效（GpuCollector.cpp:219-221）。
- 修法（任选其一）：(a) 引入滞后记忆——LUID 一旦出现在 engineLuids 就保留 N 分钟（成员 `std::map<uint64_t, steady_clock>` 记录最近出现时间），空闲独显在窗口内不剔除；(b) 既然信号 2（IddCx 命名）已覆盖本机两类虚拟适配器，直接删除信号 3。推荐 (a)。

---

## P2（建议）

1. `src/collect/GpuCollector.cpp:118` — `EnumAdapters1(i,&a) != DXGI_ERROR_NOT_FOUND` 作为唯一循环退出条件；若返回其他错误码将死循环并对 null `a` 调 `GetDesc1/Release`。改为 `while (SUCCEEDED(hr = ...))` 并对 `a` 判空。
2. `src/ops/DetailsProvider.cpp:262,275` — `seqHolder->store(seq)` 在 `Submit` 返回后才执行，worker 可能先跑完 job 使 `n.seq=0`（真数据竞争，概率低）。当前 UI（Pages.cpp `DrainNotifications`）不消费 seq，故无实际影响；修法：Submit 前在锁内生成 seq 或让 job 从队列侧携带 seq。
3. `src/ops/DetailsProvider.cpp:292-298` — `Invalidate()` 无任何调用方：`cache_` 与 `g_attemptedKinds` 随选中过的进程（含已退出）无限增长，长时间运行内存缓慢泄漏。建议在快照中发现进程消失时调用 Invalidate。
4. `src/collect/SelfCheckGate.cpp:129-137` — 第 6 项用**索引**拼接 ID Process 与 Working Set - Private 两个数组，依赖 PDH 对同一对象输出相同实例顺序。虽然当前成立，按名字（归一 `#N` 后）join 更稳；且此处注释"names lack '#N'"与 CollectDetail.h:62"含 #N"注释互相矛盾，应统一。
5. `src/collect/CollectService.cpp:164` — worker 无锁读 `gpuEnabled`，而 `SetGpuEnabled` 在 UI 线程持 `mu_` 写：构成 C++ 意义上的数据竞争（实践中 bool 无害）。读侧加锁或改 `std::atomic<bool>`。
6. `src/app/ui/Pages.cpp:871-885` — 详情面板"父进程"仅按 pid 二分查找（无 createTime 校验），父进程退出且 pid 被复用时会把无关进程显示为父进程。至少与快照中该行的 parentPid 交叉核对，或标注"pid 可能已被复用"。
7. `src/app/ui/Pages.cpp:182-196,291-298` — 树杀 plan 依赖 `jobs.Submit` 成功；`Submit` 返回 0（队列未运行）或 job 被丢弃时 `planCount` 永远为 -1，确认框永不打开且无提示，`req.kind` 卡在 KillTree 阻塞后续确认。加超时（如 3s）后自动放弃并提示。
8. `src/collect/SystemCollector.cpp:66-91` — 读写两数组的有效性独立判定：预热 tick 上可能出现"读有值、写全 invalid"，此时 `diskWriteBps=0`（显示 0 B/s）而非诚实的"—"。当 `!sawTotal && writeSum==0 && 无有效写实例` 时应保持 kUnavail。
9. `src/collect/GpuCollector.cpp:233-251` — `GpuAdapterInfo::memUsed` 由 \GPU Process Memory 按进程求和，天然漏掉非进程记账的显存（驱动/系统占用），数值系统性低于真实适配器占用；若要"适配器已用显存"语义应改用 \GPU Adapter Memory(*)\Dedicated Usage。至少在 UI/注释标注口径。
10. `src/collect/ProcessCollector.cpp:283,288` — 1/5 切片重算 `ComputeSupp` 时，若进程权限状态变化（如提权后），已缓存的 PF_AccessDenied 会被清除，与 ProcData.h:40"sticky within a snapshot"的弱承诺有出入（快照内仍一致，跨 tick 可清除）。可接受但建议注释明示。
    （防御性备注，不计级：`src/collect/CollectUtil.cpp:157-159` 对 `ImageName.Buffer` 未做 `cap` 范围校验，信任内核返回值；`src/app/ui/SortKey.h:116,131` 双 kUnavail 行未按 pid 决胜，与"pid breaks ties"的冻结语义有微小出入。）

---

## 已查无问题（按任务 9 个维度）

1. **NtQSI 布局解析**：`SysProcInfoX64`/`SysThreadInfoX64`/`SysCorePerfX64` 偏移与 NtDoc/Geoff Chappell x64 布局一致，7+3+1 条 static_assert 覆盖关键字段；`ParseProcBuffer` 对条目与线程数组均做 `cap` 越界检查，`NextEntryOffset==0` 终止（无死循环）；缓冲从 1MB 起最多 8 次重试，`need+65536` 与 `size*2` 双策略 + 溢出守卫；空映像名时 pid 0/4 分别补 "System Idle Process"/"System"。`SystemPerformanceInformation` 用 312B 固定缓冲（与 winternl/phnt 尺寸一致，本机实测通过）；`SystemProcessorPerformanceInformation` 按 `GetNativeSystemInfo` 核数 × 48B 分配、`need>=size` 时按 `need` 重试一次、count 由 ResultLength 推导。
2. **差分与身份**：cpu/disk/pageFaults/ctx 四条差分路径全部经过 `pit->second.createTime == row.createTime && row.createTime != 0` 身份校验；首 tick `elapsed==0` 全部 kUnavail；pid 复用因 createTime 不同而重置基线（prev_ 每 tick 全量重建）；`cpu = 100*Δt/(elapsed*1e7*cores)` 与契约公式一致；`diskBytesPerSec=(R+W+Other)Δ/elapsed`，UI 悬浮提示已标注"含文件+网络+设备 IO 总和"。
3. **自校验门**：6 项容差与文档一致（时间 ±1s、字节 ±25%+1/2MiB slack、计数 ±10%+slack）；任一违例整体降级、`validated==0` 降级、NtQSI 不可用降级，降级后 tick 走 Toolhelp+PSAPI 且状态栏如实显示；一次性 PDH query 在所有路径经 `PdhCloseQuerySafe` 关闭（含中途失败）；第 6 项 PDH 不可用只跳过该项不降级。
4. **GpuCollector**：WARP（software 标志）恒滤；名称过滤仅 "virtual/idd/indirect" 三个子串，真实 GPU 命名（NVIDIA/AMD/Intel）无误伤实例；`ParseGpuInstance` 对 pid 位数>10/溢出/缺 luid 段全部拒绝，hex 解析容忍大小写与前缀；pid 0、无效 LUID 实例经 `realLuids`/`createTimeByPid` 过滤不会进入 gpuProcs；无按实例索引数组之处（越界不可能）；GPU PDH 初始化失败路径正确关 query。
5. **ProcessOps**：`OpenVerified` 在单杀、树杀每成员、Trim 三处均做 GetProcessTimes createTime ±1s 重验（LastHr 为 HRESULT_FROM_WIN32，ACCESS_DENIED 分支判定正确）；树杀 BFS 用 `visited[]` 逐行标记，父链成环不可能死循环；孤儿守卫（子必须晚于父 ±1s）阻断 pid 复用误收；执行时重快照；leaf-first = BFS 逆序 + 根最后，父必死于子后；保护成员逐个 skip+`skippedProtected` 计数不整树拒绝；TerminateProcess 失败后 500ms 宽限等待、成功后 5s 有界确认；SeDebugPrivilege 各路径（含提前返回）均释放。
6. **Signature**：VERIFY/CLOSE 在唯一验证点 `TrustVerifyClose` 内背靠背完成，所有 return 路径（含缓存命中提前返回，其时根本未开 VERIFY）无泄漏；catalog 回退 generic→driver 子系统、`CryptCATAdminAcquireContext/ReleaseContext`、`HCATINFO` 释放、文件句柄 RAII 全部配对；缓存键为小写化路径、512 上限整体清空。
7. **DetailsProvider**：`pending` 在锁内置位先于提交，重复 Request 幂等；在途 job 第一步重查 `cache_`，Invalidate（虽暂无调用方）后写入被正确丢弃；worker 写/UI 读全部经 `mu_`；PEB 读命令行覆盖边界：0 长度→`cmdLineAvail=true` 空串、>32KB 拒绝、非 4 倍数字节长度不会写出缓冲（`2n+2 ≥ cmdBytes`）、命令行读失败保持 `cmdLineAvail=false`。
8. **UI**：`SortLess` 严格弱序成立（不可用恒排最后、双向一致；NaN 由 `IsColumnUnavail` 拦截，`CmpDouble` 桶内无 NaN；pid 升序决胜固定）；`FindByPid` 二分利用快照按 pid 升序契约并做 `key==` 全等校验；树杀确认框锁定请求瞬间的 ProcKey 副本，执行时 TerminateTree 再次重快照+重验，plan 后快照变化被覆盖；`U8()`/FileMetaCache 均按值缓存、容量上限清除，ImGui 在调用点即拷贝文本，无悬垂窗口。
9. **构建产物验证**：`build/Release/stm_selftest.exe` 运行输出 24/24 PASS（含 collect/ops/core/ui 四组），与"当前全绿"预期一致。

## 附：实测输出尾部

```
[PASS] ops_identity_reverify_dead_pid ... [PASS] ui_sort_column_ids_roundtrip
合计：24 通过，0 失败
```

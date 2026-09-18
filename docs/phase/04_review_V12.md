# 终审报告 V12 — 泄漏与生命周期专项

- 审查人：终审 subagent V12（与其他终审者互不知情）
- 日期：2026-09-18
- 范围：ETW 生命周期、阶段 3 页缓存与磁盘增长、五页 job 捕获与页生命周期、全仓句柄终扫、实测
- 环境：**非管理员**（ETW admin 路径仅静态审查 + 自测非管理员分支）；产物 build/Release（07:38，无更新源文件）
- 实测：`SuperTaskMgr.exe --smoke 300` 退出码 **0**；真实实例 62s 采样 vs 92s 采样：WS 124.6→124.5 MB、Private 111.8→111.8 MB、句柄 697→697、线程 20→20，**无增长趋势**（符合 ~124MB 预期）；`stm_selftest.exe` **40/40 通过**（含 etw_toggle_lifecycle、core_jobs_submit_shutdown、ops_details_request）

## 统计

**P0 = 0，P1 = 0，P2 = 7**。

## P0

无。

## P1

无。

## P2

### P2-1 崩溃可残留 ETW 会话；README“退出即清理”表述过强
- **证据**：src/collect/NetTables.cpp:294-366 会话名 `SuperTaskMgr-Net-<pid>`；反孤儿逻辑（Start 前先 STOP 同名会话）只覆盖**同 PID**（NetTables.cpp:298-309）。进程崩溃/被杀时内核侧实时会话残留：64×64KB = 4MB 非分页池，无消费者后事件被丢弃；不会跨重启（非 autologger），但崩溃后至重启前持续占用。README.md:35 声明“会话名唯一且退出即清理”——优雅退出成立，崩溃路径不成立。默认 OFF + 需管理员 + 需崩溃，风险低。
- **修法**：README 该句补“（异常终止时可能残留同名会话至重启，可 `logman stop SuperTaskMgr-Net-<pid>` 手工清理）”；可选加固：启动时用 TdhEnumerateRealTimeTraces 枚举 `SuperTaskMgr-Net-` 前缀、宿主 PID 已死的会话并 STOP。

### P2-2 EtwNetCollector::Stop 忽略 ControlTraceW 返回值，join 无超时（理论挂死）
- **证据**：src/collect/NetTables.cpp:448-452：`ControlTraceW(..., STOP)` 返回值未检查，随后 `consumer_.join()` 无超时。若 STOP 因异常原因失败而会话仍存活，ProcessTrace（NetTables.cpp:384）永不返回 → 关闭路径挂死。概率极低，但后果是 UI/退出冻结。
- **修法**：检查 STOP 的 rc；失败时兜底在 Stop 线程 `CloseTrace(openTrace_)`（文档化的取消方式：消费句柄关闭会使 ProcessTrace 返回 ERROR_CANCELLED），或将 join 放入限时等待后 detach 并记录 WARN。

### P2-3 ETW 消费线程 OpenTraceW 失败时会话“假在运行”
- **证据**：NetTables.cpp:374-378：OpenTraceW 失败仅记日志并 return；session_ 仍非 0，Running()==true，CollectService 照常置 CAP_NET_ETW（CollectService.cpp:190），UI 显示已开启但 netBytesPerSec 永远无数据，且无回滚通知。
- **修法**：Consume 失败时原子记录结果，Stop 会话并回写 netEtwEnabled=false（或回调 PostNote），与 NetworkPage 的 read-back 回滚机制（Pages3.cpp:205-227）打通。

### P2-4 DetailsProvider 失效-重建场景的在途写回竞态（当前潜伏）
- **证据**：src/ops/DetailsProvider.cpp:278-284 写回仅按 key `cache_.find` 后整体覆盖 `data` 并置 `pending=false`，无代次校验。若旧 job 在飞时 `Invalidate(key)` + 新 `Request(key)` 重建条目，旧 job 写回会用旧数据覆盖新条目并提前清 pending → 同 key 双重在途/短暂脏数据。当前 `Invalidate` **无任何调用方**（全仓 grep 证实），仅在未来接入 UI delta 清理时触发，故为潜伏项。
- **修法**：Entry 增加 uint64 代次（job 捕获提交时值，写回时不匹配则丢弃）；Invalidate/重建时 ++代次。

### P2-5 Toolhelp 快照句柄未校验 INVALID_HANDLE_VALUE（与 ProcessOps 不一致）
- **证据**：src/collect/CollectUtil.cpp:393、410 将 `CreateToolhelp32Snapshot` 返回值直接包进 `UniqueHandle`（HandleGuard.h:8-11 只判 NULL，-1 会被 CloseHandle(INVALID_HANDLE_VALUE)），且 397/415 会以伪句柄调 Process32FirstW/Thread32First——依赖 API 自行失败，行为无害但依赖隐式容错。对比 src/ops/ProcessOps.cpp:155、324 有显式校验。
- **修法**：两处补 `if (raw == INVALID_HANDLE_VALUE) return ...;` 与 ProcessOps 对齐。

### P2-6 DriversPage 签名缓存无上限
- **证据**：src/app/ui3/Pages3.cpp:1223 `std::map<std::wstring, std::shared_ptr<SigSlot>> sigs_` 只增不减（仅“重新校验签名”菜单 erase 单条，1122-1131 EnsureSig 只 emplace）。上限 = 用户点选过的不同驱动路径数（本机 226 驱动），条目极小、页生命周期，实际风险可忽略——但与 DetailsProvider 的 1024 上限策略不对称。
- **修法**：低优先；如统一策略可加简单容量（超限清空已完成条目，与 DetailsProvider::Request 的 trim 同型）。

### P2-7 磁盘文件只增不减（已声明，维持现状可接受）
- **证据**：startup_backup 每次切换生成新时间戳文件、无清理（StartupOps.cpp:254-301）；README.md:14 已声明“时间戳保留历史”，属有意保留的可逆链，且单文件 ~200B（任务 XML 数 KB），频率=手动破坏性操作——**声明充分，无需“保留 N 个”策略**（保留链正是其目的）。stm.log 追加模式（Log.cpp:44/53 `L"ab"`）无轮转/上限，当前 8.9KB，长期挂机可缓慢增长。
- **修法**：可选：stm.log 超 5MB 时轮转为 .old；backup 目录维持现状即可。

## 已查无问题清单

1. **ETW ×10 enable→disable 循环（静态论证）**：每次 Start：stopProps_.assign 复用（无增长）→ StartTraceW 新会话句柄 → EnableTraceEx2 ×1 → 消费线程 OpenTraceW→ProcessTrace→CloseTrace（句柄随线程退出关闭，NetTables.cpp:374-389）；每次 Stop：先在锁内置 session_=0（幂等）、**不持 mu_ 调 ControlTraceW(STOP)** → OnEvent（需 mu_）可完成 → ProcessTrace 返回 → join → CloseTrace(会话句柄)（431-455）。句柄/线程/缓冲零残留；失败路径（StartTraceW 拒绝、EnableTraceEx2 失败、线程创建异常）均会停会话并清 stopProps_（327-345、355-363）。Stop 若持 mu_ 调 ControlTraceW 会与在飞 OnEvent 互锁——现设计正确规避（非 admin：StartTraceW ACCESS_DENIED 分支已由 selftest 实测覆盖）。
2. **SetNetEtwEnabled 持 Impl::mu 调 Start/Stop 无死锁**：锁序恒为 Impl::mu → EtwNetCollector::mu_，消费者线程不取 Impl::mu（CollectService.cpp:310-324 论证与代码一致）；Start/Stop 并发被 Impl::mu 串行化，无“Stop 撞 Start 中段”窗口。
3. **AsyncFetch 在途写回安全（逐行）**：job lambda 仅捕获 `app`(shared_ptr)、`st`(shared_ptr<State>)、fn（AsyncFetch.h:61-68），结果只写入引用计数 State 格胞，不触碰页成员；页切换不销毁页对象；Shutdown 丢弃未启动任务（Jobs.cpp:43-46），在飞任务超时 detach 后仍只写活着的格胞（main.cpp:63-69 注释与实现一致）；Submit==0 回滚 busy（68-72）；无重复提交（busy 闸）。`lastData_` 裸指针仅作地址比较，res shared_ptr 保证存续。
4. **页不可见即停止轮询**：Pages.cpp:1351-1359 仅 BeginTabItem 激活页调用 Draw，MaybeFetch 在 Draw 内 → 非可见页零入队；--smoke 离屏全页绘制为有意覆盖（Pages3.cpp:1567-1583）。页缓存永不清空 = 有意的“旧数据保底渲染”，内存上界为每页一个 Result（整体替换），无泄漏。
5. **DetailsProvider 1024 上限**：Request 内 >1024 时批量丢非 pending 条目并**同步精确清理同一 key 集的 attempted 侧表**（DetailsProvider.cpp:196-210）；pending 条目保留且其在飞写回经 cache_.find 容忍缺失；mu_→g_attemptMu 锁序全程一致无死锁；队列未运行回滚 pending+attempted（302-308）。
6. **全仓句柄配对**：StartupOps RegKey×6 处/ComStaGuard(S_FALSE 配对)/ComPtr/BstrGuard/CoTaskMemGuard/UniqueFind 全配对，VARIANT 均 VT_EMPTY/VT_I4 无需 Clear；ServiceOps 8 处 SC_HANDLE 全 ScHandle；Sensors CreateFileW 两处均先判 INVALID_HANDLE_VALUE 再入 UniqueHandle（V9 P0-1 修复在位）、WMI ComPtr/Bs、nvml LoadLibrary/FreeLibrary+init/shutdown 双配对、CoInitializeEx RPC_E_CHANGED_MODE 正确不配对；DetailsProvider OpenProcess×2+OpenProcessToken 全 RAII；Signature CryptCATAdmin 三个 Release 全配对；ProcessCollector SCM 双出口手动 Close；Privilege/OpenProcessToken RAII；Elevate RegOpen/RegClose 无早退间隙；SingleInstance/Tray(ADD/DELETE)/Win32Window(DestroyWindow) 配对；ws2_32/tdh/ntdll/nvml DLL 按注释进程级驻留（有意）。
7. **五页 job 捕获**：NetworkPage 切换、StartupPage Submit、ServicePage Submit+DepPlan、DriverPage EnsureSig 均按值捕获 `app` shared_ptr；P3Slot 静态持有 ctx（有意），分离 worker 存续期内对象永不析构，无 UAF。

## 结论

未发现 P0/P1。泄漏与生命周期面整体扎实：ETW 停止路径的“不持锁 ControlTraceW + join + 句柄后关”顺序正确，五页异步写回的 shared_ptr 格胞模型无写回悬垂，全仓句柄 RAII 覆盖完整。7 项 P2 中 3 项是文档/一致性问题（P2-1、P2-5、P2-6），4 项是低概率防御性加固（P2-2、P2-3、P2-4、P2-7），均不阻塞发布。

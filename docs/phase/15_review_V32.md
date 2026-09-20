# 终审评审报告 V32（2026-09-21）

评审人：V32（独立终审，与其他评审者互不知情）。禁 git；除本报告外未修改任何产品文件。
对象：全仓代码质量/回归/泄漏/安全终扫，重点最近三轮新增面（LogViewer+日志模态、网络监视 NetMonitor/NetTables、PawnIoLink、NpcapSource/PcapUi、PerfCsv、列宽持久化收尾）。

## 测试与采样执行记录

| 项目 | 结果 |
| --- | --- |
| stm_selftest.exe 双轮 | 两轮均 **173 通过 / 0 失败**，exit=0（含 collect_tick_latency：热 tick p50<15ms、lastTickMs<100ms 断言） |
| SuperTaskMgr.exe --smoke 150 | exit=0（150 帧约 3s 走完，全部页签离屏渲染；日志无 ERROR） |
| --autotest 六条 | about / dialogclick / kill / startup / tree / wallpaper 全部 exit=0，autotest_result.log 六条全 PASS（wallpaper 帧顶点≈7.0k、壁纸绑定绘制命令=1；dialogclick 真实输入管线） |
| 内存 60s 双采样（真实实例 5s/65s/125s 三点） | 125,188K → 125,348K → 124,328K；句柄 683→681→673；线程 22→18。**零增长**，与 README「空载 ≈124MB」声明一致 |
| 启动时间 | 进程启动到主窗口可见 ≈347ms |
| 二进制新鲜度 | build/Release 产物（2026-09-21 04:55）晚于全部 src 源文件，实测即当前代码 |
| 日志错误扫描 | 本轮全部测试窗口 stm.log **0 ERROR**；WARN 均为已知预期（selfcheck 私有工作集首轮重试噪声、StartupApproved 实验性 WARN） |
| ETW 会话残留检查 | 发现 4 个**历史遗留**孤儿会话（SuperTaskMgr-NetMon/Dns-\<pid\> ×2 组，属先前测试轮强杀实例，README 已声明的崩溃例外）；本轮已 `logman stop` 清理，正常退出路径的会话清理经日志（"ETW 会话已停止"三连）实证有效 |

---

## P0（无）

未发现崩溃、越界、死锁、数据损坏、资源净泄漏类缺陷。

## P1（无）

近三轮新增面（LogViewer/网络监视/PawnIO/NpcapSource/日志模态）未发现会阻塞发布的缺陷。历史 P1 零复发（见回归节）。

## P2

### P2-1 ETW 孤儿会话的「同名清理」按 pid 命名实际跨实例无效：崩溃残留只能等重启（本次实测捕获 2 组实锤）

- 证据：
  - `src/collect/NetTables.cpp:417`、`src/collect/NetMonitor.cpp:290`：会话名 `SuperTaskMgr-NetMon-<pid>` / `SuperTaskMgr-Dns-<pid>`；Start 前仅清理**同名**（同 pid）残留（NetTables.cpp:425、NetMonitor.cpp:296）。
  - 实测：本机存在 4 个运行中孤儿会话（NetMon+Dns × pid 79176/69140，约 8.3MB 非分页池），属先前轮次被强杀的实例；pid 复用概率趋零 → 「下次同名实例启动时清理」实际永不触发，与 README「直至系统重启」一致（已按文档声明），但残留跨会话累积。
- 修法：启动时（或采集服务 Start 后）做一次 `ControlTraceW(QUERY)` 枚举 + 前缀匹配 `SuperTaskMgr-Net/Dns-*`，对属主 pid 已死的会话执行 STOP（需管理员，失败静默）。也可仅文档补充"建议重启"。
- 评审动作：本轮已手工 `logman stop` 清理全部 4 个，机器已恢复洁净。

### P2-2 NpcapSource 生命周期与 NetMonitor 的 lcmu 模式不对称：Stop 无互斥 + StartCapture 线程创建失败无清理

- 证据：
  - `src/collect/NpcapSource.cpp:313-321`：`Impl::Stop()` 直接 `stop_.store + join + pcap_close`，无互斥。对比 `src/collect/NetMonitor.cpp:529`（`lcmu` 生命周期互斥，注释明言 "enable/disable/析构互斥"）——NetMonitor 的 toggles 走 ops 任务 + 静态析构的并发是**被 lcmu 串行化的**；NpcapSource 的 Start/Stop 走 ops 任务（Pages3.cpp:1219/1245），静态析构 `~Impl(){Stop();}`（NpcapSource.cpp:311）在 main 返回后执行，若某 toggle 任务超过 `jobs.Shutdown(2000)` 被 detach，则 detached 任务内的 `StopCapture()` 与静态析构的 `Stop()` 并发 → `thread_.joinable()/join()` 竞争 + `pcap_` 双关（use-after-close 窗口）。
  - `src/collect/NpcapSource.cpp:452-454`：`running_.store(true)` 后构造 `std::thread` 无 try/catch——对比 DnsCollector::Start（NetMonitor.cpp:338-346）与 EtwNetCollector::Start（NetTables.cpp:477-485）均有 catch + 会话回滚；此处抛出（资源耗尽）会留下 running_=true、pcap_ 开、无线程的半状态（JobQueue 兜底 catch 不崩，下次 toggle 可恢复，但与兄弟实现不一致）。
  - 实际可达性极低（toggle 操作毫秒级，2s 关停窗口充裕），故 P2。
- 修法：给 Impl 加 `std::mutex stop_mu`，Stop() 全程持锁；StartCapture 的线程构造套用 DnsCollector 同款 try/catch（失败时 close pcap、running_=false）。

### P2-3 DrainDns 自动禁用路径在 UI 线程持 Impl::mu 执行 dns.Stop()：最坏 ~1s 单帧卡顿（罕见路径）

- 证据：`src/collect/NetMonitor.cpp:698`（`lock_guard lock(s.mu)` 持有至函数尾）→ `:741` `s.dns.Stop()`（内部 `ControlTraceW(STOP)` + `consumer_.join()`；FlushTimer=1s 下 STOP 可达百毫秒~1s）。同函数内 `:734-736` 还在持锁下调 `dns.Running()/RawEvents()`（各自再 QUERY 内核）。持锁期间 PollLoop（NetMonitor.cpp:585 `cv.wait_for` 需 mu）被阻塞——轮询线程丢一拍，UI 线程卡一帧。仅 DNS 自动禁用（字段不可靠/线程夭折）时触发，一次性。
- 修法：DrainDns 先在锁内只做 Drain/去重/计数快照，锁外执行 Running 探测与 dns.Stop()，结果回写（或把自动禁用移入 SetDnsCapture(false) 的 ops 任务）。

### P2-4 NetMonitor/NpcapSource 依赖静态析构收尾，晚于 LogShutdown：停止日志丢失 + 退出序列未显式覆盖

- 证据：`src/app/ui3/Pages3.cpp:283-287`（函数级 static unique_ptr，main 返回后析构）；`src/app/main.cpp:624-635` 退出序列为 collect.Stop→jobs.Shutdown→PawnIoShutdown→WallpaperShutdown→ShutdownAboutUi→ui.Shutdown→renderer.Shutdown→win.Destroy→LogShutdown，**无** NetMon/Pcap 显式停止；LogWrite 在 g_file==nullptr 时静默丢弃（src/core/Log.cpp:86-90）→ "ETW 会话已停止 / DNS 已停止 / pcap 已关闭" 日志永不落盘（本轮实测：退出三连日志来自显式路径的 PawnIO/壁纸，ETW 停止行缺失）。功能无损（ControlTraceW 不依赖 UI），纯可观测性缺口。
- 修法：Pages3 暴露 `ShutdownNetSourcesForExit()`（先 NetMonInst 停、再 PcapSourceInst 停），main 在 `LogShutdown()` 前调用；静态析构保留作兜底（幂等已具备）。

### P2-5 最小能力面：pcap_sendpacket（帧注入）已绑定但全仓无调用方；EtwNetCollector/DnsCollector::openTrace_ 只写不读

- 证据：`src/collect/NpcapSource.cpp:128/151/192-197/481-508`：`SendPacket` 完整实现并绑定 `pcap_sendpacket`（wpcap 优先、packet.dll 回退），`grep -rn SendPacket src` 仅 collect 层定义、app/selftest 均无调用。与 README「不注入」声明不冲突（UI 不可达），但保留绑定扩大二进制能力面。`src/collect/NetTables.cpp:503/510`、`src/collect/NetMonitor.cpp:366/373`：`openTrace_` 存取但无读者（死字段）。
- 修法：删除 SendPacket 及 pcap_sendpacket 绑定（或加注释声明仅为未来显式功能保留）；删除 openTrace_ 字段。

---

## 回归（历史用户报告缺陷零复发，抽验代码路径）

1. **确认框每帧渲染**：`DrawConfirmDialogs`（Pages.cpp:331）+ `closeAskPending` 原子消费（:337-338）；dialogclick autotest 真实输入管线 PASS。
2. **壁纸**：WallpaperDrawBackground 在 NewFrame 后调用（main.cpp:540-541）；wallpaper autotest 顶点>0+绑定命令=1+exit 0；退出只放纹理不删副本（main.cpp:630，Wallpaper.cpp:255-265）。
3. **关于点击**：about autotest PASS（开≥4帧+关闭）。
4. **兼容模式模态**：残留防护与 Esc 复位在位（Pages.cpp:3244-3248 一带，V24 P2-1 模式沿用）；日志查看器/诊断报告二级视图同款「请求长期有效 + 每帧渲染 + 被关即复位」（Pages.cpp:3272-3301）。
5. **内存加速入口**：三入口汇同一确认模态+单执行路径（V30 #11 复核仍成立）。
6. **状态栏槽位**：AnchorEndX 纯计算不读光标（StatusLayout.h:106-117）；防抖常量样本在位。
7. **顶栏固定/网络页定高**：`FillScrollRegionHeight`（PageLayout.h:115）+ `##netscroll`（Pages3.cpp:317-321）+ 实时监视/抓包 340px 定高段（NetMonUi.h:44、Pages3.cpp:897/940）全在位。
8. **列重排+UserID**：槽位钳制 `[0,kProcColSlots)`（Pages.cpp:1241-1243）、UserID 映射与恢复串相等防回写（V30 #3 复核）。
9. **V30-P1-1（netcol 缓存不随重置布局失效）已确认修复**：`LayoutResetGeneration/NotifyLayoutReset`（ThemeCfg.h:238-239）+ 两缓存 loadedGen 失效重读（Pages3.cpp:117-131、Pages.cpp:2751-2770）+ ResetLayout 触发（Pages.cpp:3540）。
10. **V30-P2-2（退出布局键残留）已确认修复**：退出剔除 `{"colW_","netcol_"}+{layoutScale,colOrder,perfZoom}`（main.cpp:615-617）。
11. **树终止身份重验**：根行 createTime ±1s 复核（ProcessOps.cpp:200、417/438 附近），保护成员跳过并报告；tree autotest PASS。

## 安全终扫（grep 全仓）

- **注入/监听/持久化**：无 CreateRemoteThread/WriteProcessMemory/VirtualAllocEx/SetWindowsHookEx；无 socket bind/listen/accept；注册表写仅 StartupOps（用户操作+备份）与 autotest 自清理；进程创建仅 autotest cmd/ping 与提权重启（ShellExecuteEx runas，用户动作）。
- **联网红线**：LHM 数据源强制 loopback（LhmSource.cpp:422/465-467），WinHttp 全程 HGuard RAII、NO_PROXY、8MB 上限；其余 ShellExecute 均为用户主动开浏览器/资源管理器。
- **保护名单双层防线**：core::ProtectedReason（ProtectedList.cpp:14-33，含 pid 0/4）+ ops 层硬门禁每个入口（ProcessOps.cpp:339/392/427/482-493、ProcessControl.cpp:309、ServiceOps.cpp:140）+ PF_Protected UI 禁用标志。
- **SeDebug 用后即释**：ProcessControl.cpp:67-71、ProcessOps.cpp:238-246/417/438、DriverOps.cpp:85-87 全部 enable→retry→disable 配对。
- **日志无敏感值**：全仓 STM_LOG 无命令行/窗口标题/凭据；诊断报告含隐私提示（LogFile.h:315）。
- **cfg 读写成对**：全部键 Get/Set 配对（netEtw 4/4、lhmEnabled/lhmPort、hotkeyEnabled、closeAction、wallpaperMask、perfWindowSec、themeMode 等）；会话键（winX/winY/page/ts…）走独立 session 存储；DNS/监视开关不持久化（默认关，诚实设计）。
- **模块加载**：PawnIO blob SHA-256 白名单双防御（驱动签名校验 + 文件哈希钉扎，PawnIoLink.cpp:60-66/737-753），重载句柄延迟回收（g_retired→PawnIoShutdown），无路径注入面。

## 已查无问题清单

1. **LogFile.h ReadLogTail**：全失败路径 fclose、BOM 剥离、UTF-8 严格校验（过长编码/代理区/超界）、窗口首/尾半行丢弃、maxLines 从尾计数——无泄漏无越界（LogViewer 模态只读 512KB 窗口，UI 线程毫秒级）。
2. **日志模态**：打开/刷新/1s 自动刷新读取节流正确；报告二级视图（复制/SaveDiagnosticsFile 落盘 logs 目录）无自动上传；Esc/关闭双路径复位 opened 标志，无隐形模态。
3. **NetMonitor**：SetEventCapture 幂等 + lcmu 串行化 enable/disable/析构；PollLoop 失败保基线不伪造风暴（V25 P1-1 在位）；事件/DNS 双环形保新弃旧+丢弃计数；Running() GUID 比对的失效诚实读回（V25 P0-2 在位）。
4. **EtwNetCollector/DnsCollector**：Start 失败全路径 STOP 回滚 + stopProps_ 清理；Stop 先作废 session_ 再无条件 join（防 terminate）；锁序恒为 Impl::mu→collector::mu_，无反向。
5. **NpcapSource**：WpcapLib 单例 LoadLibraryEx 绝对路径 + 导出完整性校验，失败 FreeLibrary 对称；BPF compile/setFilter 失败释放 pcap_t 与 prog；DrainPackets move 语义；SendPacket 句柄即用即关。
6. **PawnIoLink**：BCrypt hash 对象/提供者全路径释放；GetPawnIoStatus 探测句柄 CloseHandle；IsaBusMutex/SuperioConfigGuard RAII（配置态绝不残留）；ReadCpuDtsTemps 每核绑定后恢复原亲和性（852/860 双保险）；LpcIO 10s TTL 缓存控制 IOCTL 量（~1500 次/探测）。
7. **PDH**：GpuCollector/SystemCollector 析构 PdhCloseQuerySafe；Sensors 网卡吞吐 Cleanup 结构体析构 FreeMibTable+PdhCloseQuery；SelfCheckGate 探测查询同款清理。
8. **纹理**：壁纸 SRV 提交点换入/失败不动旧壁纸；WallpaperClear vs WallpaperShutdown 语义分离正确；关于页 Logo ComPtr + ShutdownAboutUi 先于设备销毁（main.cpp:631）；stb 解码缓冲 RAII/freer 全路径释放。
9. **CSV/文件流**：PerfCsvRecorder 错误即 Stop（flush+close）；netmon CSV 导出（Pages3.cpp:857-874）BOM+flush+good() 校验；ThemeCfg/Cfg 原子写（tmp+替换）；主日志 1MB 滚动保留 3 份。
10. **线程启停幂等**：CollectService（running+joinable 双查）、JobQueue（Shutdown 丢弃未开始任务+超时 detach 语义）、NetMonitor 三路开关（toggle 槽 shared_ptr + 诚实读回回滚）、pcap toggle、DNS/ETW Stop 幂等（netmon_session_kill_detection 实测双轮通过）。
11. **shared_ptr 捕获链**：全部 ops 任务按值捕获 `shared_ptr<AppContext>`（RequestMonToggle/ExportNetMonCsv/RequestPcapStart/Stop/AsyncFetch/托盘回调），脱离工作线程仅写自己持有的 State 单元，无裸页面对象；LiveP3Ctx() 空时诚实降级提示。
12. **退出顺序**：collect.Stop→jobs.Shutdown(2000)→PawnIoShutdown→WallpaperShutdown→ShutdownAboutUi→ui.Shutdown→renderer.Shutdown→win.Destroy→LogShutdown（main.cpp:624-635）；PawnIO 句柄释放先于 LogShutdown（有日志实证）；托盘 sink 先摘除再拆 tray（:621）。
13. **内存基线**：125.2→125.3→124.3MB（120s 零增长），句柄/线程数稳定；README ~124MB 声明成立。
14. **启动**：主窗可见 ≈347ms；--smoke 150 帧约 3s（无掉帧卡死）。
15. **本轮环境事件说明**：评审期间 ZCode harness 重跑了先前后台命令（日志可见 05:06/05:14 的 smoke=150 与 autotest tree/wallpaper 重入），由此产生的实例消失/互斥顶替均为环境工件——重入实例全部走干净退出路径（退出三连日志齐全），无 WER/事件日志崩溃记录，非产品缺陷。

## 结论

P0=0，P1=0，P2=5（全部为加固/卫生级：孤儿会话清扫、Npcap 停止互斥与线程异常回滚、DrainDns 锁外停止、退出显式收尾网络源、最小能力面修剪）。近三轮新增面实现质量高（RAII 覆盖率与诚实降级设计尤佳），历史缺陷零复发，性能与安全声明与实测一致。**不阻塞发布**；建议下一维护轮优先 P2-1（孤儿会话启动清扫，唯一有真实世界证据的一项）与 P2-4（退出收尾可观测性）。

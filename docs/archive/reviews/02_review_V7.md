# 阶段 2 审查报告 — V7（维度：资源泄漏与生命周期）

审查对象：`src/collect/*`、`src/ops/{ProcessOps,Signature,DetailsProvider}.cpp`、`src/app/**`、
`src/core/{Jobs,Notifications,Log,Cfg,FsUtil}.cpp`，对照契约 `ProcData.h`/`CollectService.h`/`ops/*.h`
与资源章程（01_架构设计文档.md §5）。
运行验证：`stm_selftest.exe` 24/24 通过 exit 0；`SuperTaskMgr.exe --smoke 120` exit 0（均未构建/修改产物）。

## 统计

| 级别 | 数量 | 说明 |
|---|---|---|
| P0 | 0 | 无正常路径必现、且进程内持续累积的泄漏 |
| P1 | 4 | 退出/错误路径未释放、无界增长、生命周期悬垂 |
| P2 | 5 | 建议、性能项、前瞻性提示 |

---

## P1（错误路径泄漏 / 无界增长 / 生命周期后果）

### P1-1 SystemCollector / GpuCollector 的 PDH query 无析构关闭（违反章程"query 生命周期跟随其 Collector"）
- 位置：`src/collect/CollectDetail.h:136-155`（SystemCollector）、`CollectDetail.h:157-175`（GpuCollector）——两个类都没有析构函数；`src/collect/SystemCollector.cpp:44`（PdhOpenQueryW）、`src/collect/GpuCollector.cpp:151`。
- 场景：正常路径首次采集成功后，`pdhQuery_`（3 个计数器：diskRead/diskWrite/hardFaults）与 `query_`（utilCounter/dedCounter/shrCounter，GPU Engine 实例可达数千个）在 Collector 销毁时**从不 PdhCloseQuery**。错误路径（`pdhFailed_`）有 `PdhCloseQuerySafe` 关闭（SystemCollector.cpp:48、GpuCollector.cpp:156），唯独对象析构路径缺失。主程序为 1 次/进程、退出由 OS 兜底；但 **stm_selftest 在同一进程先后构造 6 个 CollectService**（collect_test.cpp），每个跑过 ≥1 tick 即留下 1–2 条泄漏的 query+计数器句柄，属可实测的句柄累积。若阶段 3 出现"停止/重启采集服务"功能则变成主程序常态泄漏。
- 修法：为两个 Collector 各加 `~X() { PdhCloseQuerySafe(&query_); }`（PdhCloseQuery 会级联释放子计数器，无需逐个 Remove）。放在头文件 inline 即可，不动契约。

### P1-2 DetailsProvider 的 cache_ 与 g_attemptedKinds 无界增长：Invalidate 定义了但全仓库无人调用
- 位置：`src/ops/DetailsProvider.h:58`（`cache_`）、`src/ops/DetailsProvider.cpp:160`（全局 `g_attemptedKinds`）、`DetailsProvider.cpp:292-298`（Invalidate）、`src/app/ui/Pages.cpp`（仅有 Request：814、897；Peek：894、904 等，**无任何 Invalidate 调用**）。
- 场景：UI 每选中/右键校验一个进程就 `BeginEntry` 建缓存项并 `MarkCmdLineAttempted` 写入全局 map；进程死亡后条目永不回收。每条 Entry 含 cmdline（可达 32KB）+ userName + modules（数百条路径字符串），长时间挂机逐个查看进程即线性累积；`g_attemptedKinds`（std::map 节点 ~100B/条）同样只增不减。附带后果：worker lambda 里"invalidated while queued"分支（DetailsProvider.cpp:210）永远不可达。
- 修法：在每 tick（或每 N tick）由 UI 用当前 Snapshot 的 ProcKey 集合对 `cache_` 做差集 `Invalidate`（ProcKey 含 createTime，PID 复用不误删）；或仿 Signature 缓存加上限（如 512 条 + clear）。

### P1-3 JobQueue::Shutdown 的 detach 分支造成退出期 UAF 窗口
- 位置：`src/core/Jobs.cpp:61-70`（busy 超时 → `worker_.detach()`）、`src/core/Jobs.h:16`（`~JobQueue(){ Shutdown(0); }` 同分支可达）。
- 场景：main.cpp:164 调 `jobs.Shutdown(2000)`。在途任务很容易超过 2s——`TerminateVerified` 单进程最多等 5000ms（ProcessOps.cpp:34、287），树杀多个僵死目标必然超时 → detach。此后 main 继续 ui.Shutdown → renderer.Shutdown → win.Destroy → LogShutdown → return 0，`AppContext`（含 jobs、notes、details）随栈析构，而分离中的 job lambda 仍持有 `DetailsProvider* this`、`NotificationQueue* notes` 并继续 `notes_.Push`、写 `mu_/cache_`（Pages.cpp:109-171、DetailsProvider.cpp:205-274）→ 对已析构对象的 use-after-free；`~JobQueue` 的 Shutdown(0) 亦可能析构 deque 本体。注释"process teardown will follow"依赖 OS 收尾，但 CRT/静态析构与该线程存在竞争，可造成退出崩溃。
- 修法：任务 lambda 只捕获 `shared_ptr`（notes、provider 的共享状态、plan 模式已有先例 Pages.cpp:182-185）；或 Shutdown 超时分支把在途 job 连同其所需上下文"堆化"后 detach（队列对象不随 this 析构被引用）。至少：Shutdown 超时后置一个 `abandoned_` 标志并让 push 侧快速失败。

### P1-4 ImGuiLayer::Init 失败路径不回滚 ImGui/ImPlot 上下文与字体图集
- 位置：`src/app/ImGuiLayer.cpp:26-27`（CreateContext/ImPlot::CreateContext）、`53-54`（`ImGui_ImplWin32_Init` / `ImGui_ImplDX11_Init` 失败直接 `return false`）、`src/app/main.cpp:105`（`if (!ui.Init(...)) return 1;` 不调 Shutdown）。
- 场景：后端初始化失败（如 VeryOldRDP/窗口类异常）时，已创建的 ImGui+ImPlot 上下文、字体图集、以及已成功的那一半后端均不释放，直到进程退出由 OS 兜底；错误路径泄漏 + 后端状态半初始化。
- 修法：Init 失败各分支调用与 Shutdown 相同的逆序回滚（先成功者 Shutdown）；或给 ImGuiLayer 加析构 + `inited_` 标志，main 的 early-return 依赖析构兜底。D3DRenderer 无此问题（ComPtr 成员随栈析构释放）。

---

## P2（建议 / 性能项 / 前瞻）

- **P2-1 NtQSI 每 tick 新分配 ~1MB 缓冲**：`src/collect/CollectUtil.cpp:193-207`（`new BYTE[size]` 每次调用）。`unique_ptr` 保证无泄漏，但 1s 一 tick 的稳态下是无谓的大块分配/页错误。建议 CollectUtil 内缓存"上次的 size"复用缓冲（仅增长时重分配）。同类：ProcessCollector 每 tick 重建 rows/ProcInfo 向量属正常。
- **P2-2 DetailsProvider::Peek 返回裸指针逃逸出锁**：`src/ops/DetailsProvider.cpp:286-290` 返回 `&cache_` 内部指针，锁已释放。当前因 Invalidate 从不被调（见 P1-2）而实际安全；一旦按 P1-2 修复，UI 持指针跨帧/跨 Request 即有悬垂风险。建议 Peek 返回 `ProcessDetails` 值拷贝，或文档强约束"当帧用毕"。
- **P2-3 窗口类未注销的 early-return 不对称**：`src/app/Win32Window.cpp:54` RegisterClassExW 成功后，若 CreateWindowExW 失败（59-61）或 main.cpp:96/100 的 early return，`win.Destroy()` 不执行 → 窗口类/窗口保持注册至进程退出。进程级一次性、OS 兜底，建议在 `Create` 失败分支内补 UnregisterClass 保持对称。
- **P2-4 NtqsiQueryPerCoreTimes 二次调用以 need 为长度传入 size 大小的缓冲**：`src/collect/CollectUtil.cpp:281-283`——`need >= size` 时用 `need` 作为缓冲长度重调，但缓冲只按 `size` 分配。当前 CPU 热增核场景下 need==size 恒成立、不可达，属潜在堆越写的休眠缺陷；建议第二次调用前 `if (need > size) return false;` 或按 need 重分配。
- **P2-5 COM：当前无 CoInitialize 使用点（确认为"配对无缺口"而非缺陷）**：全仓库无 CoInitialize/CoUninitialize；唯一的 COM 分配 `SHGetKnownFolderPath` 已正确 `CoTaskMemFree`（src/core/FsUtil.cpp:34-44）。章程中"OpsWorker 内 COM 走 CoInitializeEx(STA)+CoUninitialize"在阶段 2 尚无落点（WinVerifyTrust/CryptCATAdmin 当前实践可不显式初始化）；阶段 3 引入 ETW/Shell COM 前需在 JobQueue worker 落实配对，届时注意 P1-3 detach 会撕裂该配对。

---

## 已查无问题清单（按资源类别 × 文件）

- **句柄 OpenProcess/OpenProcessToken/快照/互斥体/注册表/文件**：ProcessCollector.cpp（ComputeSupp 切片 OpenProcess→UniqueHandle；IsElevatedProcess 令牌→UniqueHandle；ToolhelpEnumerate/ToolhelpThreadCountOf→UniqueHandle，Process32First 失败路径亦由 RAII 释放）；SelfCheckGate.cpp:48（每候选 UniqueHandle，continue/return 各路径均析构）；ProcessOps.cpp（SnapshotProcesses 快照 153-158→UniqueHandle；OpenVerified 各 verdict 路径 UniqueHandle；树杀循环、TrimWorkingSet、QueryImagePath、FindImageNameBySnapshot 全 RAII）；DetailsProvider.cpp（两级 OpenProcess 218-224→handle.reset；FetchUserName 令牌 96-97→UniqueHandle）；Signature.cpp（CreateFileW→UniqueHandle，INVALID_HANDLE 分支跳过）；Privilege.cpp（两处 OpenProcessToken→UniqueHandle）；SingleInstance.cpp（CreateMutexW 特意持有至进程结束——语义正确，非泄漏）；Elevate.cpp（RegOpenKeyExW→RegCloseKey 全路径）；FsUtil/Err（FormatMessage ALLOCATE_BUFFER→LocalFree 配对 20）。每条 return/early-exit 均有归属，无裸 HANDLE 泄漏点。
- **动态库**：ntdll 全部走 `GetModuleHandleW`（ntdll 常驻、不可卸载），函数指针存 function-local static 一次解析（CollectUtil.cpp:25-34、ProcessOps.cpp:104-109、DetailsProvider.cpp:28-36）；无 LoadLibrary/FreeLibrary 反复加载。签名与文档原型一致（LONG(WINAPI*)(ULONG,PVOID,PULONG) 等）。PurgeStandbyList 每次重新 GetProcAddress（ProcessOps.cpp:434-437）——低频用户操作，非泄漏，可不改。
- **PDH 配对**：SelfCheckGate 一次性 query 在成功/失败/提前 break 所有路径后统一 `PdhCloseQuerySafe(&q)`（SelfCheckGate.cpp:121-157）；PdhLocalizeEnglishPath 探针 query 321-337 全路径 close；SystemCollector/GpuCollector 错误路径 close+计数器置空（唯一缺口为析构，见 P1-1）；不存在 query 重建循环（通配符模式免重建，符合章程）；PdhFmtArrayDouble 缓冲为 `std::vector<BYTE>` 栈语义 + 4 次扩容重试（CollectUtil.cpp:346-373），无 new/delete 配对问题。
- **COM/DXGI**：GpuCollector.cpp:113-134 工厂与每枚举适配器 `a->Release()`/`factory->Release()` 逐路径配对（EnumAdapters1 非 NOT_FOUND 错误时 DXGI 置空出参，无悬挂引用）；D3DRenderer 全 ComPtr，Shutdown 逆序（target→swapchain→context→device），Resize 先 ReleaseTargets；GetIfTable2→FreeMibTable 配对（SystemCollector.cpp:102-111）。
- **内存配对与上限**：ProcessCollector 的 supp_/prev_/denied_ 每 tick 以 `seen` 差集剪枝（365-373，死进程不残留）；SnapshotStore shared_ptr 换入换出；toasts 上限 6（Pages.cpp:85）；PerfHistory 环上限 120；Signature 缓存 512 上限 + clear（Signature.cpp:162）；FileMetaCache 2048 上限（VersionInfo.h:88-91）；UiText 8192 上限（UiText.h:17-21）；NtQSI 缓冲 unique_ptr（性能项见 P2-1）；无裸 new/delete。
- **线程与退出协议**：CollectService::Stop 通知+join，Run 的 sleep 循环 3 处 running 检查保证退出（CollectService.cpp:95-133、222-231）；~CollectService 兜底 Stop；main 正常退出顺序 tray.Remove→collect.Stop→jobs.Shutdown(2000)→ui.Shutdown→renderer.Shutdown→win.Destroy→LogShutdown（main.cpp:158-169），ImGui→后端→D3D 逆序符合章程；--smoke 跳过 tray.Create 与 tray.Remove 的 `added_` 守卫对称（Tray.cpp:40-48）；窗口类在 MainWindow::Destroy 注销（Win32Window.cpp:76）；JobQueue 常规 join 路径由自测 core_jobs_submit_shutdown 验证（唯一隐患为 detach 分支，见 P1-3）；提权重启 wantExit 路径走同一完整清理序列。
- **GDI/USER 与托盘**：GetGuiResources 只读无副作用；Tray 单实例 uID=1（hWnd+uID 对），TaskbarCreated 广播后 added_ 复位重加（Tray.cpp:65-71）；LoadIconW 共享图标无需 DestroyIcon；ShowMenu 的 CreatePopupMenu→DestroyMenu 配对（非空守卫后所有路径）；自定义消息钩子 `cbs.onMessage` 捕获 `&tray`，其生命周期覆盖窗口（win.Destroy 先于 tray 析构，销毁后 WndProc 不再触发）。
- **运行验证**：`stm_selftest.exe` 24/24 exit 0；`SuperTaskMgr.exe --smoke 120` exit 0（未构建，仅运行既有产物）。

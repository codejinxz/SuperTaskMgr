# 阶段 2 审查报告 —— V8（安全防线 + 性能）

- 审查人：V8（独立审查，与其他审查者互不知情）
- 范围：`src/collect/*.cpp`、`src/ops/*.cpp`（除 Elevate/SingleInstance/SessionState，但 session.json 写入内容按任务要求核查）、`src/app/**`；参照 `docs/phase/01_架构设计文档.md` §6/§7/§10。
- 方法：全文精读 + 全量 `STM_LOG` / 敏感 API grep（socket/listen/注册表 Run/启动文件夹/schtasks/窄字符文件 IO）+ ImGui 1.92.9 nav 源码核对 + 运行既有产物（`stm_selftest.exe` 24/24 通过；`SuperTaskMgr.exe --smoke 120` 退出码 0）。未构建、未修改任何文件。

## 统计

| 级别 | 数量 |
|---|---|
| P0 | 0 |
| P1 | 2 |
| P2 | 11 |

---

## P1 发现

### P1-1 「释放工作集」对保护名单进程是零层拦截（双层防线缺口）
- 位置：`src/app/ui/Pages.cpp:802-806`、`src/ops/ProcessOps.cpp:409-422`
- 场景：右键 `dwm.exe`（内置名单成员、非 PPL、同用户/管理员可开 `PROCESS_SET_QUOTA`）→ 菜单中「终止进程/终止进程树」被 `BeginDisabled(protectedProc)` 禁用，但「释放工作集」（806 行）在该禁用块**之外**；同时 `TrimWorkingSet` 全函数没有任何 `ProtectedReason` 调用。
- 证据：`Pages.cpp:790-818` 中仅 803-804 两项被禁用；`ProcessOps.cpp:412` 直接 `OpenVerified(...PROCESS_SET_QUOTA...)` → `EmptyWorkingSet`。即该操作 UI 层、ops 层**两层防线都不过**，与用户约束「不可误伤系统关键进程（内置保护名单）」直接冲突。名单中其余成员多为 PPL 会被 OS 拒绝 SET_QUOTA（诚实报错），`dwm.exe` 是实际可达面（工作集被换出 → 桌面合成卡顿/闪烁）；防线完整性问题上这是 §6 协议的缺口。
- 修法：① `TrimWorkingSet` 开头加与 `TerminateProcessById` 同款硬门：`std::wstring reason = ProtectedReason(key, L"", L""); if (!reason.empty()) { if (err) *err = L"已拒绝释放工作集：" + reason; return false; }`；② UI 把「释放工作集」挪进 `BeginDisabled(protectedProc)` 块；③ selftest 增补 `ops_trim_protected_refusal` 用例（现有 `ops_protected_refusal` 只覆盖 kill 路径）。

### P1-2 确认框默认键盘焦点落在破坏性按钮上，两连击 Space 即执行
- 位置：`src/app/ImGuiLayer.cpp:29`（`NavEnableKeyboard`）、`src/app/ui/Pages.cpp:360-375`
- 场景：确认框内「终止进程」按钮先于「取消」提交。ImGui 1.92 对弹窗自动做 nav 初始化（`imgui.cpp:13853` `init_for_nav=true`；首项匹配清位 `imgui.cpp:13579`），键盘导航焦点自动落在第一个可导航项＝动作按钮；1.92 的键盘激活键是 **Space**（`imgui.cpp:14026-14027`；Enter 只切换 nav 输入源，`imgui.cpp:13972`）。
- 证据：弹窗后按 Enter（武装键盘导航）+ Space、或 Space+Space，即可零鼠标触发终止。「二次确认」作为最后防线的兜底意图被默认焦点方向削弱。任务问「Enter 误触发的可能性」——精确结论：Enter 本身不激活（1.92 行为），但 Space 激活且默认焦点在破坏性按钮上，风险等价。
- 修法：交换按钮顺序（「取消」在左/先提交），或对动作按钮 `ImGui::SetNextItemNoNav()`，或进入弹窗时 `ImGui::SetKeyboardFocusHere` 到取消项。三者任一即可。

---

## P2 发现

### P2-1 名单按镜像名匹配 → 恶意同名获得「免杀护盾」（防误伤方向是安全的，但存在规避面）
- 位置：`src/core/ProtectedList.cpp:15-25`（`(void)path`，路径完全忽略）
- 场景：非系统路径下的伪造 `csrss.exe` 会被**过度保护**（无法终止）——不会误杀，方向安全；但恶意程序把自己命名为 `csrss.exe`/`lsass.exe` 即可获得本工具全部破坏性操作的豁免（用户无法用它清理），构成轻微的规避检测面。
- 评估与建议（P2）：实际风险低（本工具非杀软，OS PPL 仍是真底线）。强化建议：名单命中时追加校验——路径位于 `%SystemRoot%\System32`（`QueryFullProcessImageNameW` 已有解析，`ops::ProtectedReason` 464-468 行已具备路径解析能力）或父进程链为 `smss.exe`；不满足则降级为「疑似伪装」徽标而非硬拒绝。`path` 参数与 16 行注释（"future parent-chain / signature hardening"）说明架构已预留，落地即可。

### P2-2 提权重启在 UI 线程同步等待 UAC
- 位置：`src/app/ui/Pages.cpp:1176-1179`、`src/app/ui/Tray.cpp:106-108`（回调）→ `ops::RelaunchAsAdmin`（ShellExecuteExW runas）
- 场景：点击「以管理员身份重启」后 `ShellExecuteExW(runas)` 在调用线程阻塞直至 UAC 端反馈（安全桌面交互期间主窗口消息循环停转，任务管理器显示「无响应」）。违反「UI 永不阻塞」精神。
- 修法：经 `ctx.jobs.Submit` 发起（JobQueue 线程非 STA 限制只约束 COM；ShellExecuteEx 在工作线程可用），或接受现状但在文档标注。

### P2-3 重 tick 叠加：SCM+EnumWindows+1/5 切片+GPU 可落同一 tick（§10 p95 预算的主要风险）
- 位置：`src/collect/ProcessCollector.cpp:255-261`、`src/collect/CollectService.cpp:162-194`
- 场景：tick 预算内各调用成本——NtQSI 进程表 ~1-3ms@500、每核时间/内存计数器 ~0.2ms、GetIfTable2 ~0.2-1ms、PDH 磁盘+硬故障 ~0.5-2ms（每 tick）。叠加项：① 1/5 切片 tick 约 100 进程 ×（OpenProcess+QueryFullProcessImageNameW+OpenProcessToken+GetTokenInformation+IsWow64Process2+GetPackageFullName）≈ 2-8ms（`ProcessCollector.cpp:283-286`，新进程突发时全量补算更高）；② `tickId%5==1` 的 suppCycle tick 再叠加 `EnumServicesStatusExW`（~1-5ms）+ `EnumWindows`（~0.5-2ms），且该 tick 同时是 slice==1 的切片 tick（三重叠加）；③ GPU 2s 节奏内联在 `DoTick`，首个 GPU tick 与 suppCycle tick 重合，极端时六类工作同 tick。p95 窗口 120 样本中重 tick 占 1/5，p95 恰由重 tick 决定——15ms 预算在快机可过，慢盘/多服务机器有越线风险。
- 削峰建议：SCM 挪到 `tickId%5==2`、EnumWindows 挪到 `%5==3`（缓存本就全周期复用）；GPU 查询在 suppCycle tick 跳过顺延；或将 GPU 挪到独立线程/JobQueue（其 `gpuRing` 统计线程局部性需同步调整）。

### P2-4 最小化/隐藏到托盘无 2s 节流（§10「最小化 2s 间隔」未落地）
- 位置：`src/app/main.cpp:138-156`（PeekMessage 循环无等待）、`src/app/D3DRenderer.cpp:69`（Present(1)）
- 场景：帧节奏完全依赖 vsync；窗口隐藏（托盘模式）或最小化时部分 DXGI 路径 Present 不再节流 → 全速 ImGui+D3D 空转烧 CPU/GPU。§10 属阶段 4 门禁，现登记不阻塞阶段 2。
- 修法：循环内检测 `IsIconic`/`!IsWindowVisible` → `WaitMessage`+2s 定时器或 `Present(0)`+Sleep(2000)。

### P2-5 DetailsProvider 缓存无界增长；`Invalidate` 从未被调用
- 位置：`src/ops/DetailsProvider.cpp:292-298`、`src/ops/DetailsProvider.h:58`；全局 `g_attemptedKinds`（`DetailsProvider.cpp:159-176`）同样无清理。
- 场景：每选中一个 (pid,createTime) 即永久留一条 Entry（cmdLine ≤32KB；Modules 未启用时较轻）。长会话高频切换选中缓慢累积；进程退出不回收。单条小、增长慢，但与「缓存增长上限」约束不符。
- 修法：快照 tick 检测 key 消失时 `Invalidate`（UI 已有 `RefreshSelection`/`lost_` 时机），或容量阈值 + LRU/清空（同 Signature.cpp:162 的做法）。

### P2-6 U8() 缓存 8192 上限触发整体 clear 的悬垂风险
- 位置：`src/app/ui/UiText.h:16-23`
- 场景：windowTitle 每 5 tick 刷新、动态标题（浏览器多标签）与 toast 文案会持续制造新 key；到 8192 时 `cache.clear()` 使**同帧早前返回的指针全部失效**。当前所有调用点都是「取用即弃」未踩中，但这是依赖纪律而非结构保证。
- 修法：clear 前至少延迟一帧（双缓冲代），或超限时淘汰最旧而非 clear；至少在头文件注释中明示「同帧内不得同时持有两个 U8 结果」的契约。

### P2-7 ExePath 用 MAX_PATH 缓冲 + longPathAware manifest 未接线（§7）
- 位置：`src/core/FsUtil.cpp:7-17`；`CMakeLists.txt`（无 .manifest/.rc，§7 的 `asInvoker+PerMonitorV2+longPathAware` manifest 未以文件形式存在）
- 场景：安装于 >260 字符中文深路径时 `GetModuleFileNameW` 静默截断 → config/session/log 落到错误目录。DPI 用运行时 API（Win32Window.cpp:11）已覆盖；longPathAware 只能走 manifest，目前缺失。
- 修法：`GetModuleFileNameW(nullptr, nullptr, 0)` 双调取全长，或缓冲扩到 32K；补 .manifest（UAC asInvoker 默认生成不受影响）。

### P2-8 排序实现与 §8 偏差：towlower 码点序，非 CompareStringEx；每 tick 排序成本
- 位置：`src/app/ui/SortKey.h:80-88`、`src/app/ui/Pages.cpp:610-614`
- 场景：§8 写明「中文列 CompareStringEx」，实现为码点逐字 towlower——中文进程名不按拼音/区域惯例排序（自我声明 header 7-9 行"no locale collation"，与契约矛盾属文档-实现二选一需对齐）。性能：Name 列 stable_sort ~4500 次比较 × 逐字符 towlower ≈ 0.5-1ms @500 行，每 tick 一次（1Hz），可接受；若未来提频需预计算小写键。
- 修法：按架构师裁决二选一——改 `CompareStringEx`（LOCALE_CUSTOM_UI_DEFAULT）或修订文档冻结「码点序」。

### P2-9 树杀确认框「预计 N 个」不含根，与执行结果 off-by-one
- 位置：`src/app/ui/Pages.cpp:188-189`（存 `members.size()`，仅后代）vs `src/ops/ProcessOps.cpp:365`（`planned = kids+1`）
- 场景：根有 3 个子进程时弹窗显示「预计终止 3 个进程」，执行后 toast「终止 4 个」——诚实性口径不一致。
- 修法：Plan 侧 `+1`（根），或弹窗文案改为「预计终止 N 个进程（含目标）」。

### P2-10 JobQueue 排队数未显示（§5 队列契约）；树杀 5s/进程等待可长占串行队列
- 位置：`src/core/Jobs.cpp:33-36`（`PendingCount` 无 UI 调用方，grep 验证）；`src/ops/ProcessOps.cpp:34,287`
- 场景：§5「UI 显示排队数」未实现；`TerminateVerified` 每目标至多等 5s，大树杀慢退出目标可占队列数十秒，期间其它破坏性操作/详情请求排队且无任何 UI 提示。
- 修法：状态栏加排队数（PendingCount 已具备）；树杀单目标等待上限可降至 ~1.5s 并把超时计入 failed。

### P2-11 session.json 写入非 §7 原子协议（temp+MoveFileExW 缺失）——登记项
- 位置：`src/ops/SessionState.cpp:23` → `core/Cfg.cpp:118-132` 直接 `wb` 截断写。
- 场景：写入中途崩溃留下损坏 session.json；降级路径安全（`LoadSession` ts/解析校验失败→全新启动，SessionState.cpp:32），仅损失一次交接。内容本身=白名单字段（page/selPid/selCreateTime/sortKey/sortDir/win rect/intervalMs/ts），**无敏感值** ✓。
- 修法：写 `session.json.tmp` 后 `MoveFileExW(REPLACE_EXISTING)`（§7 原文）。该文件不在本维度评审范围，正式归属请以对应审查者结论为准。

---

## 已查无问题清单

1. **单杀/树杀双层防线齐备**：UI 菜单对 `PF_Protected` 禁用两项（Pages.cpp:802-805）；ops 硬门在开句柄前拒绝（ProcessOps.cpp:325-330）；树杀成员级逐个校验、受保护成员跳过+计数+如实上报（376-384），根=保护进程同样被跳过（root 在 `order` 内参与 FindRow→ProtectedReason）且 UI 入口本就被禁用。
2. **PID 复用防护**：所有 ops 先 `OpenProcess`+`GetProcessTimes` ±1s 重验（227-274）；树收集拒收「创建早于父 1s 以上」的孤儿防父 PID 复用误收（211）；GPU pid 按 (pid,createTime) 映射、未知丢弃（GpuCollector.cpp:263-265）；差分基线按 (pid,createTime) 键控（ProcessCollector.cpp:294-296）。
3. **SeDebug 对称性**：`seDebugHeld==nullptr` 路径 enable→release 成对（235-245）；树循环结束统一释放（403）；Trim 用后即释（415）；PurgeStandby RAII 且仅在启用成功后构造（430-432）；启用/释放均记日志（Privilege.cpp:44），满足 §7「透明可审计」。
4. **确认闭环**：四类破坏性操作全部经 `ctx.jobs` 工作线程、UI 仅收 Notification（Pages.cpp:106-171）；确认框打开瞬间按值锁定 key/name/path（199-227）；树杀 Plan 异步、失败有失败 toast；执行时 ops 不信任 UI 副本（重验 createTime）。
5. **无隐蔽/持久化/规避检测**：grep 全库无 Run 键/启动文件夹/服务/计划任务/自启动写入；无 socket/监听（iphlpapi 仅 GetIfTable2 流量统计，无 ws2_32 链接用途）；托盘+主窗口均为常规可见窗口；ETW 未启用——`caps=0`（CollectService.cpp:149）、`SetNetEtwEnabled` 仅存 bool（274-277）。
6. **日志合规**：全量 STM_LOG 审计无命令行、无窗口标题；仅 pid/进程名/HRESULT/NTSTATUS/状态（允许「路径与 PID」）；滚动 3×1MB（Log.cpp:14-15）符合预算。
7. **Unicode**：无窄字符文件 IO（全部 `_wfopen_s`，grep 验证）；路径全程 wstring+W API；剪贴板经 ImGui Win32 后端 UTF-8→UTF-16（CF_UNICODETEXT）正确；字体路径 WideToUtf8 后由 ImGui 内部宽字符打开；selftest `core_wide_utf8_roundtrip` 通过。
8. **SnapshotStore 锁纪律**：`Set`/`Get` 只在锁内交换 shared_ptr，无持锁快照拷贝；UI 每帧恰好一次 `Get()`（DrawShell:1271）。
9. **内存有界项**：Signature 缓存 512（Signature.cpp:162）、FileMetaCache 2048（VersionInfo.h:92）、U8 8192、Ring/PerfHistory 120 点、toasts ≤6、`supp_/prev_/denied_` 每 tick 按 seen 剪枝（ProcessCollector.cpp:364-373）；Snapshot 双缓冲仅保留最新。
10. **UI 帧小项**：描述列 GetFileVersionInfoW 限流 3 次/帧 + UNC 拒绝（Pages.cpp:465,777；VersionInfo.h:28）；版本信息 4MB 上限（VersionInfo.h:31）；cmdline 读取 32KB 上限（DetailsProvider.cpp:82）；每帧快照仅一次 shared_ptr 拷贝。
11. **运行观察**：`stm_selftest.exe` 24/24 通过（含 tick p50<15ms、保护名单拒绝、身份重验、树杀纯函数、中文 UTF-8）；`SuperTaskMgr.exe --smoke 120` 正常渲染 120 帧退出（exit=0），日志无 error/warn。

## 观察项（不计级）
- Escape 可关闭模态但 `Ui().confirm` 悬留至下次 Request 重置——已确认无副作用。
- KillTree 规划任务若队列未运行（Submit 返回 0）`planCount` 永远 -1，弹窗不打开且无提示——实际仅退出竞态可触发。
- 「进程详情已更新」toast 在每次选中后弹出，高频切换略噪（UX 层面）。

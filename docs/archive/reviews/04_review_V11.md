# 阶段 4 终审报告（V11）

- 审查人：终审 subagent V11（与其他终审者互不知情）
- 日期：2026-09-18
- 对象：阶段 4 变更（Cfg/FsUtil/Jobs/main/Pages/manifest/README）+ 全仓回归
- 环境：Win32 22631 x64，build/Release 既有产物（禁止 git，未重构建）
- 结论：**P0 = 0，P1 = 2，P2 = 8**。selftest 40/40、--smoke 150 独占运行 exit 0，无阶段 4 引入的致命回归。

---

## P1（应修）

### P1-1 wantExit 跨线程置位用的是非原子 bool（阶段 4 新引入的数据竞争）
- 证据：`src/app/AppContext.h:24` `bool wantExit = false;`（注释"set by menu/confirm dialogs"已过时）；阶段 4 把工具栏提权挪进 jobs 后，`src/app/ui/Pages.cpp:1216` 在 **JobQueue worker 线程**执行 `app->wantExit = true;`；`src/app/main.cpp:159,165` 主线程 `while (!ctx->wantExit)` 读。无任何同步 → 按 C++ 标准是 UB（x86/MSVC 下实际通常表现为"碰巧能工作"，但编译器有权缓存/重排该读）。
- 修法：`std::atomic<bool> wantExit`（顺带更新注释）；tray/Pages3 等 UI 线程写点自动兼容。

### P1-2 最小化→恢复会把用户改过的刷新间隔悄悄回退为启动时快照
- 证据：`src/app/main.cpp:158` `const uint32_t userInterval = ClampInterval(ctx->cfg.GetInt(L"intervalMs", 1000));` 在帧循环**前**取一次性快照；运行中用户拖动滑条时 `src/app/ui/Pages.cpp:1186` 只更新 `ctx.cfg` 与 collect，`userInterval` 不会更新。复现：启动(1s)→滑条调 3000→最小化(collect=2000)→还原 → `main.cpp:169` 恢复为 userInterval=**1000**，而滑条/cfg 仍显示 3000 → UI 与实际采集频率背离，直至再碰滑条。
- 时点核对（任务书问点）：`ApplySession`（Pages.cpp:1300-1312）**不**把 session.intervalMs 写回 cfg，cfg 也从未在启动时被 session 覆盖；正常退出路径两者一致（Pages.cpp:1320 从 cfg 取值写 session），但 config.json 损坏+session 有效时 `main.cpp:81`（用 session.intervalMs）与 `:158`（用 cfg 默认 1000）来源分裂 → 见 P2-8。
- 修法：还原分支改为现读：`ctx->collect.SetInterval(iconic ? 2000 : ClampInterval(ctx->cfg.GetInt(L"intervalMs", 1000)));`（cfg 由滑条实时维护，是唯一可信来源）。

## P0
- 未发现（无崩溃、无数据损坏、无主路径失效）。

## P2（建议修）

### P2-1 工具栏提权按钮无防抖
Pages.cpp:1209-1225：UAC 等待期间按钮仍可点，连点 → SaveSessionFromCtx 重复执行 + 队列多个 RelaunchAsAdmin + 多个 UAC 弹窗/多次重启尝试。修法：pending 期间禁用（如 UiState 记 elevatePending，job 完成回 note 清除）。

### P2-2 其余 4 处提权入口未同步阶段 4 修法（UI 线程同步 ShellExecuteExW + 失败静默）
`src/app/ui3/Pages3.cpp:108,1322,1401`、`src/app/ui/Tray.cpp:120`（经 main.cpp:134-137 回调）：仍在 UI 线程阻塞等 UAC，失败（含用户取消）无任何 toast。与工具栏新行为不一致，属阶段 4"提权走 jobs"未覆盖的同一债务。

### P2-3 状态栏"操作队列"不含在途任务
Pages.cpp:1251 用 `jobs.PendingCount()`，Jobs.h:23 注释自认只数排队。UAC 等待、停服务等长操作 in-flight 时显示"操作队列空闲"，误导。修法：暴露 busy 标志（atomic）并入显示，如"执行中/队列 N"。

### P2-4 Cfg::Save 原子写缺持久化屏障
Cfg.cpp:118-143：fwrite 后未 FlushFileBuffers(tmp)，MoveFileExW 未带 MOVEFILE_WRITE_THROUGH → 掉电时 rename 可先于数据落盘，留下空/截断文件（原子性✓、durability✗）。配置/会话丢失后果可接受（默认值兜底），故 P2。修法：打开 tmp 句柄 FlushFileBuffers + `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`。另注：Save 并发调用会竞用同一 .tmp——当前全部 Save 均在 UI 线程，无实际问题，建议注释固化该前提。

### P2-5 最小化状态下退出 → 会话记录 -32000 图标坐标，下次启动窗口跑到屏幕外
SaveSessionFromCtx（Pages.cpp:1321-1329）未判 IsIconic，GetWindowRect 对最小化窗口返回 -32000 系坐标与缩小尺寸（winW 可能仍 >100 通过 main.cpp:148 守卫）→ MoveWindow 到屏幕外，用户误以为没启动。非阶段 4 引入，但最小化节流使"最小化时退出"成为常态路径。修法：IsIconic 时改用 GetWindowPlacement 的 rcNormalPosition 或跳过 rect 保存。

### P2-6 提权重启交接窗口仅 1s
新实例 `SingleInstance::TryAcquire(1000)`（main.cpp:55）；旧实例 wantExit 后仍需 save session + collect.Stop(join) + jobs.Shutdown(≤2000) + renderer 销毁才释放互斥体。慢盘/AV 场景可超 1s → 新实例弹"已在运行"退出且旧实例同时退出，用户两头落空。修法：提权交接场景（可借 `--relaunched` 参数识别）加大等待/重试。交接数据本身无虞：工具栏路径 SaveSessionFromCtx(nullptr) 的零 rect 中间态会被 main.cpp:186 退出前正式保存覆盖，且该保存先于互斥体释放。

### P2-7 --smoke 撞上已运行实例时会弹阻塞 MessageBox（CI 挂起风险）
main.cpp:55-58 对 smoke 无豁免；无头 CI 中 MessageBox 永久挂起。本次审查实测复现：与并行审查者的实例并发时，--smoke 150 以 EXIT=1 结束且日志被污染（独占重跑 exit 0，见下）。修法：smoke>0 时 TryAcquire 失败直接 return 非零。另实测本机 --smoke 150 全程约 2.5~3.5 分钟（WARP 软渲染 + 全页枚举），CI 预算需预留。

### P2-8 intervalMs 双来源分裂（低概率）
config.json 损坏/缺失但 session.json 仍处 60s 窗口内时：`main.cpp:81` 用 session.intervalMs 启动采集，`main.cpp:158` 用 cfg 默认 1000 作为还原值。修法：ApplySession 中 `ctx.cfg.SetInt(L"intervalMs", ClampInterval(session.intervalMs))` 统一来源（与 P1-2 的现读方案互补）。

---

## 实测记录（build/Release 既有产物）

| 命令 | 结果 |
|---|---|
| `stm_selftest.exe` | 40 通过 0 失败，exit 0 ✓ |
| `stm_selftest.exe --json` | 末尾输出 `{"pass":40,"fail":0,...}` ✓ |
| `SuperTaskMgr.exe --smoke 30`（独占） | exit 0，日志确认 4 页枚举执行 ✓ |
| `SuperTaskMgr.exe --smoke 150`（独占） | exit 0（约 2.5 分钟）✓ |
| manifest 验证 | exe 内提取到 `asInvoker`/`PerMonitorV2`/`longPathAware`/Win10 GUID 四串 → CMake `add_executable(... src/app/app.manifest)` 合入生效 ✓ |
| exe 体积 | 1,733,632 B ≈ 1.7MB，单文件 ✓；空载 WS ~127MB 与 README"≈124MB"一致 ✓ |

注意：首次 --smoke 150 出现 EXIT=1 + 残留进程，经日志取证为**并行审查者实例并发**所致（单实例互斥 + P2-7 的阻塞 MessageBox 路径），独占复跑正常，不计产品缺陷。

## 已查无问题清单

1. **Cfg.cpp Save 原子写**：tmp 与目标同目录同卷；写失败/移动失败均清理 tmp；单线程（UI 线程）使用无并发写；core_cfg_roundtrip 自测通过。
2. **FsUtil ExePath 长缓冲**：n==0、截断（n==buf.size() 增倍）、32768 上限放弃均正确；`std::wstring(data, n)` 显式长度无越界。
3. **Jobs.cpp Run 异常屏障**：catch(exception)/catch(...) 全覆盖并记日志；busy_ 在锁内复位；单互斥锁序无死锁；Shutdown 丢队列+限时等在途+超时 detach 与 main/shared_ptr ctx 捕获方案闭环；detach 后 ~JobQueue→Shutdown(0) 因 joinable==false 安全返回。
4. **main.cpp 全文回归**：单实例（WAIT_ABANDONED 已处理）；启动顺序（单实例→session→ctx→RegisterPages→ApplySession→Bind→collect.Start→jobs.Start→DPI→窗口→D3D→ImGui→tray）无倒置；失败路径依赖成员析构（~JobQueue→Shutdown(0)）安全；smoke 跳过 tray/会话/配置写（不污染用户数据）✓；退出顺序 tray.Remove→balloon sink 置空→collect.Stop→jobs.Shutdown(2000)→ui→renderer→win→Log 正确；IsIconic 沿判断（wasIconic 初值在循环前采样、窗口已创建）逻辑正确。
5. **Pages.cpp 提权 job**：liveCtx shared_ptr 按值捕获（生命周期正确）；失败经 notes→toast 有反馈；SaveSessionFromCtx 在 UI 线程先行调用。
6. **状态栏/采集自省**：frameMs、TickP95Ms、degraded 提示与快照一致。
7. **manifest vs 代码**：EnableDpiAwareness(PMV2 API) 与清单 PMV2 声明一致无冲突；supportedOS 为 Win10/11 GUID。
8. **README 抽验**：日志路径 `%LOCALAPPDATA%\SuperTaskMgr\logs` ✓；日志内容为计数/枚举摘要，无命令行/窗口标题 ✓；运行中日志可共享读 ✓；"40 项"“--json"“--smoke N 退出码 0"“覆盖全部页签"（DrawSmokeAllPages 遍历 ctx.pages）"列宽持久化"（colW_* 入 cfg）均验证属实。
9. **线程安全横查**：NotificationQueue 互斥 ✓；collect/jobs 队列 notify/wait 谓词正确；暂停/恢复采集与 interval 交互（恢复用滑条现值 Start）正确。

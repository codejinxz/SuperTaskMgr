# 终审报告 V16 —— 回归与资源生命周期（F4 落地批后）

- 评审人：V16（独立终审 subagent，2026-09-18）
- 范围：确认框回归、本轮新代码资源生命周期、线程/队列、cfg 往返、UI 状态机、实跑验证
- 方法：通读本轮全部改动源码（Pages.cpp / Pages3.cpp / GcPages.* / PerfCsv.h / ProcTree.h /
  ProcControlUi.h / WindowUtil.h / CrashUi.h / JumpState.h / PageHelpers.h / ProcKind.h /
  ConfirmAction.h / AutotestDialog.cpp / ProcessControl.* / CrashLog.* / Sensors.cpp /
  LhmSource / Jobs.cpp / Cfg.cpp / main.cpp）+ 实跑全部产物
- 产物基线：build/Release（15:35 构建，晚于最新源码 15:30，为当前源码的产物）

## 统计

- P0：0
- P1：1
- P2：6
- 自测：stm_selftest 70/70 双轮通过；--smoke 150 exit 0；--autotest dialogclick/kill/tree/startup
  全部 PASS；真实实例 120 s 内存采样 WS 132→138 MB、Private ~122 MB，增速递减、无异常增长。

---

## P1

### P1-1 Sensors.cpp `ReadProp` 泄漏 WMI 返回的 SAFEARRAY（VT_ARRAY|VT_UI1，G-B 新路径）

- 证据：src/collect/Sensors.cpp:222-237。`IWbemClassObject::Get` 填充的 VARIANT 拥有
  `parray`，调用方负责释放（VariantClear/SafeArrayDestroy）。现有代码 `SafeArrayAccessData`
  拷贝后仅 `unacc(...)` 解除访问，VARIANT 出栈时既未 `VariantClear` 也未
  `SafeArrayDestroy`；全仓库 grep 无任何 `VariantClear`/`SafeArrayDestroy` 调用。BSTR 分支
  （:254-257）是手工释放的，唯独字节数组分支漏配对。
- 触发面：仅 G-B 新增的 `ReadExtraSensors` 查询
  `MSStorageDriver_FailurePredictData.VendorSpecific`（Sensors.cpp:1366-1369）。传感器页每次
  刷新（≥10 s 间隔）× 每块 SMART 磁盘行泄漏一个 SAFEARRAY（典型 512 B + 头部）。
  管理员且该类可读的机器上长期运行无上界；本机非管理员返回拒绝访问、rows 为空，故
  selftest 与本机实跑观察不到。非崩溃级，判 P1（资源生命周期审计的主发现）。
- 修法：`ReadProp` 末尾统一 `VariantClear(&var)`（可同时替代 BSTR 手工释放），或在字节数组
  分支拷贝后 `SafeArrayDestroy(var.parray)` 并将 `var.vt` 置 VT_EMPTY；配套在
  sensors_multi_test 补一条"字节数组属性读取后销毁"的用例（可用计数断言或至少代码路径覆盖）。

---

## P2

### P2-1 cfg 键 `perfCsvWanted` 只写不读，异常退出后永久残留 true
- 证据：仅 Pages.cpp:1738/1764 两处 SetBool，全仓库无任何 GetBool(L"perfCsvWanted")。
  记录 CSV 期间应用退出/崩溃后键值停留 true；下次启动既不自动恢复记录也不清除，成为
  永久脏状态（诚实状态机缺口）。
- 修法：二选一——启动时读取并自动恢复记录（Start 失败回写 false），或去掉该键只以
  状态栏/按钮实况为准。

### P2-2 全局热键 id 0x47434D 超出文档约定的应用范围（0x0000–0xBFFF）
- 证据：GcPages.h:27 `kGcHotkeyId = 0x47434D`；main.cpp:334/387、GcPages.cpp:653/664/669。
  实践中 RegisterHotKey 对超范围 id 通常接受，且失败路径诚实（toast + cfg 回滚，main.cpp:387-393），
  但按 MSDN 0x0000–0xBFFF 的约定属越界，存在个别 Windows 版本严格校验的风险。
- 修法：改为 0x0000–0xBFFF 内的常量（如 0xB34D），一行改动。

### P2-3 亲和性模态在查询失败时永远停在"正在查询当前亲和性…"
- 证据：Pages.cpp:1350-1365。`GetCtrlInfoCopy` 命中后要求 `systemAffinityMask != 0` 才
  init；查询失败返回零值 info 时永远不满足，模态文案是进行时态（"正在查询…"），实际
  查询已结束且不可用，仅有"取消"可走。不撒假数据，但状态文案失真。
- 修法：查询已落地（slot ready）且 systemAffinityMask==0 时改文案为"无读取权限，无法
  设置亲和性"并保留取消。

### P2-4 ops 串行队列头部阻塞：崩溃查询 200 条 / EnumServices 与终止/挂起同队
- 证据：CrashPage（AsyncFetch 10 s）、HostSvcTooltip（EnumServices，本机 275 项）与
  ExecuteConfirmedAction 的 kill/suspend 共用同一 JobQueue（core/Jobs.cpp 单 worker）。
  事件日志庞大的机器上一次 crash 查询会推迟紧随其后的终止动作数秒；状态栏"操作队列 N"
  有可见性（Pages.cpp:2036-2041），危害有限。
- 修法（后续）：为只读枚举类 fetch 建独立低优先级队列，或对 QueryCrashEvents 分批让行。

### P2-5 页面级模态在切页后"开着但不渲染"
- 证据：##affinity（Pages.cpp:1332）、##confirm_close_window（GcPages.cpp:413）等在页面
  Draw 内 Begin，切走 Tab 后不再 Begin，弹窗停留 open 状态但不遮挡（返回原页复现）。
  壳级 ##confirm/##hostsvc 每帧渲染不受影响。无崩溃、无僵尸输入遮挡，体验性瑕疵。
- 修法：切页时（DrawShell 切 Tab 处）对当前页 pend/aff 槽位做 CloseConfirm 式清理，或
  接受现状并知晓无阻塞。

### P2-6 HostSvcCache / CtrlSlot 等按 pid/ProcKey 的缓存无淘汰
- 证据：GcPages.cpp:482-485（std::map<uint32_t, shared_ptr<HostSvcSlot>>）、
  Pages.cpp:1607（ctrl_ map）。长期运行随悬停/选中累积；单项极小且 pid 种类有限（服务宿主
  数十），实际无界风险低。
- 修法：可按快照 tick 淘汰未命中的 pid（低优先级）。

---

## 已查无问题清单（证据与核对结论）

1. **确认框主回归（用户报告"点击无反应"）——未复发**：
   - DrawConfirmDialogs（Pages.cpp:257-432）保持正确模式：请求期间**每帧** BeginPopupModal；
     OpenPopup 仅一次（confirmOpenRequested 一次性消费，:277-283）；`SetKeyboardFocusHere(0)`
     仅 `IsWindowAppearing()` 一帧（:406）；动作按钮 NoNav + 取消首位（:412-419）；所有退出
     路径（取消/动作/Esc→:293）都 CloseConfirm 清状态，无僵尸请求/隐形模态。
   - 新增 Suspend/SetPriority 复用同一 ##confirm（:364-391）；亲和性/宿主服务/窗口关闭确认/
     启动项/服务五处新模态均为同一正确模式的复用（OpenPopup 一次 + 每帧 Begin + Appearing
     一次聚焦 + NoNav 动作钮 + 双按钮 CloseCurrentPopup），没有引入新的手写单帧模态。
   - 实跑：`--autotest dialogclick` PASS（真实渲染对话框 + 合成鼠标点击管线，
     AutotestDialog.cpp 单帧化哨兵 + 几何瞄准均验证）；kill/tree/startup PASS；
     fix_confirm_* 五条 selftest PASS。
2. **PerfCsv**：Start 失败路径关闭句柄并清 path（PerfCsv.h:113-127）；每行 flush（:137）；
   Stop flush+close（:140-145）；Start 先 Stop 保证开关切换无句柄叠加；进程退出时 ofstream
   析构兜底；BOM+表头失败显式报错；csv_header_roundtrip selftest 锁定列数。
3. **ProcessControl**：全部句柄 UniqueHandle RAII（OpenVerifiedControl/PerThreadSuspendResume/
   GetProcessControlInfo）；保护名单先于 OpenProcess；身份 (pid,createTime)±1s 重验；
   NtQSI 缓冲 1 MB 起步、按 needed+64KB 上调、至多 8 次、栈式 vector 自动释放
   （:241-251）；布局偏移有 static_assert 锁定。
4. **CrashLog**：UniqueEvt RAII 全覆盖；EvtNext 批量句柄逐个移交 UniqueEvt 并置空
   （:244-246），无双重关闭；EvtRender 双调用模式正确（空缓冲探测 → ERROR_INSUFFICIENT_
   BUFFER → 按 needed 分配重试，二次不足即跳过该事件，:250-266）。
5. **WindowUtil**：每窗口 OpenProcess→CloseHandle 全路径配对（:84-92）；枚举无句柄滞留；
   陈旧 HWND 的前置/最小化/WM_CLOSE 均走 API 失败→诚实错误文案，不崩溃。
6. **热键对称**：GcHotkeySetEnabled 先 Unregister 再 Register（幂等）；退出路径
   GcHotkeyUnbindWindow（main.cpp:472）在窗口销毁前反注册；启动注册失败回写 cfg false；
   headless 不注册。注册/反注册严格对称。
7. **Sensors 其余 COM/资源配对**：CoInitializeEx/CoUninitialize 经 CoGuard 配对，含
   RPC_E_CHANGED_MODE 不配对分支（:264-278）；BSTR 经 Bs RAII；PDH 查询 PdhCloseQuery +
   FreeMibTable×2 经 Cleanup 结构（:943-952）；NVML init/shutdown/FreeLibrary 经 NvmlGuard
   （:535-542）；磁盘句柄 INVALID_HANDLE_VALUE 归一后才进 UniqueHandle（:798-806）。
   （唯 SAFEARRAY 见 P1-1。）
8. **线程与队列**：本轮全部新 job 均按值捕获 shared_ptr——树规划 [app,elevated,key,plan]、
   控制信息 [app,capture,key]、模块签名 [app,slot,path]、宿主服务 [app,slot,pid]、崩溃查询
   （AsyncFetch [app,st,fn]）、服务起停 [app,s,start,elevated]、依赖规划 [app,plan,name]、
   LHM 探测 [app,probe]、ETW 切换 [app,toggle,desired]、提权重启 [app]；共享槽位经 mutex/
   atomic 交接副本，UI 线程不持指针读共享单元。JobQueue worker 捕获异常不致死（Jobs.cpp:90-96），
   Shutdown 丢弃未启动任务 + 2 s 在途等待 + detach 生命周期注释与实现一致。
9. **cfg 往返与默认值**：sysDistMode（读时钳 0-2，Pages.cpp:605-607）、procTreeMode、
   sensDetailMode（默认 false，Pages3.cpp:1427/1852）、sensShow*（SensorGroupCfgKey 全部
   键名唯一、默认值集中 PageHelpers.h:100-121）、lhmEnabled/lhmPort（钳 1024-65535）、
   hotkeyEnabled、netEtw（读回=真相回写）、alertOn/alertCpu/alertMem 均读写成对；键名与
   存量键无冲突；Config::Save 原子写保持。（perfCsvWanted 见 P2-1。）
10. **UI 状态机**：树形/平铺切换重置 sortReflected_ 且回平铺时重同步表头排序（Pages.cpp:810,
    841）；树形模式禁用表头 Sortable 并给出 tooltip（:832-835, 862-865）；行身份用
    (pid,createTime) 双 PushID，右键菜单/选中不因刷新换行错位；过滤→区分过滤→排序→先过滤
    后建树的叠加顺序正确（RebuildRows :706-749），孤儿/环提升为根不丢行；窗口页行操作对
    已销毁窗口诚实失败；崩溃页空态/部分失败/重试路径齐全。
11. **实跑记录**：selftest 70/70 ×2（exit 0）；`--smoke 150` exit 0；`--autotest dialogclick`
    = "PASS - 真实管线点击生效：子进程退出、模态已关闭"；`--autotest kill`/`tree`（含 ping
    子进程）/`startup` 均 PASS（autotest_result.log 尾行核对）；真实实例 12 s 间隔采样：
    WS 132,240→135,728→137,228→138,436 KB（120 s），Private 118,884→122,164 KB，句柄
    692→728；增速持续递减（首分钟字体图集/元数据缓存的正常暖机），基线与上轮 ~125-135 MB
    持平（终值 WS 138 MB 含共享页，Private 122 MB），无新功能引入的异常增长。

## 结论

上轮"确认框点击无反应"未复发：主确认链路模式完好，本轮五处新模态全部复用同一正确模式，
dialogclick 实测通过。本轮唯一的资源级问题是 P1-1（WMI SAFEARRAY 泄漏，管理员+特定 WMI 类
可读时按传感器刷新周期缓慢累积），一行 VariantClear 可修；P2 各项均为状态文案/约定/可见性
级别的改进项。不阻塞发布。

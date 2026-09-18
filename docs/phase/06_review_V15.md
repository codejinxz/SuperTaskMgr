# 终审报告 V15（06 阶段 · 本功能轮新增）

- 评审人：终审 subagent V15（与其他评审者互不知情；未用 git；除本报告外未修改任何文件）
- 日期：2026-09-18
- 对象：进程控制（ops/ProcessControl.cpp + Pages.cpp 集成）、崩溃记录页（ops/CrashLog.cpp + ui3/GcPages.cpp）、窗口管理页（ui3/WindowUtil.h + GcPages.cpp）、ProcKind.h、ProcTree.h、JumpState.h、PerfCsv.h、全局热键（GcPages.cpp/main.cpp/Pages.cpp）、Sensors.cpp 本轮多值改动。

## 实跑结果

| 项 | 结果 |
|---|---|
| build/Release/stm_selftest.exe | **70 通过，0 失败**（含 ui_prockind_classify / ui_proctree_order / ui_procctrl_labels / ui_crash_labels / ui_windowutil_labels / csv_header_roundtrip / ui_jump_filter） |
| SuperTaskMgr.exe --autotest kill | **PASS**（exit 0，"已终止进程 cmd.exe"） |
| SuperTaskMgr.exe --autotest tree | **PASS**（exit 0，"终止 3 个"） |
| SuperTaskMgr.exe --autotest startup | **PASS**（exit 0，StartupApproved 禁用编码生效） |
| SuperTaskMgr.exe --autotest dialogclick | **PASS**（exit 0，真实管线点击生效） |
| SuperTaskMgr.exe --smoke 150 | **exit 0**，页面枚举/服务/驱动/启动项均在日志中正常完成 |

环境注：本机存在并行评审实例竞争单实例互斥；headless 启动在互斥被占时按 V11-P2-7 设计静默 exit=1（无日志行）。复跑（等待互斥空闲）后四条 autotest 全部 PASS。

## 统计

- **P0：0**
- **P1：2**（CrashLog EVT 句柄泄漏；PerfCsv 磁盘满后静默丢数据）
- **P2：9**

---

## P0

无。重点核实项（曾按 P0 候选排查，均排除）：

1. **保护名单硬门是否覆盖四个操作**：Suspend/Resume/SetProcPriority/SetProcAffinity 全部经 `GateAndOpen`（ProcessControl.cpp:307-316）→ `ops::ProtectedReason(const ProcKey&,...)`（ProcessOps.cpp:482-501）。名字为空时按需解析镜像路径/Toolhelp 名，并带同名伪装豁免（路径在 Windows 目录外则不保护）+ 未知路径保守保护。UI 层另有 `BeginDisabled(protectedProc)` 双保险（Pages.cpp:1042-1088，含"恢复进程"）。`GetProcessControlInfo` 只读豁免为头文件契约明示。
2. **XML 自包含解析**：`UnescapeXml` 顺序正确（&amp; 最后，"&amp;lt;"→"&lt;"）；`TagBoundary` 防 `<Datax`/`<Provider` 前缀误配；自闭合、无闭合标签、缺字段、超长（EvtRender 大小探测+二次重试）与 swscanf 失败（时间→0→"—"）均诚实降级，无越界（gt-1 因首字符为 '<' 恒 ≥1）。
3. **ProcTree 环与复用**：父 createTime 晚于子→提升根（防 PID 复用）；环节点因 createTime 链无法成立时由第二轮提升，行不丢不重；DFS 迭代式防深递归。
4. **亲和性/优先级**：mask==0 拒绝、system 子集校验（ProcessControl.cpp:403-413）、UI 复选框限定系统掩码、确定钮 chosen==0 禁用；Realtime 双警示（确认框红字 Pages.cpp:381-385 + ops WARN 日志）且失败如实传播。

---

## P1

### P1-1 CrashLog：maxCount 截断批内剩余 EVT 句柄泄漏
- 文件:行：`src/ops/CrashLog.cpp:243-244`（`for (DWORD i = 0; i < returned && out->size() < maxCount; ++i)`）
- 证据：内层循环在 `out->size()` 达到 maxCount 时提前退出，`EvtNext` 本批已产出但未取用的 `batch[i+1..returned-1]` EVT_HANDLE 无人 close（每个句柄仅在被取出时包进 `UniqueEvt`），外层 while 随即退出。CrashPage maxCount=200、kBatch=16，两通道合计事件 ≥200 时每次刷新（页面激活 + 每 10s 自动）泄漏最多 15 个句柄，长会话无界增长。
- 修法：maxCount 截断发生时对剩余句柄补 `EvtClose`（或在取用处先把整批包进 RAII 数组再裁剪），并在 ui_gc/control 自测中加"查询两通道合计 > maxCount"用例。

### P1-2 PerfCsv：磁盘满/写错误后静默丢行，UI 仍显示"记录中"
- 文件:行：`src/app/ui3/PerfCsv.h:134-138`（Append）、`140-145`（Stop）；消费点 `src/app/ui/Pages.cpp:1754-1766、2059-2067`
- 证据：`f_ << ...; f_.flush()` 后不检查流状态。磁盘满后 ofstream 锁定 failbit，后续写入全部空操作，但 `Active()`（is_open）仍为 true——状态栏持续显示"● 记录 CSV"、性能页持续显示"● 记录中"，无任何 toast/自动停止；`Stop()` 的 flush 同样静默。用户以为在记录，实际数据早已中断，直接违背"诚实数据"原则；且"长时间记录填满磁盘"恰是该功能的目标场景。
- 修法：Append/flush 后 `if (!f_) { f_.close(); failed_=true; }`；`Active()` 返回 false 或 UI 层检测到 failed_ 时 toast"写入失败（磁盘满/被占用），已停止记录"并复位 perfCsvWanted。

---

## P2

1. **Sensors.cpp:397-405（+432-444）**：WMI 基础设施性失败（winmgmt 停止/RPC 故障，`w.ok==false` 且非 denied/notSupported）落入"本机无 ACPI 热区传感器"（NoHardware）——四态不诚实残余；ReadDisks 已检查 `w.ok`（:778）而 ReadCpuTemp 未检查。另 ：432-444 else-if 链在 invalid>0 时吞掉"枚举被拒绝（结果可能不完整）"提示。**G-B 自称修复的"WMI denied 误判 NoHardware"复核结论：正确**——WmiQuery:314-321 捕获枚举期 WBEM_E_ACCESS_DENIED→denied，ReadCpuTemp:387-395 正确映射 NeedAdmin（residual 仅上列边缘）。修法：按 `w.ok` 区分"NoHardware"与"查询失败（WMI 不可用）"文案；提示改为可叠加。
2. **Pages.cpp:1738/1764**：`perfCsvWanted` 只写不读（全仓库无消费点）——无"重启自动恢复记录"，属误导性死配置。修法：删除，或启动时读取并自动 Start（并在失败时清位）。
3. **PerfCsv.h:54-55 vs 118-120**：表头核心数在 Start 固化（cores_），行却按当前快照 `perCorePercent.size()` 生成；记录中核数变化（热增核/SMT/VM 调整）导致行列数与表头漂移。修法：Append 用 cores_ 补齐/截断到表头列数。
4. **GcPages.cpp:396-458 + WindowUtil.h:110-117**：关闭确认框打开期间目标窗口销毁→PostMessage 失败（诚实，OK）；但若句柄值被系统复用，WM_CLOSE 会命中无关窗口。修法：PendClose 记录 pid，发送前 `IsWindow(hwnd) && GetWindowThreadProcessId(hwnd,&pid2)` 复核，不符则提示"窗口已关闭"。
5. **GcPages.cpp:264-292**：选中窗口已销毁时（刷新后 `Selected()` 返回 nullptr），前置/置顶/最小化/关闭按钮点击被 `&& sel != nullptr` 静默吞掉，无提示不刷新。修法：`selectedHwnd_ != nullptr && sel == nullptr` 时 toast"窗口已关闭，请刷新"并自动重枚举。
6. **GcPages.cpp:482-521**：HostSvcCache 按 pid 静态缓存无淘汰、永不失效——长会话内存缓涨，且进程后来新承载的服务永不刷新（"未承载可枚举的服务"永久陈旧）。jobs 生命周期本身安全（捕获 shared_ptr AppContext；Submit==0 时置 ready+err 不静默）。修法：加 LRU 上限或 TTL；模态打开时支持手动重查。
7. **Pages.cpp:1253-1257、1293-1307**：`GetProcessControlInfo` 失败（如拒绝访问）时 slot 已存 err，但 `GetCtrlInfoCopy` 照常返回默认 info——详情面板显示"未知（类 0x0）"而非权限原因；亲和性模态无权限时永远停在"正在查询当前亲和性…"（文案"无读取权限时无法设置"仅部分覆盖）。修法：查询失败时把 slot->err 渲染出来（"查询失败：…"），模态据此停止轮询。
8. **CrashLog.cpp:293-300**：两通道同时失败只报 Application 的错误（`!appErr.empty() ? appErr : sysErr`），System 失败原因被丢弃。修法：两个 err 拼接（"Application: …；System: …"）。
9. **ProcessControl.cpp:128-154、69**：逐线程回退在"身份验证通过→Toolhelp 快照"之间存在 pid 复用 TOCTOU（可能挂起复用者的线程；仅 ntdll 缺导出时才走此路，现实概率≈0）；`OpenVerifiedControl` 无条件 `SeDebugPrivilege=false`，若调用前已被他人启用会被误关（各操作自取自还，自愈）。修法：回退路径复核快照内该 pid 的进程镜像名；记住进入时特权状态、按原状恢复。

---

## 已查无问题清单

- **ProcessControl**：四操作前置硬门 + `PROCESS_SUSPEND_RESUME`/`PROCESS_SET_INFORMATION` 最小权限；NtSuspend/NtResume 动态绑定（ntdll 常驻、static 局部初始化线程安全）+ 文档化逐线程回退 + NTSTATUS→中文（含 ACCESS_DENIED 专文案）；QuerySuspendedFlag 结构体 static_assert 锁版、walk 边界/截断/布局异常一律 honest unavailable；身份重验 ±1s 全操作（含只读 GetProcessControlInfo）共用；挂起态徽标决定菜单文案（Pages.cpp:1051-1064）。
- **CrashLog/GcPages 崩溃页**：全链只读（EvtQuery/EvtNext/EvtRender/EvtClose，全部 RAII）；通道打开失败降级为中文原因（"系统限制或被策略禁用"）；单通道失败+有数据→顶部黄色"部分事件读取失败"，双失败→错误+重试钮；空态诚实文案；EvtNext 2s 超时有界；渲染失败跳过并记日志。
- **窗口管理页**：仅可见顶层 + DWMWA_CLOAKED 动态绑定过滤 UWP 幽灵窗（工具窗口有意保留——它们确为可见顶层窗口，符合"诚实呈现"）；无权限进程名空→"—"不伪造；WM_CLOSE 温和关闭两段式确认（取消首位+NoNav+焦点管理）；置顶/前置失败如实报错（含前台锁文案）；最小化返回值语义注释正确；枚举在 jobs 线程、GetWindowText 对跨进程窗口不投递消息（无卡 UI 风险）；AsyncFetch 2s 最小间隔、busy 单飞、teardown 安全。
- **ProcKind**：优先级 Critical>ServiceHost>Uwp>Windows>User>Unknown；PathUnderSystemRoot 边界正确（"C:\Windowsa" 不匹配、'/' 归一、恰为根目录处理）；空 path 仅走保守知名名表（宁漏勿错）；"只看用户进程"保留 UWP、隐藏 Critical/Windows/ServiceHost 符合预期，Unknown 诚实可见；GetWindowsDirectory 失败退化为按名/用户——朝"多显示"保守方向。
- **ProcTree**：孤儿/自引用/父 createTime 晚于子→提升根；环二次提升不丢行；先过滤后建树为文档化行为（被过滤父的子提升为根）；树形模式禁表头排序且提示"排序仅作用于同级"；行以 (pid, createTime) PushID，选中/右键/详情与行绑定正确，与平铺一致；DFS 行序经 BuildTreeOrder 输出绝对下标映射。
- **JumpState**：pid==0 忽略；DrawShell 消费即切页（进程页恒为 pages[0]，RegisterPages:2084 首位）、进程页下一帧解析；目标已退出 toast"进程 N 已退出，无法跳转"；单 UI 线程槽位无锁正确。
- **PerfCsv（其余路径）**：文件打开失败/目录创建失败/表头写入失败均有中文 err+toast；NaN→空单元格（绝不 0 冒充）；时间戳取快照自证；文件名固定 `perf_+时间戳` 模式，无任何用户输入进入文件名（无路径注入）；UTF-8 BOM；每行 flush。开关竞态（记录中退出）：ofstream 析构兜底关闭，静态存储期安全；仅 P1-2 所述写错误路径不诚实。
- **热键**：主线程注册/反注册（窗口线程合法）；退出路径 `GcHotkeyUnbindWindow()` 先于窗口销毁（main.cpp:472）；启动注册失败→cfg 回滚 false+日志（main.cpp:387-393），复选框失败不落 cfg（诚实状态机，Pages.cpp:1991-2002）；headless 不注册；WM_HOTKEY wParam==kGcHotkeyId 判定正确，与托盘左键共用 `ToggleMainWindowVisible` 三分支（最小化还原置前/可见隐藏/否则显示置前），无分叉漂移；`UnregisterHotKey` 幂等先清旧注册。
- **Sensors 多值（其余）**：ACPI 逐实例（"ACPI 热区 N"+InstanceName），无效/离谱温度不造假并计数提示；NVML 逐卡逐传感器展开（温度/慢速阈值/功耗/GPU 利用率/显存利用率/风扇），0% 风扇视为真实读数、设备数钳 8、Load 失败 FreeLibrary+Guard 配对；SAFEARRAY 四个绑定函数齐备、LBound/UBound/AccessData 校验、4096 字节钳制、Access/Unaccess 配对；extra 组空则整组不显示（absence promises nothing）；legacy gpu 向量契约保持。
- **实跑环境**：四条 autotest 与 smoke 全部通过（见上表）；单实例互斥在并行会话下的 headless 静默 exit=1 为 V11-P2-7 既有设计，非本轮缺陷。

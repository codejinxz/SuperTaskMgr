# F2 审查报告：交互流"静默无效"类缺陷与遗漏 bug（只读分析）

范围：工具条 / 进程页 / 性能页 / 网络页 / 启动项页 / 服务页 / 驱动页 / 传感器页 / 托盘 / 告警 / 确认框 / 排序与过滤 / session 恢复。
边界：终止进程 / 进程树 / 启动项禁用三条链路归 F1，本报告不重复其修复本身，仅报告共享管线问题。
基线：build/Release（imgui 1.92.9）。可运行验证见文末。

---

## P0（用户可复现的功能失效）

### P0-1 shell 确认框仅提交一帧即消失：释放工作集 / 清理待机缓存永远无法确认执行
- 位置：`src/app/ui/Pages.cpp:301-321`（`DrawConfirmDialogs`）
- 机制：`confirmOpenRequested` 在 OpenPopup 同帧被清零（L317），下一帧起 L314 直接 return，`BeginPopupModal("##confirm")` 再也不会被调用。ImGui 弹窗只有被 Begin 提交才会渲染：模态窗口从第 2 帧起不可见，但 `##confirm` 仍留在 OpenPopupStack；`GetTopMostPopupModal()`（third_party/imgui/imgui.cpp:12376）不检查 Active/WasActive，`UpdateHoveredWindowAndCaptureFlags`（imgui.cpp:5500-5503）据此把模态矩形之外的全部悬停清空——消失后的模态残桩继续吞掉主窗口 hover/点击，直到用户任意一次右键（imgui.cpp:5465-5474 ClosePopupsOverWindow）清栈。净效果 = 操作"点了没反应"。
- 复现：右键任意进程 → 释放工作集 → 确认框闪现约一帧（60fps 下不可感知）后消失，无法点确认，trim 不执行；工具条"清理待机缓存"同理。StartupPage/ServicePage 的确认框（Pages3.cpp:589-634 / 939-1008）每帧重提交 `BeginPopupModal`，模式正确、不受影响——反证 shell 版本是一次性 OpenPopup 写法错误。
- 注意：同一函数同时服务 Kill/KillTree（F1 范围）。根因唯一，**建议并入 F1 对 `DrawConfirmDialogs` 的修复，把 L314 的提前 return 移到 `BeginPopupModal` 之后（`req.kind != None` 期间每帧重提交），四种 kind 一并修好，勿两处重复修**。
- 相关管线（可报）：通知→toast 回填链路本身正常（`DrainNotifications` 每帧一次，Pages.cpp:1172-1176），F1 链路一旦能提交，结果反馈无误。

### P0-2 列宽持久化把"描述"列像素宽写进 stretch 权重槽，第二次启动起名称列塌缩
- 位置：`src/app/ui/Pages.cpp:578`（`save(12, L"colW_desc", /*stretch=*/false)` 保存 `WidthGiven` 像素）vs `:550`（加载进 `widths_[12]`）vs `:726-727`（`TableSetupColumn("描述", WidthStretch, widths_[12])` 把它当权重用）。描述列是 `WidthStretch` 列，保存/加载语义错位。
- 实证：本机 `%LOCALAPPDATA%\SuperTaskMgr\config.json` 已有 `"colW_desc": 726.000`（像素值）、`"colW_name": 32.855`（被连带放大的补偿权重）。
- 复现：运行一个会话（PersistWidths ~1Hz 必然把 ≈数百 px 的 WidthGiven 写入 colW_desc）→ 重启 → 名称列权重 32.9 vs 描述列权重 726，名称列只剩约 4% 宽，描述列占约 96%，无需任何用户拖拽即 100% 复现，且每次重启自我延续。进程名是主识别列，表格实质不可用。
- 建议：col 12 与 col 0 一致按 StretchWeight 保存（`save(12, L"colW_desc", true)`，删去 L578 的非 stretch 保存）；加载侧对 >10 的旧值重置为 0.6 兜底。

## P1（特定条件失效 / 明显功能缺失）

### P1-1 session 恢复的页签索引被完全忽略，永远打开"进程"页
- 位置：`src/app/ui/Pages.cpp:1300-1312`（`ApplySession` 写 `ctx.activePage`）、`:1353-1362`（`DrawShell` TabBar 从不消费它——无 `ImGuiTabItemFlags_SetSelected`、无任何选中注入；`activePage` 只在 `BeginTabItem` 命中时被反向覆盖）。
- 复现：切到"传感器"页 → 退出 → 60s 内重启（session 未过期）→ 打开的仍是"进程"页。
- 建议：首帧渲染 TabBar 时给目标页的 `BeginTabItem` 传 `ImGuiTabItemFlags_SetSelected`（一次性标志，命中后清除）。排序键/选中项的恢复经 cfg 链路验证有效（见 OK 清单）。

### P1-2 托盘左键在窗口最小化时执行"隐藏"而非"还原"
- 位置：`src/app/main.cpp:123-130`：`IsWindowVisible(最小化窗口)` 返回 TRUE（最小化不清 WS_VISIBLE）→ 走 `SW_HIDE` 分支。
- 复现：最小化主窗口 → 点托盘图标 → 任务栏按钮直接消失（用户感知：点了没反应/窗口丢了）；再点才以最小化态闪回（`SW_SHOW` 不还原 iconic 窗口）。
- 建议：三分支——`IsIconic` → `SW_RESTORE`+SetForegroundWindow；可见 → `SW_HIDE`；其余 → `SW_SHOW`。

### P1-3 传感器页"风扇"卡片被 SameLine 溢出裁剪，完全不可见
- 位置：`src/app/ui3/Pages3.cpp:1273-1279`：`half = (avail.x - spacing) * 0.5f`，三个各 50% 宽的子卡片用 SameLine 链接，第三个起点在 `avail.x + spacing` 处，超出父窗口且页面无横向滚动条 → 永久不可达。
- 复现：打开"传感器"页 → 只见 CPU、GPU 两卡，风扇卡（及其中"需要驱动支持"等诚实态）不可见。
- 建议：三卡改 `avail.x/3` 宽，或第三个前 `ImGui::NewLine()` 换行。

### P1-4 驱动页/传感器页提权按钮：UAC 取消后零反馈，且 UI 线程被 UAC 阻塞
- 位置：`src/app/ui3/Pages3.cpp:104-110`（`DrawElevateButton`）、`:1318-1324`、`:1396-1404`：直接在 UI 线程调 `ops::RelaunchAsAdmin`（`ShellExecuteExW(runas)` 阻塞至 UAC 关闭，窗口停帧可被系统判"未响应"）；返回 false（用户取消/失败）时无任何提示。
- 对比：工具条同名按钮已走 ops job + 失败 toast"提权重启失败或已取消"（Pages.cpp:1207-1226）。
- 复现：非管理员 → 驱动页降级页（24H2+）或传感器页"提权/提权重启" → 取消 UAC → 界面无任何变化。
- 建议：与工具条统一：job 内执行 + 取消/失败 PushNote。

### P1-5 性能页 GPU 块缺失：采集在跑、规格有要求、UI 不渲染
- 位置：采集侧 `src/collect/CollectService.cpp:198-230`（2s 节奏产出 `snap->gpuProcs` / `sys.gpus`，含 p95>10ms 自动降 4s 的预算机制）；UI 侧 `src/app/ui/Pages.cpp:1052-1166` PerfPage 只有 CPU/内存/磁盘/网络四象限；全 UI 无任何 `gpuProcs/sys.gpus/GpuTickP95Ms` 消费（grep 证实）。架构文档 §8"性能页 …(+GPU，阶段 3)"、§12 未落地。
- 复现：打开性能页 → 无 GPU 象限；GPU 采集开销白付。
- 建议：增补 GPU 适配器利用率图 + 每进程 GPU 占用（快照里数据齐全）。

## P2（体验 / 边缘）

- **P2-1 详情更新 toast 噪音**：每次点选进程行，DetailsProvider 完成后都推"进程详情已更新" toast（`src/ops/DetailsProvider.cpp:286-298` → Pages.cpp DrainNotifications）。选行是高频操作，建议 Info 级详情更新只进状态栏 lastNote，不进 toast。
- **P2-2 已退出进程的详情字段显示"查询中…"不诚实**：面板 `alive=false` 时不发请求（Pages.cpp:877），`Peek==nullptr` 时签名字段永久"查询中…"（:935-963）。session 恢复到一个已死 PID 即复现。应对非 alive 态显示"—/未查询"。
- **P2-3 暂停采集缺全局指示 + 恢复后历史丢 1 样本**：`Ui().paused` 只改按钮文案，状态栏/性能页无"已暂停"；Stop/Start 后 `Impl::Run` 的 tickId 从 1 重计（CollectService.cpp:99,250），与 `Hist().lastTick` 撞号的那一帧被 AppendHistory 跳过（Pages.cpp:440）。建议 tickId 挂在 Impl 成员上单调递增，状态栏加暂停标记。
- **P2-4 页内提权路径丢窗口矩形**：`SaveSessionFromCtx(ctx, nullptr)`（Pages.cpp:1212；Pages3.cpp:107,1321,1400）→ 提权重启后新实例用默认位置/尺寸；托盘路径传了 hwnd（Pages.cpp:137-140）不受影响。建议统一传 hwnd。
- **P2-5 工具条权限标签窄窗口遮挡按钮**：Pages.cpp:1230 `SameLine(GetWindowWidth()-90)`，窗宽 <约 620px 时"管理员/普通权限"与"清理待机缓存/以管理员身份重启"重叠（child NoScrollbar，遮挡点击）。
- **P2-6 网络页手动刷新在拉取进行中静默 no-op**：AsyncFetch busy 时 MaybeFetch 直接 false（AsyncFetch.h:43-74），按钮无反馈，tooltip 却承诺"立即重新拉取"（Pages3.cpp:253-258）。建议 busy 时禁用按钮或显示"拉取中…"。
- **P2-7 ETW 失败 toast 固定归因"需要管理员权限"**：Pages3.cpp:217-221，StartTrace 也可能因会话数上限等原因失败，文案以偏概全（状态机本身诚实回滚，正确）。
- **P2-8 启动项工具条"启用/禁用"不判断当前状态**：Pages3.cpp:469-472，对已启用项仍可点"启用"，重写并报"已启用启动项"（行内菜单只给反向操作，正确）。建议按 `sel->enabled` 禁用对应按钮。
- **P2-9 DriverPage::EnsureSig 在 liveCtx 为空时先登记 slot**：Pages3.cpp:1122-1131，队列不可用时该行永远"查询中…"（仅退出竞态可见）。建议 app 为空不登记。
- **P2-10 服务页死调用**：Pages3.cpp:689 `BecameActive(lastFrame_)` 返回值未用，切页不刷新与启动项页行为不一致；删除或改为 force。
- **P2-11 服务/启动/驱动表行 PushID 用行下标**（Pages3.cpp:526,826,1186）：数据刷新重排后同一 ID 指向不同实体；恰逢右键菜单打开时菜单内容可能换成另一行。低概率，建议用稳定 id/name。

## 确认无问题清单（逐面验证）

- **工具条**：暂停/继续（Stop/Start + 按钮文案切换 + toast，暂停时滑条只写 cfg、恢复按新值 Start，正确）；刷新间隔滑条（SliderInt→SetInterval 立即唤醒 worker 生效，最小化回读 cfg，正确）；工具条提权（job 内执行、失败有 toast）；状态栏（帧耗时/采集 p95/队列/兼容模式均实时）。
- **进程页**：搜索过滤（实时、大小写不敏感、名称+PID、计数联动、"清空"有效）；排序点击（SpecsDirty→重建+持久化；不可用值恒排尾、pid 升序破平、过滤先行后 stable_sort——过滤态排序稳定）；排序箭头还原（ReflectPersistedSortOnce）；徽标/表头 tooltip；详情面板按需请求（kDetailKinds 选择时一次拉取、pending 去重、CmdLine 单次尝试语义正确）；父进程跳转（ProcKey 定位、detailKey 重置、lost 复位）；选中跟踪（pid 二分+createTime 校验、退出后冻结"已退出"诚实态）；右键菜单：复制名称/路径（UTF-8 剪贴板）、打开文件位置（explorer /select）、校验签名（Request+面板联动）、保护进程禁用+原因（BeginDisabled）；双击仅等于两次选中，无重复触发。
- **性能页**：四象限 ImPlot、120 tick 环形、NaN 跳过、核心数热变更重建、内存/提交进度条。**告警控件**：阈值改动即生效（AlertTick 每帧读 cfg）、5% 迟滞回臂、5 分钟冷却、默认关、气泡经 tray sink 且退出前置空（main.cpp:194）。
- **网络页**：过滤（地址/端口/PID/进程名）；ETW 开关状态机（job 执行 + NetEtwEnabled 读回真值 + 失败回滚 checkbox/cfg + toast；pending 期间禁用防连点）；错误/重试路径；UDP 无状态显示"—"。
- **启动项页**：切页+手动刷新；按稳定 id 选中跨刷新保持；提权门（canToggle）+ 备份提示 + 操作后 refetch（串行队列保证读到期后状态）；**确认框模式正确**：每帧重提交、取消键获焦（Enter/Space 落在取消）、Esc 经 NavEnableKeyboard 关闭且 pend_ 复位、点击外部关闭复位、连点安全（首击即清 pend_）。
- **服务页**：过滤/刷新；启动/停止门（非提权禁用、SERVICE_DISABLED 不可启动、canStop 不接受停止——均带原因 tooltip）；停止前运行依赖异步警告；确认框模式同上正确；操作后 refetch。
- **驱动页**：24H2+ 非提权整页降级 + 重试；选中触发签名、右键重校验；部分错误横幅。
- **传感器页**：诚实三分法渲染、磁盘健康表、序列号 tooltip、刷新按钮（除 P1-3/P1-4 外）。
- **托盘**：TaskbarCreated 重注册；右键菜单 SetForegroundWindow + WM_NULL 修法正确；三项菜单动作全部接线；气泡 NIF_INFO；smoke 模式跳过托盘。
- **确认框通用**：两段式（取消先获焦、动作键禁键盘导航，V8-P1-2 在位）；树杀计划失败有 toast 且弹窗不打开。
- **session/配置**：排序键/方向、选中（selPid+createTime 一次性还原后清零防陈旧）、窗口矩形（最小化 -32000 防护）、间隔恢复 clamp，均真实还原（页签除外，见 P1-1）；列宽仅"描述"列错位（见 P0-2），其余列保存/加载语义一致（名称列双写最终以 stretch 权重覆盖，结果正确）。
- **ImGui 陷阱排查**：未发现每帧重建吞输入（表格用 clipper+行索引，输入框状态在成员变量）；BeginDisabled 区域内的按钮均不可点（无假可用）；ID 冲突（同名标签靠 PushID(pid/r) 区分、弹窗 id 互异、toast 指针唯一）；Popup 栈平衡（除 P0-1 外，Startup/Service 的 Open/Begin/End 成对）；键盘 Nav（NavEnableKeyboard）与确认框的安全交互已验证；SameLine 溢出仅 P1-3、P2-5 两处。

## 可运行验证

- `stm_selftest.exe`：**39/40**。唯一失败 `collect_tick_latency`（两轮 p50≈28-34ms > 15ms 预算）；V13 记录为 40/40，判定为当前机器负载（并行构建中）所致的环境性失败，建议空闲时复跑再定级，非交互缺陷。
- `SuperTaskMgr.exe --smoke 150`：退出码 0，正常。
- `%LOCALAPPDATA%\SuperTaskMgr\logs\stm.log`：无异常；历史记录中唯一的 ERROR 是非管理员实例启动时按持久化 netEtw=true 尝试开 ETW 被拒（Win32 5），属预期诚实失败，且 UI 侧 one-shot 回读已把开关滚回 false。
- config.json 实测佐证 P0-2（colW_desc=726.000 为像素值进入权重槽）。

## 统计

P0 ×2、P1 ×5、P2 ×11。P0-1 与 F1 的三条链路同根因（同一函数），修复时需协调，避免两处重复改 `DrawConfirmDialogs`。

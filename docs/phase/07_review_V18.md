# 终审报告 V18 —— 回归与资源生命周期（P3/H-A/Phase-6 功能轮后）

- 评审人：V18（独立终审 subagent，与其他评审者互不知情；未用 git 历史；除本报告外未修改任何仓库文件）
- 日期：2026-09-19
- 范围：确认框每帧渲染回归（MemCleanup/closeAsk/亲和性/宿主服务/关于/壁纸选图）、本轮新代码资源
  生命周期（MemCleanup 批量 job / CrashLog EVT / Sensors 新增 WMI / 壁纸纹理 / WM_CLOSE 拦截）、
  cfg 新键往返、线程与队列、实跑验证
- 方法：通读本轮全部改动（main.cpp / AppContext.h / ConfirmAction.h / Pages.cpp / Pages3.cpp /
  Theme.* / AboutInfo.h / AboutUi.* / MemCleanup.h / ThemeCfg.h / Wallpaper.* / D3DRenderer.* /
  Sensors.* / CrashLog.cpp / 四个新 selftest）+ 针对性问题用独立探针程序实证 + 全量产物实跑
- 产物基线：build/Release（01:11 构建，晚于最新源码 01:00；01:29 增量重编译 0 警告 0 错误）

## 统计

- P0：1（壁纸永不渲染 —— 新功能 100% 无效且状态谎报，发布阻塞）
- P1：0
- P2：6
- 上两轮缺陷回归核查：**均未复发**（详见"已查无问题"第 1、3、4 条；另 V15/V16 的 P2 修复已落地：
  kGcHotkeyId 改 0xB34D、亲和性查询失败文案、perfCsvWanted 启动复位）
- 自测：stm_selftest **85/85 双轮通过**（exit 0×2）；`--smoke 150` exit 0；`--autotest
  dialogclick/kill/tree/startup` 全部 PASS（autotest_result.log 尾行核对）；真实实例 90 s 双采样
  WS 125.5→125.9 MB、Private 112.4→112.8 MB、句柄 699→700，无异常增长（基线符合 125–140 MB 预期）。

---

## P0

### P0-1 自定义壁纸在所有路径上永不渲染：背景绘制列表在 ImGui::NewFrame 之前写入（DrawData 永不含壁纸）

- 文件:行：`src/app/main.cpp:474-479`（帧循环顺序 `renderer.BeginFrame()` →
  `ui::WallpaperDrawBackground(...)` → `ui.NewFrame()`）；唯一绘制实现
  `src/app/ui3/Wallpaper.cpp:247-264`（`ImGui::GetBackgroundDrawList()` 上 AddImage + 遮罩矩形）。
- 证据（源码层）：imgui 1.92.9（vendored）中背景绘制列表按帧戳失效与收集——
  `imgui.cpp:5298-5306`：`GetViewportBgFgDrawList` 仅当 `BgFgDrawListsLastTimeActive != g.Time`
  时 `_ResetForNewFrame()` 并把戳更新为当前 `g.Time`；`imgui.cpp:5608`：`g.Time` 只在
  `NewFrame()` 内递增；`imgui.cpp:6205-6208`：`Render()` 仅当
  `BgFgDrawListsLastTimeActive[0] == g.Time` 才把背景列表并入 DrawData。main.cpp 在 NewFrame
  **之前**画壁纸 → 打戳为上一帧的 `g.Time` → 本帧 `Render()` 判"非本帧活跃"而跳过 →
  壁纸顶点永不进入渲染数据；下一帧再进入时又因戳过期被整体清空重画，循环往复。
- 证据（实证）：用 vendored imgui 构建独立探针（`D:\tmp_stm_v18\probe.cpp`，编译链接
  third_party/imgui 全部 TU），完全复刻 main.cpp 的调用顺序对照测试：
  - 顺序 A（NewFrame 之前 AddRectFilled，即现 main.cpp 顺序）：5 帧全部
    `DrawData vtx=0` —— 内容从未被渲染；
  - 顺序 B（NewFrame 之后同样调用）：5 帧全部 `DrawData vtx=4` —— 内容正常并入。
- 后果：用户选图成功后 toast「壁纸已加载」、外观菜单显示「已加载：…（W×H）」、
  `WallpaperAutoRestore` 每次启动解码+建纹理（显存常驻）、每帧执行 AddImage+遮罩绘制命令——
  但屏幕上**永远是纯色背景**。新功能整体无效且 UI/日志（`已加载壁纸 WxH`）谎报已生效，
  属"新功能 100% 失效 + 不诚实状态"，判 P0（发布阻塞）。selftest 与 --smoke 均不覆盖
  （选图对话框 headless 不可驱动，smoke 只画状态行），故此前未被发现。
- 修法（一行移动）：把 `ui::WallpaperDrawBackground(...)` 移到 `ui.NewFrame()` 之后
  （main.cpp 帧循环中改为 `ui.NewFrame(); ui::WallpaperDrawBackground(...); DrawShell(*ctx);`）。
  背景绘制列表天然渲染在所有普通窗口之下，绘制层级不受移动影响。修后建议补一条可自动化的
  回归：加载测试图后断言 `ImGui::GetDrawData()->TotalVtxCount` 相对无壁纸基线有增量（或最低
  限度在 README 人工验收清单中加"选图后背景立即变化"）。

---

## P1

无。

---

## P2

### P2-1 CloseAsk 双击 X 的状态机：pending 滞留导致「最小化到托盘/取消」后弹窗重开（最小化后隐形滞留）
- 证据：`Pages.cpp:294-301` 仅在 `confirm.kind == None` 时消费 `closeAskPending`；模态打开期间
  再次点击 X（`main.cpp:345-356` 每次都置位）后 pending 保持 true。用户随后点「最小化到托盘」
  （窗口 SW_HIDE、CloseConfirm）→ 下一帧 kind==None 且 pending==true → 关闭询问模态在**已隐藏
  的窗口里重新打开**并常驻；托盘恢复窗口后用户看到悬挂的关闭询问。点「取消」同理会在下一帧
  又弹一次（第二次 X 请求被补执行）。
- 修法：CloseAsk 三个按钮分支与 Esc 退出路径统一 `app->closeAskPending.store(false)`（或模态
  打开期间 DrawConfirmDialogs 直接丢弃新置位的 pending）。

### P2-2 WM_CLOSE 全量拦截的互操作后果：外部优雅关闭请求永远无法退出
- 证据：`main.cpp:345-356` 对所有 WM_CLOSE 置 handled（不交 DefWindowProc）；本机实测
  `taskkill /IM SuperTaskMgr.exe`（发 WM_CLOSE）后 3 秒进程仍存活（WS 124.9 MB），须
  `/F` 强杀。默认 closeAction=0 时外部请求转化为"应用内弹窗"，但无人点按的自动化/
  安装器/进程管理器场景从此无法优雅关闭本应用（改动前 DestroyWindow→干净退出）。
- 修法（可选其一）：在 onMessage 里对"会话结束"类消息（WM_ENDSESSION）直接 wantExit 快速
  退出；或在文档/README 已知限制中写明"请使用托盘退出或 taskkill /F"。

### P2-3 壁纸「加载中…」提示不可达（同步加载同帧完成，状态机永远展示不到）
- 证据：`Pages.cpp:2290-2294`：`Ui().wallpaperBusy = true; WallpaperLoad(...); wallpaperBusy =
  false;` 三条语句在同一次 Draw 的按钮回调内完成。状态行渲染在按钮之前、下一帧 busy 已
  复位——`if (Ui().wallpaperBusy)` 分支（:2304-2306）永远不成立，属不可达代码；几十 ms
  阻塞期间用户得不到任何提示。
- 修法：要么删掉 wallpaperBusy 与该分支，要么把加载挪到 jobs 后置完成（需设备级同步，注释
  已论证代价大），建议前者并把状态行改为加载完成后由 toast 反馈（现状已是如此）。

### P2-4 ops 串行队列新增两个占用源（V16 P2-4 的叠加项，未达 P1）
- 证据：单 worker JobQueue（core/Jobs.cpp）上，本轮叠加：a) 传感器刷新
  （Pages3.cpp:1439 经 jobs [app]）新增 2–3 次 WMI 枚举（Win32_Temperature /
  ThermalZoneInformation / DPTF 双命名空间，Sensors.cpp:1458-1525）；b) MemCleanup 单 job
  批量至多 10 次 TrimWorkingSet（ConfirmAction.h:284-310）。与 kill/suspend 同队，头部阻塞
  量级为几十 ms～百 ms，远小于既有崩溃查询 10 s 项；状态栏队列计数可见。
- 修法（后续）：沿 V16 P2-4 的建议——只读枚举类 fetch 独立低优先级队列。

### P2-5 死代码与重复实现：D3DRenderer::CreateTextureFromMemory 全仓库无调用点
- 证据：`D3DRenderer.cpp:72-92` 新增的纹理上传助手在本轮无任何调用（Wallpaper.cpp:122-143
  自带一份等价实现 `CreateRgbaTextureSrv`）；全仓库 grep 仅声明/定义两处。/W4 不报未引用
  成员函数，属悄然累积的重复路径。
- 修法：删除，或让 Wallpaper.cpp 改用它（注意 selftest 无 d3d11 链接的头文件约束）。

### P2-6 壁纸纹理退出路径无显式释放；About 模态打开期间每帧读注册表
- 证据：a) `Wallpaper.cpp:60` g_srv 只在 Clear/替换时 Release，main 退出序列
  （main.cpp:524-534）无壁纸清理调用——依赖 D3D COM 引用计数与进程回收（无泄漏、无崩溃，
  仅洁癖级）；b) `AboutUi.cpp:22-41` WindowsBuildText 每次 Draw 都 RegOpenKeyEx+RegGetValue×2
  （仅模态打开期间，约 60 次/秒）。
- 修法：main 退出前调用一次只释放纹理的内部函数（或注释声明依赖进程回收）；About 首帧
  缓存 Build 文本。

---

## 已查无问题清单（证据与核对结论）

1. **确认框每帧渲染主回归——未复发，六个抽查对象全部走正确模式**：
   - DrawConfirmDialogs 骨架保持 F1 修复模式（Pages.cpp:317-343：请求长期有效、OpenPopup 仅
     一次、BeginPopupModal 每帧、Esc 退出即 CloseConfirm 无僵尸模态）。
   - **MemCleanup**（Pages.cpp:360/437-495）：RequestConfirmMemCleanup 冻结 Top10 快照后置
     confirmOpenRequested，同走 ##confirm 每帧模态；零勾选时执行钮禁用；vector<bool> 代理经
     栈上 bool 中转（:462-465）；取消/动作双路径 CloseConfirm。
   - **closeAsk**（Pages.cpp:289-307/516-548）：closeAskPending 原子位每帧消费 → 同一
     ##confirm 模态；三按钮焦点 `SetKeyboardFocusHere(2)` 落在取消（V8-P1-2 规则）；不参与
     dialogclick 哨兵（:308-309 有注释）；「记住我的选择」只在按下对应按钮时写 cfg。
   - **亲和性**（Pages.cpp:1560+）/ **宿主服务**（GcPages.cpp:560-637）：均为
     openRequested 一次性 OpenPopup + 每帧 Begin + Appearing 一次聚焦 + 双按钮
     CloseCurrentPopup；Esc 路径 `!IsPopupOpen && !openRequested` 早退，无隐形模态。
   - **关于**（AboutUi.cpp:77-122）：点击 OpenPopup 一次 + `IsPopupOpen` 守卫 + 每帧
     Begin；纯展示对话框无待清理状态。
   - **壁纸选图**：非模态（外观菜单面板 + GetOpenFileName Shell 对话框）；状态行/滑条/
     提示经 --smoke 离屏窗口覆盖渲染路径；选图与加载按钮点击路径按设计留人工验收（本轮
     P0-1 恰好证明该人工验收缺失了实际显示效果核对）。
   - 实跑：`--autotest dialogclick` PASS（"真实管线点击生效：子进程退出、模态已关闭"），
     kill/tree/startup PASS（log 尾行核对），单帧化哨兵未触发。
2. **MemCleanup 批量 job 资源生命周期**：MakeMemCleanupJob（ConfirmAction.h:284-310）按值捕获
   [app(shared_ptr), elevated, items, selected, purgeStandby]，无裸指针/引用捕获；逐项
   `ops::TrimWorkingSet`（ProcessOps.cpp:423-446）自带 ProtectedReason 硬门禁（注释与实现
   一致）+ OpenVerified 的 (pid,createTime) 身份复核；单项失败只计数不中断；PurgeStandby 失败
   补发独立 note；聚合 toast 文案标注"估计值"；SelectTopCleanupCandidates/DefaultCleanupSelection
   纯函数有 selftest（ui_memcleanup_test）。
3. **CrashLog EVT 句柄全路径（V15-P1-1 修复仍覆盖）**：QueryOneChannel 结构未动——
   EvtBatchGuard（CrashLog.cpp:445-452）兜底整个 EvtNext 批：maxCount 截断中途退出、EvtNext
   失败 return false、`returned < kBatch` drain 三条退出路径都关闭未接管的句柄；取用处
   `UniqueEvt ev(batch[i]); batch[i]=nullptr;` 移交后置空无双关。本轮 350 行改动全部在解析层
   （纯字符串）：UnescapeXml 数值实体边界（code<=0xFFFF、*endp==';'、裸 & 原样）、
   AttrValueInTagText 整词匹配+双引号风格、ParseEventData 自闭合/缺闭合标签/CDATA、
   FindTokenEndingWith 尾标点剥离、IFindFrom 的 ++pos 推进均无越界/死循环；四种新解析形态有
   4 条 selftest（crashlog_parse_wer_named / blob_unnamed / hang_1002 / unnamed_falls_back_to_pid）。
4. **Sensors 新增 WMI 枚举（COM 配对/SAFEARRAY）**：三个新查询全部走既有 WmiQuery/ReadProp——
   V16-P1-1 的修复在位（Sensors.cpp:276-289：拷贝后统一 `VariantClear`，BSTR 手工释放已由
   其取代，字节数组 SAFEARRAY 随之销毁），新查询的 Name/InstanceName/CurrentTemperature 属
   BSTR/VT_I4 等既有分支；CoInit/CoUninitialize、枚举器释放路径未改动；DPTF 命名空间缺失
   notSupported 静默跳过、单位两级合理性门禁（开尔文十分位→摄氏十分位→丢弃）不做假数据；
   新增 sensors_temp_sources_labeled / sensors_dptf_silent_when_absent 两条 selftest。
5. **WM_CLOSE 拦截与退出路径顺序**：拦截→`closeAskPending`（atomic）→UI 每帧消费（除 P2-1
   的双击边角，状态机正确）；三出口（wantExit / SW_HIDE / CloseConfirm）都经 CloseConfirm
   清状态。正常退出 teardown 顺序完整：`SaveSessionFromCtx → cfg.Save → StripColWidthKeys
   → tray.Remove → GcHotkeyUnbindWindow → SetBalloonSink(nullptr) → collect.Stop →
   jobs.Shutdown(2000) → ui.Shutdown → renderer.Shutdown → win.Destroy`；balloon sink 在
   tray.Remove 后、collect 停止前丢弃（中间窗口期触发仅 Shell_NotifyIcon 失败，Tray 对象
   仍存活，无 UAF）；jobs 在途任务靠 ctx shared_ptr 存活（V7-P1-3 机制未动）。headless 的
   退出走 wantExit 不经 WM_CLOSE，--smoke/--autotest 实测不受拦截影响。
6. **cfg 新键读写成对、默认值、无冲突**：closeAction（写：CloseAsk 记住+按钮；读：onMessage
   经 NormalizeCloseAction 钳制非法值→0；默认 0=每次询问；ui_close_action_cfg selftest 锁定
   往返）；themeMode（ThemeModeFromInt 越界回退 Dark，启动/菜单/WM_SETTINGCHANGE 三处读一致，
   菜单写）；wallpaperMask（ClampMask 钳 [0,0.85] 含 NaN→0；滑条写、帧循环与面板读）；8 个
   perfShow*（visible() 同点读写成对，默认除 CtxSwitch 外全开）；colW_* 剔除清单
   （ColWidthCfgKeys = 0..10 + badges + desc）与 PersistWidths/LoadPersistedOnce 写读一一对应，
   两步软删除+按值剔除的"重置后重新调整"场景推演正确（数值行保留、""行剔除、
   StripColWidthKeysFromFile 拒绝改写不认识的行形状返回 -1 不截断，colw_strip_file selftest
   覆盖）。全部新键名与存量键无冲突（全量键名枚举核对）。
7. **线程与队列**：本轮唯一新 job（MemCleanup 批量）shared_ptr 捕获（见第 2 条）；CloseAsk
   明确不走执行表（ExecuteConfirmedAction 返回 false 并注释）；崩溃查询/宿主服务/控制信息等
   既有 job 捕获模式未动；JobQueue Shutdown 语义未动。长任务阻塞评估见 P2-4。
8. **主题系统（H-A）**：Theme::Apply 仅在窗口线程（启动/菜单/WM_SETTINGCHANGE，均无活跃帧
   冲突）；uiThemeReady 门禁防 ImGui 上下文就绪前响应广播；浅色强调色全套（ThemeAccentColor
   双模式表）；ReadAppsUseLightTheme 读不到→诚实回退深色（selftest 覆盖）；布局值双模式一致
   不回流；ColDone/Fail/Warn/Info、徽标、类别高亮全部收口到 ThemeAccent（Pages.cpp 硬编码色
   清除），浅色下可读性核对通过（ColMuted 0.60/0.62/0.68 浅底可读）。
9. **壁纸质 rest（除 P0-1 渲染与 P2-6 外）**：ReadFileBytes/WriteFileBytes UniqueHandle 配对；
   stbi 缓冲经 PixelFreer RAII；CreateRgbaTextureSrv ComPtr→Detach 单引用移交；失败路径
   （解码失败/纹理失败/副本写失败）旧壁纸与旧副本完好、srv Release 配对；512 MiB/4096² 双门禁；
   仅 Wallpaper.cpp 一个 TU 编译 stb（#pragma warning(push,0) 隔离）；存储副本"解码成功后才
   覆盖"保证 AutoRestore 不被坏图清空；扩展名大小写/目录含点用例自测覆盖。
10. **Pages3/其余改动**：LhmGroupOfCpuTemps 提升门（temperature + core/package/tctl/tdie/ccd
    双条件）不把 Voltages/Clocks/DIMM 卷入 CPU 组；CPU 组 LHM 计数与文案诚实（每核 DTS 需
    内核驱动声明）；性能页网格两列布局 BeginGroup 配对、8 个显隐复选框读写即存；硬故障/
    上下文切换新环 hasData 诚实空态（NaN 不入线）；ctxSwitch 聚合仅计有效样本。
11. **实跑记录**：构建 0 警告 0 错误；stm_selftest 85/85 ×2（exit 0）；`--smoke 150` exit 0；
    四条 --autotest PASS（log：dialogclick 真实管线点击生效 / kill 终止 cmd.exe / tree 终止
    3 个 / startup 禁用编码生效+备份）；真实实例 90 s 双采样（6 s vs 93 s）：WS 125.5→125.9 MB、
    Private 112.4→112.8 MB、句柄 699→700、CPU 累计 1.3 s——无异常增长，符合 125–140 MB 基线。
    附带实测：taskkill 优雅关闭信号被拦截（P2-2 记录），进程最终经强杀结束。

## 上两轮缺陷回归核查结论（对应任务 1）

- V14"确认框点击无反应/单帧模态"：未复发（骨架 + 五处既有新模态 + 本轮 MemCleanup/closeAsk
  均为每帧模式；dialogclick 实测 PASS）。
- V15-P1-1 CrashLog maxCount 截断句柄泄漏：EvtBatchGuard 仍在且覆盖全部退出路径（第 3 条）。
- V15-P1-2 PerfCsv 磁盘满静默丢行：本 Tw 轮未见回归；perfCsvWanted 启动复位（V16-P2-1）已落地
  （GcPages.cpp:698-700）。
- V16-P1-1 WMI SAFEARRAY 泄漏：VariantClear 统一 teardown 在位（第 4 条）。
- V16-P2 系列：P2-2（热键 id→0xB34D）、P2-3（亲和性查询失败文案）已修；P2-4（队列头部阻塞）、
  P2-5（切页后页面级模态不渲染）、P2-6（按 pid 缓存无淘汰）仍开放（均为体验级，不阻塞）。

## 结论

上两轮的确认框与资源泄漏缺陷全部未复发，本轮新增代码（MemCleanup、closeAsk、主题、关于、
Sensors 扩展、CrashLog 解析）的资源生命周期与线程捕获审查通过，实跑全绿。唯一发布阻塞项是
P0-1：壁纸功能因背景绘制列表在 NewFrame 之前写入而 100% 不渲染（独立探针实证 DrawData 恒空），
一行调用顺序移动即可修复；修复后本批可发布。P2 六项均为状态机边角/互操作/洁癖级改进。

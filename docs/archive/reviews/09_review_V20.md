# 终审报告 V20 —— R-Fix/R-NetData 数据层全仓回归 + 本轮新增面

- 评审人：V20（独立终审 subagent，与其他评审者互不知情；未用 git；除本报告外未修改任何仓库文件；
  自建私有探针编译于 %TEMP%，不落仓库）
- 日期：2026-09-19
- 范围：本轮新代码生命周期（HeaderLayout / WallpaperShutdown-WallpaperClear / Logo RCDATA /
  AboutUi 纹理 / 托盘 HICON / MemCleanup 固定行）、上两轮缺陷回归（确认框点击 / 壁纸渲染 /
  关于点击）、盲区排查（状态栏两段式 / 性能页固定行 / CrashLog 新样本 / AdapterInfo 契约一致性）
- 方法：通读 main.cpp / Pages.cpp(外壳+状态栏+PerfPage) / HeaderLayout.h / Wallpaper.* /
  AboutUi.* / Tray.* / AutotestDialog.* / D3DRenderer.* / Theme.* / CrashLog.cpp / AdapterInfo.* /
  MemCleanup.h / app.rc / Win32Window.cpp + 独立探针实证（ParseEventXml 喂真实事件 XML、
  裸 GAA 链速枚举）+ 全量产物实跑
- 产物基线：**发现 build/Release 产物过期**（SuperTaskMgr.exe 06:52 / stm_core.lib 01:10，
  早于最后一轮源码修改 07:33–07:48；build_cn 07:51 才是新鲜构建）。评审以当前源码重建
  build/Release（07:59，**0 警告 0 错误**）后实跑。发布打包前须确认以新鲜构建为准。

## 统计

- P0：0
- P1：1（AdapterInfo 链路速度"未知值 -1"未归一化，契约被违反，实机 22/47 行命中）
- P2：4（Light+壁纸可读性 / HICON 共享句柄声明不符 / AboutUi 纹理跨 Shutdown / ENDSESSION 不保存配置）
- 观察项：collect_tick_latency 在外部重负载下会失败（本轮第 2 轮 selftest 与评审者自身的 cl.exe
  编译并行时 p50≈17.2ms 触发；串行复跑两轮均 93/0）——测试对机器负载敏感，CI 需串行保障
- 上两轮用户报告缺陷回归核查：**均未复发**（见"已查无问题"第 5–7 条）

---

## P0

无。

---

## P1

### P1-1 AdapterInfo：GetAdaptersAddresses 的"链路速度未知 = 0xFFFFFFFFFFFFFFFF"未归一化，linkSpeedMbps 产出 1.84e13 的荒谬值（契约明文"0 = API 未上报链路速度"被违反）

- 文件:行：`src/collect/AdapterInfo.cpp:188`
  `n.linkSpeedMbps = std::max(a->TransmitLinkSpeed, a->ReceiveLinkSpeed) / 1'000'000ULL;`
  契约：`src/collect/AdapterInfo.h:32-34`——"0 = API 未上报链路速度"。
- 证据（实证）：独立探针（%TEMP/v20/probe/gaa.cpp，裸 GAA、与实现完全相同的
  INCLUDE_ALL_INTERFACES 标志组）在本机（Win11 22631）枚举 47 个适配器，其中 **22 行**
  TransmitLinkSpeed/ReceiveLinkSpeed = 18446744073709551615（即 -1，OS 的"未知"约定），
  **包括 up=1 的适配器**（"本地连接\* 7/8/9"、多个 WFP/QoS 过滤层）；这些行经实现公式得到
  linkSpeedMbps = 18446744073709 Mbps（≈18.4 万亿），而非契约承诺的 0。本机真实自测
  `adapters_enum_ok`（adapter_test.cpp）不校验速度字段，故 93 条自测全绿也拦不住。
- 后果：数据层对下游（下一 Phase 的"以太网/WLAN"UI 与 GetIfTable2 联接）持续供给荒谬速度值，
  一旦 UI 直显即"18,446,744,073,709 Mbps"，违反项目"诚实数据、绝不伪造/失真"红线；这正是
  契约写明"0 = 未上报"要防的情形，属"契约与实现一致性"缺陷。因当前尚无 UI 消费者、未对用户
  可见，判 P1（发布前必修）而非 P0。
- 修法（一处 + 一测）：
  `const unsigned long long raw = std::max(a->TransmitLinkSpeed, a->ReceiveLinkSpeed);
  n.linkSpeedMbps = (raw == ~0ull) ? 0ULL : raw / 1'000'000ULL;`
  并在 adapter_test.cpp 增补纯函数/实机钉子（如把换算提为 `LinkSpeedMbps(uint64 tx, uint64 rx)`
  纯函数，钉 -1→0、0→0、1e8→100）。顺带：`ifIndex`（:184）在纯 IPv6 拨号伪接口上可能为 0，
  与"统一接口索引"表述略有出入，联接时注意（观察项，不判级）。

---

## P2

### P2-1 浅色主题 + 壁纸叠加：文字可读性失控（推透明只看 WallpaperActive()，与主题正交）

- 证据：`src/app/ui/Pages.cpp:2771-2775` 壁纸激活时对 ##approot 推透明 WindowBg/ChildBg；
  `src/app/Theme.cpp:75-104` 浅色主题为深色文字（StyleColorsLight 基础、TextDisabled 0.42）。
  壁纸背后叠的是黑色可读性遮罩（wallpaperMask 至 0.85，`Wallpaper.cpp:282-285`）——遮罩越深
  背景越暗，浅色主题深文字在深背景上对比度越差；默认深色主题不受影响。渲染逻辑本身正确
  （--autotest wallpaper PASS），纯为"浅色×壁纸×高遮罩"组合的 UI 可读性缺陷。
- 修法：`wallpaperUnder && ThemeIsLight()` 时一并推浅色场景专用的 Text/TextDisabled
  （或给该组合强制遮罩下限/提示切回深色），一行级改动。

### P2-2 HICON"共享句柄无需 DestroyIcon"声明与 MSDN 不符（3 处 LoadImageW 均缺 LR_SHARED）

- 证据：`src/app/main.cpp:390-403`（ICON_BIG/ICON_SMALL 两个）与 `src/app/ui/Tray.cpp:33-36`
  （托盘小图标）均 `LoadImageW(..., LR_DEFAULTCOLOR)` 无 LR_SHARED。MSDN LoadImage：非
  LR_SHARED 加载的图标为非共享句柄，不再需要时应 DestroyIcon。注释声明"共享资源句柄，
  进程生命周期内有效，无需 DestroyIcon"——"进程内始终有效"半句成立（句柄被 WM_SETICON/
  NOTIFYICONDATA 持有），"共享/无需销毁"半句无依据；后果仅为进程退出时 3 个 HICON 不销毁
  （OS 回收），无实际危害。
- 修法：三处加 `LR_SHARED`（资源图标 + 进程生命周期使用的标准写法，同时使声明成真）；
  或退出路径 DestroyIcon。

### P2-3 AboutUi 徽标纹理跨 renderer.Shutdown 存活（静态 ComPtr 无重置入口）

- 证据：`src/app/ui/AboutUi.cpp:80-84` 匿名命名空间静态 `g_logoSrv`（ComPtr<ID3D11ShaderResourceView>）
  全仓无任何 Reset/Shutdown 接口；退出顺序 `main.cpp:592-594`：WallpaperShutdown → ui.Shutdown →
  **renderer.Shutdown()（device_.Reset()）** 之后，静态析构在 CRT atexit 才对已被设备强制销毁的
  SRV 调 Release。D3D11 子对象在设备销毁后被置为僵尸态、其上 Release 安全（外部引用计数独立
  递减），实机长跑/退出无崩溃——属资源卫生问题而非可复现缺陷；与 V18 已修的"壁纸纹理显式
  释放"不对称（壁纸有 WallpaperShutdown，关于徽标没有对应物）。
- 修法：AboutUi 导出 `ShutdownAboutUi(){ g_logoSrv.Reset(); g_logoTried=false; g_logoTexture={}; }`，
  在 main.cpp `renderer.Shutdown()` 之前调用（与 ui::WallpaperShutdown 并列）。

### P2-4 WM_ENDSESSION/WM_QUERYENDSESSION 未处理：系统关机/注销时进程被强杀，会话与配置静默丢失

- 证据：全仓无 ENDSESSION 处理（Win32Window.cpp 仅 WM_DESTROY；main.cpp onMessage 仅
  WM_CLOSE/WM_SETTINGCHANGE/WM_HOTKEY）。关机流程：DefWindowProc 对 QUERYENDSESSION 默认
  放行 → ENDSESSION(TRUE) → 系统超时强杀 → `main.cpp:575-581` 的 SaveSessionFromCtx /
  cfg.Save / StripColWidthKeysFromFile 不执行 → 关机前的会话矩形/激活页/主题/刷新间隔/
  closeAction 等改动全部丢失且无提示。V18-P2-2 曾给出"处理会话结束消息"的备选修法，本轮
  仅落地了 closeAction==1 的 WM_CLOSE 透传，该缺口仍在。
- 修法：onMessage 增加 `WM_ENDSESSION`（wParam=TRUE）→ 保存会话+cfg 后置 wantExit=true
  （保存量级为毫秒级，在 5s 关机预算内）。

---

## 已查无问题清单（重点核对项逐条）

1. **HeaderLayout 纯函数**：仅头文件、零状态、零 ImGui 依赖；装箱循环每轮隐藏一名 victim 或
   break，终止性成立；priority-0 钳制会连带隐藏被压住的可见项，钳后不重叠；退化输入
   （count<=0 / 负 spacing / 负 leftFlowEndX / count>kHeaderMaxItems 截断）均有护栏。
   `header_layout_fuzz_invariants / degenerate_clamp / flow_segment_fits / toolbar_about_any_width /
   statusbar_right_group` 五条自测钉住。
2. **WallpaperShutdown / WallpaperClear 语义**：无矛盾。退出路径唯一调用 Shutdown（main.cpp:592，
   只释放 GPU 纹理、保留 %LOCALAPPDATA% 副本），AutoRestore（main.cpp:418）下次启动据此恢复
   ——"退出删副本导致无法恢复"的死代码已根除；Clear 仅由用户「关闭壁纸」（Pages.cpp:2368）
   与私有探针 UiFixProbe.cpp:409 调用（删除副本+释放），语义各归其位；副本只在解码+建纹理
   成功后写入（Wallpaper.cpp:207-218），坏选择不可能毁掉恢复源。小疵（不算缺陷）：
   Wallpaper.h:36-37 / Wallpaper.cpp:257-258 残留未清理的中英混排注释。
3. **Logo RCDATA 句柄**：app.rc `1 ICON` + `200 RCDATA logo_256.png`；AboutUi.cpp:89-93
   FindResource→LoadResource→LockResource 用法正确——Win32 下 LockResource 并不真锁、
   UnlockResource 是无操作、FreeResource 不需要，资源数据随模块存活至进程结束——"Unlock 不
   需要、句柄进程存活有效"声明核实成立。stb 解码失败/资源缺失静默不显示，rgba 用后即 free
   （:114）。
4. **MemCleanup 固定行 × 图表全隐藏**：`DrawMemQuickActionRow` 是 PerfPage::Draw 的第一条语句
   （Pages.cpp:1900），位于 `lastTick==0` 早退与全部 perfShow\* 复选框之前——8 块图表全部隐藏、
   或尚无采集数据时该行仍渲染，8 个显隐复选框也仍可勾选恢复；「内存加速」三入口（顶行/
   内存条块内/工具条）同走 RequestConfirmMemCleanup → DrawConfirmDialogs 每帧模态。
5. **回归①确认框点击**：DrawConfirmDialogs 保持"请求长期有效 + OpenPopup 一次 + BeginPopupModal
   每帧"的 F1 模式（Pages.cpp:290-355）；CloseAsk 三按钮与 V18-P2-1 的 pending 清除修复在位
   （:543/:552）；本轮新增的模态/菜单改动（about 模态、外观菜单、状态栏）不影响其调用位置
   （DrawShell 末尾每帧无条件）。`--autotest dialogclick` **PASS**（真实管线点击：子进程退出、
   模态已关闭）。
6. **回归②壁纸渲染（V18 + R-Fix 两修复合流）**：main.cpp:503-508 保持 V18 顺序
   （NewFrame → WallpaperDrawBackground → DrawShell）；R-Fix 的透明推送（DrawShell:2771-2775）
   条件只看 WallpaperActive()，Push/Pop 平衡、仅作用于 ##approot，弹窗/toast/确认框在 End 之后
   不受影响——两修复正交合流。`--autotest wallpaper` **PASS**："帧顶点=7160 壁纸绑定绘制命令=1
   active=1"（壁纸真实进入 DrawData，不只是加载）。Light 叠加的可读性见 P2-1。
7. **回归③关于点击**：工具条「?」先于菜单提交（悬停竞争根因消除）、LayoutHeaderRight 实测定位；
   `--autotest about` **PASS**（模态打开并保持 ≥4 帧 → 真实点击「关闭」→ 退出）。
8. **状态栏两段式**：左段（采集）逐段 FlowSegmentFits 降级，锚点段必显；右段（外观/开销）
   LayoutHeaderRight 优先级装箱，热键(2)→徽标(1)→帧耗时(0) 依次先藏、帧耗时永不藏；窄窗热键
   隐藏后 ⋮ 菜单含等价开关保持可达。行为在 85 用例代 → 93 用例代之间由 5 条新 header 自测
   钉住，实机 smoke/autotest 无异常。
9. **性能页顶部固定行**：与第 4 条同——不受 lastTick 早退、图表显隐影响；树形/过滤属进程页，
   --smoke 离屏全页绘制通过，无叠加回归。
10. **崩溃页解析对新样本**：本机 Get-WinEvent 实抓三形态新样本（.NET Runtime 1000 无名中文
    blob / WER 1001 具名 P1 / Application Hang 1002 AppName+路径），经独立探针（链接 stm_ops
    的 ParseEventXml）实证：.NET blob 无 .exe 记号 → 诚实兜底 "PID 87360" + 首行摘要；
    WER → "powershell.exe"；Hang → "SuperTaskMgr.exe"。三者摘要均无"未知"占位。EvtNext 批次
    RAII、双通道聚合错误、单/双引号与数字实体解码均在位。
11. **托盘**：TaskbarCreated 重建、SetTip/ShowBalloon 菜单三态、WM_NULL 置后均正确；HICON
    声明不符见 P2-2（无功能危害）。
12. **实跑汇总**：build 0 警告 0 错误；stm_selftest 93/0 ×3 通过（另 1 轮因评审者并行编译
    cl.exe 触发 collect_tick_latency p50 17.2ms——负载敏感观察项，串行复跑即绿）；`--smoke 150`
    exit 0；`--autotest dialogclick/about/wallpaper/kill/tree/startup` 6/6 PASS（autotest_result.log
    逐条核对）；真实实例 60s 双采样 WS 124.4→124.7MB、Private 111.0→111.3MB、句柄 682→683，
    无异常增长（基线 125–140MB 区间内；本轮新增纹理仅关于徽标 ~0.26MB ≤ 0.3MB，壁纸未启用
    时为零常驻）。

# 终审报告 V22 —— A1 统一风格（外观模态/关于按钮/WindowsBuildText）/ 图表 Phase A / 网络适配器 A2 全仓回归

- 评审人：V22（独立终审 subagent，与其他评审者互不知情；未用 git；除本报告外未修改任何仓库文件；
  探针与测试图像全部放 %TEMP%\v22probe，不落仓库）
- 日期：2026-09-19
- 范围：①历史缺陷回归（确认框点击/壁纸渲染/关于点击）；②本轮新代码生命周期（外观模态 cfg 写频率、
  壁纸 UI 线程同步解码量化、NetAdapterUi+AsyncFetch、PerfChart RingView、WindowsBuildText 句柄）；
  ③既有 102 项自测与六条 autotest、工具条窄窗流式布局、状态栏两段式；④漏网 bug 自由排查
  （时间窗与采集间隔的"秒"语义、CSV/告警并存、cfg 键膨胀、版本宏一致性等）
- 方法：通读 main.cpp / Pages.cpp(工具条+外观模态+外壳+PerfPage) / AboutUi.* / ThemeCfg.h / Wallpaper.* /
  PerfChart.h / NetAdapterUi.h / AsyncFetch.h / Pages3.cpp(适配器+告警) / AdapterInfo.* / Cfg.cpp /
  AutotestDialog.* / ImGuiLayer / Win32Window + 两个独立探针（vendored stb 解码计时、
  WindowsBuildText 注册表读取计时）+ 会话注入式窄窗实证 + 全量产物实跑
- 产物基线：build/Release（SuperTaskMgr.exe / stm_selftest.exe 2026-09-19 09:22–09:23）晚于全部源码
  最新 mtime（09:14，PerfChart.h），**无需重建即为本轮新鲜产物**，全部实跑基于该产物。

## 统计

- P0：0
- P1：1（工具条流式布局窄窗裁剪：实测 ≲730px 窗宽时「主题…/关于」不可达且无任何替代入口；
  1366×768 屏半屏吸附即可触发）
- P2：4（时间窗"秒"标签实为 tick 计数 / 关于模态每帧读注册表 23µs 实测 / 壁纸同步加载量化+两处
  体验尾巴 / 浅色×壁纸可读性（V20-P2-1 未修，顺延））
- 观察项：5（AlertTick 未钳制读阈值 / PushID(ifIndex) 伪接口 0 撞 ID / 外观模态粘滞标志理论卡死态 /
  header_toolbar_about_any_width 测试名与守护对象脱节 / 外观模态无 autotest 哨兵）
- 上两轮历史缺陷回归：**三条全部未复发**（见"已查无问题"第 1–3 条）
- 前轮遗留修复核验：V20-P1-1（链速 -1 归一化）、V20-P2-2/3/4（LR_SHARED / ShutdownAboutUi /
  ENDSESSION 保存）**均已落地并实证**

---

## P0

无。

---

## P1

### P1-1 工具条流式布局在窄窗直接裁剪「主题…/关于」按钮，且两个模态的入口全应用仅此一处——窄窗下功能整体不可达（会话注入实证）

- 文件:行：`src/app/ui/Pages.cpp:2508-2594`（DrawToolbar 自然 SameLine 流式，A1 刻意删除旧「⋮」折叠
  与右缘布局）、`src/app/Win32Window.cpp:44-69`（无 WM_GETMINMAXINFO，窗口宽度无下限）；
  「主题…」入口 Pages.cpp:2570、「关于」入口 Pages.cpp:2582（tray 无等价入口，全仓 grep 证实）。
- 证据（**实机注入实证**，非估测）：备份 %LOCALAPPDATA% session.json 后按 60s 时效写入指定 winW，
  逐宽度跑 `--autotest about`（真实窗口 MoveWindow 到会话矩形，驱动只认本帧真实提交的按钮矩形）：
  - winW=760 → **PASS**（stm.log：按钮矩形 (666,8)-(707,32)，右缘 707 < 内容右界 ≈738，完整可点）；
  - winW=700 → **FAIL**「移动真实光标后『关于』始终未进入悬停态」——按钮已提交（矩形仍发布
    666-707）但右半被裁剪，悬停命中区不完整，真实管线点击落空；
  - winW=640 / 560 → **FAIL**「工具条『关于』按钮从未提交（未渲染或被布局裁剪/遮挡）」
    （IsItemVisible 为 false，btnValid 恒 false）。
  即**阈值 ≈730px 外窗宽**：非提权工具条全流（暂停采集+刷新间隔滑条+以管理员身份重启+清理待机缓存
  +内存加速…+主题…+关于）实测约 700px，再加两侧 padding。
- 后果：Win32 无最小宽度，1366×768 屏（仍常见）的半屏吸附 = 683px → 主题/壁纸/列宽/关于全部不可达，
  且 A1 删除了旧「⋮」菜单后**没有任何替代入口**；状态栏右段只剩热键开关。V19-P1-1 曾因窄窗「?」压住
  「外观/⋮」判 P1；本轮是同一条 reachability 红线的更重回归（不可达 > 重叠）。自测没能拦住的原因：
  `header_toolbar_about_any_width` 在 A1 后只测 LayoutHeaderRight（现仅状态栏右段使用），工具条已无
  任何宽度守护；`--autotest about` 只在默认 1280 宽跑。
- 修法（一行级 + 一守护）：Win32Window.cpp 加 `WM_GETMINMAXINFO`，`ptMinTrackSize.x ≈ 760`
  （760 实证可点；提权态更宽松）——保留 A1 的简单流式布局同时恢复可达性；或在 DrawToolbar 末尾用
  实测游标对比窗口宽，超宽时把溢出按钮折叠为「»」。同时给 --autest about 增加一条"窄窗注入"变体
  （会话注入法本轮已验证可行），防止守护再次脱靶。

---

## P2

### P2-1 性能页时间窗标签是"tick 数"不是"秒"：刷新间隔 ≠1000ms 时「60/120/300/600 秒」全部失真（违背"诚实数据"口径）

- 文件:行：`src/app/ui3/PerfChart.h:31-43`（kHistCap=600、窗口白名单以"秒"命名）；
  `src/app/ui/Pages.cpp:628-631`（AppendHistory 每**采集 tick** Push 一次）、`:1903-1911`
  （Combo「60 秒/120 秒/300 秒/600 秒」）、`:1981`（X 轴 0..window-1）、`:1979`（轴标签「秒」）、
  `:1875`（PlotLine x 比例 1.0 = 每 tick 1 单位）；而刷新间隔滑条允许 500–5000ms（`:2537`）。
- 证据：间隔 500ms 时一个 tick=0.5s，「600 秒」窗实际只覆盖 300s 墙钟（kHistCap 600 tick 上限同理
  减半）；间隔 5000ms 时「60 秒」窗实际覆盖 300s。CSV 记录器的提示（`:2122`「按当前刷新间隔…」）
  是诚实的，图表的"秒"不是。默认 1000ms 档完全正确，故非默认配置才失真。
- 修法：PlotRing 的 x 比例由 1.0 改为 `intervalMs/1000.0`（读一次 cfg），窗口上限按 tick 数 = 秒数×1000/intervalMs
  换算后再 clamp 到 kHistCap；或在白名单文案旁如实标注「（按采集拍计）」。二选一，前者更诚实。

### P2-2 关于模态每帧执行 WindowsBuildText()：2×RegOpenKeyExW+2×RegGetValueW+2×RegCloseKey，实测 23µs/帧（结果运行期不变，纯浪费）

- 文件:行：`src/app/ui/AboutUi.cpp:169`（`EnvField(L"Windows 版本", WindowsBuildText());` 在模态
  每帧体内）；`src/app/ui/AboutUi.h:74-135`（真实读取器 + 注释自认「AboutUi.cpp 每帧调用」）。
- 证据（探针实测，%TEMP%\v22probe\regbench，20,000 次迭代×2 轮，与实现逐 API 相同
  含 KEY_WOW64_64KEY）：**23.1 / 23.8 µs 每次**。模态开着时 60fps 即 ~1,400µs/s 纯系统调用，
  且 Build 号运行期不变；还把帧耗时搅进状态栏「帧 X ms」的显示里。句柄本身无泄漏（见"已查无
  问题"7）——纯粹是缓存缺失。
- 修法：`WindowsBuildText()` 内函数级 `static const std::wstring cached = ...; return cached;`
  （构建号进程内恒定；一行动作，selftest 走的是 With 注入版不受影响）。

### P2-3 壁纸 UI 线程同步解码量化：4K PNG 44–52ms / JPEG 59–62ms（解码本体）+ 读文件/纹理上传/存档复制，合计 ≈60–150ms——**结论：可接受、不必转 jobs**；但两处体验尾巴该收

- 证据（%TEMP%\v22probe\wpbench，用仓库同款 vendored stb_image.h、与 WallpaperLoad 完全相同的
  调用序与闸门，.NET System.Drawing 生成真实 3840×2160 三格式，各 5 次）：
  PNG(2.2MB) read 1.2ms + decode min44/med46/max52ms；JPEG(3.6MB) read 2.5ms + decode 59–62ms；
  BMP(33MB) read 16.3ms + decode 39–46ms（RGBA 展开 31.6MB）；另有 WallpaperLoad 中未计入探针的
  CreateTexture2D(31.6MB SUBRESOURCE_DATA) 上传与存档副本写盘 ≈ 数十 ms → 全程 ≈60–150ms，
  闸门上限 4096×4096 情形约 2 倍。感知上是"点一下卡一帧"，属用户主动、低频、有明确因果的动作，
  失败安全提交点（Wallpaper.cpp:203-228）也不适合搬线程——**转 jobs 收益不抵生命周期复杂度，维持同步**。
- 该收的两条尾巴：
  1. `src/app/ui/Pages.cpp:2364-2367` 置 `wallpaperBusy=true → 同帧同步加载 → =false`，「加载中…」
     提示（:2381-2382）**永远不可能渲染**（死反馈）；要么删，要么真异步时再用。
  2. `src/app/main.cpp:426` WallpaperAutoRestore 在首帧前同步执行，把同样的 60–200ms 计入启动
     耗时；建议挪到首帧之后（或后台），与 P2-2 同属"进帧循环前的隐性开销"。

### P2-4 浅色主题 × 壁纸 × 深色遮罩的文字可读性（V20-P2-1）仍未修——顺延一条

- 证据：`src/app/ui/Pages.cpp:2819-2823` 壁纸激活时仍只推透明 WindowBg/ChildBg；
  `src/app/Theme.cpp:75-107` 浅色主题深色文字未做壁纸场景补偿。外观模态本轮新增的主题三态单选
  让"开着壁纸切浅色"比从前更容易触发。修法同 V20：`wallpaperUnder && ThemeIsLight()` 时一并推
  浅色场景 Text/TextDisabled，或提示切回深色。

---

## 观察项（不判级）

1. `src/app/ui3/Pages3.cpp:2140-2141` AlertTick 直读 alertCpu/alertMem 不钳制（UI 写入端都钳 1..100）；
   手改 cfg=0 会立即触发告警。与全仓"读端也诚实钳制"纪律不一致。
2. `src/app/ui3/Pages3.cpp:339` DrawAdapterCard 以 `PushID((int)a.ifIndex)` 作唯一 ID：GAA 伪接口
   的 IfIndex 可为 0（V20 已注明），两张 ifIndex=0 的卡会撞子窗口 ID。改用 mac/friendlyName 组合更稳。
3. `src/app/ui/Pages.cpp:2435-2448` 外观模态的粘滞 `appearanceOpened` 标志：若 RequestOpenAppearance
   在模态已开时被再次调用会停在"弹出栈有窗但 opened=false"的卡死态（每帧不 Begin）。当前唯一调用点
   是工具条按钮、被模态遮罩挡住而不可达，故无实害；About 的"每帧 IsPopupOpen 判定"模式更稳，建议对齐。
4. `src/selftest/ui_header_test.cpp:65` `header_toolbar_about_any_width` 名字仍指工具条，实际守护对象
   在 A1 后只剩状态栏右段（P1-1 的守护脱靶即源于此）。
5. 外观模态无 autotest 奖哨兵（dialogclick/about/wallpaper 有）；若后续往模态里加更多交互控件，建议补
   一条"RequestOpenAppearance → 模态 ≥4 帧 → 点击关闭"驱动。

---

## 已查无问题清单（重点核对项逐条）

1. **回归①确认框点击**：DrawConfirmDialogs 保持 F1 修复后的「请求长期有效 + OpenPopup 一次 +
   BeginPopupModal 每帧」模式（Pages.cpp:295-351），Esc 复位、树规划 pending、autotest 观察哨兵每帧
   重置俱全；外观模态/关于模态与确认框同层但各自独立 `##id`，互不干扰。`--autotest dialogclick` PASS
   （真实管线点击：子进程退出、模态已关闭）。
2. **回归②壁纸渲染**：main.cpp:511-516 保持「NewFrame → WallpaperDrawBackground → DrawShell」顺序；
   外观模态遮罩滑条每帧拖动 → cfg.SetDouble（纯内存）→ main 每帧读 cfg 传 ClampMask → 背景绘制列表
   实时变化——「滑条/关闭/再加载」循环渲染路径闭环（WallpaperClear 只删副本+释放 SRV，
   WallpaperLoad 失败安全提交点不变）。`--autotest wallpaper` PASS（帧顶点=7134、壁纸绑定绘制命令=1、
   active=1，真实进入 DrawData）。
3. **回归③关于点击**：关于按钮矩形由 DrawToolbar 每帧发布（IsItemVisible 才置 btnValid）、AboutClickDriver
   悬停确认 ≥2 帧才注入、模态 ≥4 帧哨兵、真实点击「关闭」闭环。`--autotest about` PASS（模态打开并保持
   ≥4 帧，点击「关闭」后退出）。
4. **外观模态 cfg 写频率**：遮罩滑条拖动时每帧 SetDouble、主题 RadioButton 点击时一次 SetInt、
   恢复列宽点击时 SoftDelete+Strip 一次——`core/Cfg.cpp` 的 Set* 全部纯内存（items_ 向量），文件写仅
   发生在 WM_CLOSE/退出/WM_ENDSESSION/提权重启四处（main.cpp:381/585）——**无每帧 IO 放大**。
   cfg 键总数有界（约 30 个），colW_* 13 键有 Strip 收口，无键膨胀。
5. **PerfChart RingView 索引**：window>cap → count=min(n,window)≤600 封顶；环绕 offset=(Offset()+start)%600
   正确；RingViewChunks 两段拼接 na+nb==count、nb≤offset 数学恒成立；tickId 为 uint64 单调（1Hz 不会
   回绕），AppendHistory 同 tick 守卫、核心热变更 assign 清历史有注释。ui_chart_test 三条
   （clamp/ring_view/readout）全过，其中 ring_view 显式覆盖满环 650 推、尾窗 60、窗 1..600 逐点、
   window≤0/空环空视图、零拷贝。注意：PerfReadoutText/RingLogicalAt 尚未接线 UI（Phase B 预留），
   现消费者只有 selftest——无运行时风险。
6. **NetAdapterUi + AsyncFetch**：生产者均为无捕获静态函数；MaybeFetch 按值捕获 shared_ptr<AppContext>
   与 shared_ptr<State>，Shutdown 超时后的脱队任务只写自己持有的单元；Submit==0 时回滚 busy；
   Peek 返回 shared_ptr<const Result>，UI 侧零悬挂引用。最小间隔（连接 2s/适配器 10s）+force 语义与
   tooltip 文案一致。V20-P1-1 链速 -1→0 归一化已落地（AdapterInfo.cpp:189-190）并有
   netadapter_speed_format 钉住 0→"—"。fmt 瑕疵：`IsPhyscialAdapter` 拼写（physcial）、
   AdapterInfo.cpp:97-100 混排烂注释（见 P2 附带，观感级）。
7. **WindowsBuildText 句柄**：RegOpenKeyExW 后 RegCloseKey 在两条返回路径都执行（AboutUi.h:87/104）；
   RtlGetVersion 用 GetModuleHandleW（ntdll 常驻，不需 FreeLibrary）+ GetProcAddress 动态绑定，
   RTL_OSVERSIONINFOSize 正确；KEY_WOW64_64KEY 固定 64 位视图正确；三级回退（注册表→Rtl→"不可用"）
   与 ui_about_test 的注入用例（含 rtlBuild=0 不伪造）一致。每帧调用的开销见 P2-2。
8. **关于页版本宏一致性**：IMGUI_VERSION/IMPLOT_VERSION 直接取自 vendored 头
   （third_party/imgui/imgui.h = 1.92.9/19290，third_party/implot/implot.h = 1.0），与实际链接同源编译，
   构造性一致，无第二份版本信息可漂移。
9. **CSV 记录 × 时间窗切换**：共用 AppendHistory 单一摄取点（Pages.cpp:635），切窗只写 perfWindowSec
   不动 CSV；V15 的写失败自动停+诚实 toast 在位（:635-639）。**告警阈值线 × DragLine**：DragLineY 仅
   CPU 图（alertOn 开时）一处，id 0 无复用；与底部 InputInt 双向写同一 cfg 键、双方都钳 1..100（写端），
   无冲突。
10. **状态栏两段式**：左段 segFits 逐段降级（兼容模式锚点必显、p95/队列/进程数/CSV/最近提示依次藏）、
    右段 LayoutHeaderRight 装箱 [热键(2)→徽标(1)→帧耗时(0)]、帧耗时永不藏——本轮未破坏。
11. **前轮遗留修复核验**：LR_SHARED 图标（main.cpp:397-404）、WM_ENDSESSION 保存（main.cpp:379-383）、
    ShutdownAboutUi 先于 renderer.Shutdown（main.cpp:600-603）均在位。
12. **实跑汇总**：stm_selftest.exe **102/0 ×2 轮**（exit 0×2，含 chart×3、netadapter×5、windows_build×2、
    theme/colw/about 用例）；`--smoke 150` exit 0（含离屏 DrawAppearancePanel 渲染路径）；
    `--autotest dialogclick/about/wallpaper/kill/tree/startup` **6/6 PASS**（exit 0×6，autotest_result.log
    尾行逐一核对）；真实实例 60s 双采样 **WS 124.9→125.4MB、Private 111.5→111.9MB、句柄 696→696**
    ——无异常增长（基线 125–140MB 区间内；壁纸未启用、会话注入实验后已还原 session.json）。
    窄窗注入实验（P1-1）共 4 次 --autotest about 运行，session.json 已从备份还原、无残留实例。

## 结论

本轮 A1 统一风格与 Phase A/A2 的代码质量良好：三条历史缺陷零复发、四个前轮遗留项全部落地、
新纯函数面（RingView/AdapterUi/WindowsBuildText）测试钉得扎实。发布阻塞项仅 **P1-1**：窄窗裁剪
使主题/关于全应用不可达，一行 WM_GETMINMAXINFO 即可修复；P2-1（"秒"标签）触及项目"诚实数据"
口径，建议同批修。壁纸同步加载实测 60–150ms，**维持同步是正确取舍**，只需清掉两处体验尾巴。

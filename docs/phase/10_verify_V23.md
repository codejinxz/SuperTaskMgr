# 独立复核报告 V23（2026-09-19）—— V21-P0-1 PlotRing 线性化 / V22-P1-1 最小窗宽 / V21-V22 P2 批次

- 复核人：V23（新视角独立 subagent；未用 git；除本报告外未修改任何仓库文件；仅运行 build/Release 产物）
- 方法：通读 Pages.cpp / PerfChart.h / vendored ImPlot(implot_items.cpp, implot.cpp) / main.cpp /
  Win32Window.cpp / ImGuiLayer(.cpp/.h) + imgui_impl_win32 backend / AboutUi.* / AdapterInfo.cpp /
  ui_chart_test.cpp + AutotestDialog.cpp 消费端核对 + 全量产物实跑
- 产物基线：build/Release（SuperTaskMgr.exe 09:49:36 / stm_selftest.exe 09:48:42）晚于全部源码最新
  mtime（Pages.cpp 09:22）——**产物即含修复的新鲜构建，未重建**。

## 统计

- 修复复核判定：V21-P0-1 线性化本体 **PASS**、xscale 轴配套 **FAIL（新 P1）**；V22-P1-1 **PASS**；
  P2 批次 4 项 **全 PASS**
- 新发现问题：**P0×0，P1×1，P2×1**，观察项×2
- 实跑：selftest **103/0 ×2 轮**（exit 0×2，collect_tick_latency **PASS** 无环境性失败）；
  `--smoke 150` exit 0；`--autotest kill/tree/startup/dialogclick/about/wallpaper` **6/6 exit 0+PASS**
  （autotest_result.log 尾 6 条逐一核对；库内 36 条历史 FAIL 全部为 V22 轮窄窗注入实验遗留，非本轮）

---

## 一、P0 复核（V21-P0-1 PlotRing × ImPlot Offset 语义）

### ① CopyRingTail 环绕拼接数学 —— PASS

- 实现：`PerfChart.h:156-166` = MakeRingView(:119-128) + RingViewChunks(:132-150) + 两段 memcpy。
- 具体例子推演（与 selftest 用例同参数：620 次 Push 值 0..619，满环 head=20，window=60）：
  - 存储态 v = [600..619, 20..599]，Offset()=head=20，Count()=600；
  - MakeRingView：count=min(600,60)=60，start=n-count=600-60=540，
    **offset=(20+540)%600=560**（v[560]=560，即窗内最老样本值 560 ✓）；
  - RingViewChunks：room=600-560=40 < 60 → a=v+560 长度 40（值 560..599）、b=v+0 长度 20
    （值 600..619），na+nb=60 ✓；
  - CopyRingTail：out=[560..619] 恰为逻辑尾窗，与 ui_chart_test.cpp:290-296 逐点断言一致
    （chart_plot_ring_linearized **PASS**）；window=600 满窗 out[0]=20/out[599]=619 ✓。
  - 预热期 n<cap：Offset()=0、start=0、offset=0，单段直拷 ✓；window≤0/空环 → 返回 0，PlotRing
    早退不画 ✓；outCap=600 ≥ count=min(n,window)≤600 恒成立，防御分支不可达 ✓。
- 结论：拼接数学正确，稳态/预热/满窗/空环四态皆对，并有新回归用例钉死。

### ② `static thread_local std::vector<float> buf(kHistCap)` 多线程风险 —— PASS

- 全部调用点（grep 全仓）：Pages.cpp:2005/2006（CPU 总量+每核）、2022-2023（内存）、2036-2037
  （磁盘）、2050-2051（网络）、2072（每核）、2089（硬故障）、2106（上下文切换）——**全部在
  PerfPage::Draw 静态成员函数体内，仅 UI 线程帧循环触达**；CopyRingTail 另一消费方是 selftest
  （独立进程单线程）。thread_local 每线程一份 + C++11 动态初始化线程安全，单 UI 线程下等价于
  函数级 static，无竞争。
- 关键的"同帧多次 PlotRing 共用一块 buf 是否互相踩"：**不会**。vendored
  `implot_items.cpp:1905-1976` PlotLineEx 在 `PlotLine`(:1980-1983) 调用**内部同步**完成
  RenderPrimitives（GetterLineStrip 逐点读 buf 并写入 draw list）+ EndItem，数据消费在本次
  PlotRing 返回前结束，下一次 PlotRing 覆盖 buf 无影响；不经过 EndPlot 延迟读取。
- 附带核对：PlotLine 以 `IndexerLin(xscale,x0)` 计算 x=i*xscale+x0（:1981），spec.Offset=0 时
  IndexerIdx 直读 values[idx] —— 与 PlotRing 的 `PlotLine(label, buf.data(), n, tickSec, 0.0)`
  调用匹配 ✓。

### ③ xscale=tickSec 后 X 轴范围失配 —— **FAIL（新发现 P1-1）**

- 现状：PlotRing 的 x 坐标 = i×tickSec（真实秒），但 7 个图的
  `SetupAxisLimits(ImAxis_X1, 0.0, window-1, ImPlotCond_Always)`（Pages.cpp:1988/2018/2032/
  2046/2067/2085/2102）**未乘 tickSec**，且 `window = windowSec`（:1919）按 tick 计未换算。
- vendored `implot.cpp:2201-2203`：`cond == ImPlotCond_Always` 时每帧强制
  `axis.SetRange(0, window-1)`——轴范围不看数据；界外点被 plot clip rect 裁掉。
- 推演（刷新间隔滑条允许 500–5000ms，Pages.cpp:2547；tickSec=intervalMs/1000，:1895）：
  - intervalMs=500（tickSec=0.5）、120 秒窗：n=120 tick，x∈[0, 59.5]，轴 [0,119] →
    **数据只占左半幅，右半空白**；「120 秒」窗实际只覆盖 59.5s 墙钟（窗口按 tick 数未换算）。
  - intervalMs=5000（tickSec=5.0）、120 秒窗：x∈[0, 595]，轴 [0,119] → **只有最老 24 个样本
    可见，最新 ~96 tick（约 8 分钟）被裁出图外**——用户看到的是"没有最近数据"。
  - 默认 1000ms（tickSec=1）完全正确，smoke/autotest 默认配置跑不出此问题。
- 判定：V22-P2-1 的修复只做了"数据坐标换算"一半，轴上限与窗口 tick 数两处未同步。数据本身
  不失真（线性化部分 PASS、悬停读数/CSV 不走该轴），默认配置不受影响，但非默认间隔下核心
  功能可视性破坏且「时间窗 N 秒」文案仍不诚实 —— 判 **新 P1**（不判 P0：无错误数据渲染，
  仅默认外配置受影响）。
- 修法（一行级×2）：`SetupAxisLimits(ImAxis_X1, 0.0, (window-1)*tickSec_, ImPlotCond_Always)`
  ×7 处；若要「N 秒」窗语义完整，再把传给 PlotRing/CopyRingTail 的 window 由秒换算为
  tick 数 `clamp((int)round(windowSec/tickSec), 1, kHistCap)`。建议补一条 tickSec≠1 的
  selftest/autotest 变体防回归。

## 二、P1 复核（V22-P1-1 WM_GETMINMAXINFO 760px）—— PASS

- **handled=true 语义**：`main.cpp:348-354` 仅改 `mmi->ptMinTrackSize.x/y`（760/480）即
  `*handled=true; return 0;`；`Win32Window.cpp:18-21/41` handled=true 时跳过 DefWindowProc。
  系统在**发送 WM_GETMINMAXINFO 前已把 MINMAXINFO 预填为默认值**（ptMaxSize=工作区、
  ptMaxPosition、ptMaxTrackSize=虚拟屏全宽），DefWindowProc 的处理正是重填同一批默认值 ——
  跳过它不影响最大化尺寸/位置/最大跟踪尺寸，**最大化语义无损**。标准写法。
- **760px 充足性**：V22 实测非提权全流工具条 ≈700px（760 窗宽时「关于」右缘 707<738 可点）。
  高 DPI 不构成回退：本应用 Per-Monitor v2（Win32Window.cpp:11）但 **UI 不随 DPI 缩放**——
  字体固定 16px 一次加载（ImGuiLayer.h:12 默认参、ImGuiLayer.cpp:41），imgui_impl_win32 后端
  未挂 Platform_GetWindowDpiScale、未 ScaleAllSizes，io.DisplaySize 为物理像素 → 工具条像素
  宽度与 DPI 无关，760 物理px 在任意缩放档下均够。余量约 60px（默认字体 msyh 路径下），偏紧
  但 V22 注入实证已过。提权态按钮更少，更宽松。
- MoveWindow/程序化设窗不受 ptMinTrackSize 约束 → headless autotest 的会话矩形注入不受影响
  （本轮 6/6 PASS 实证）。

## 三、P2 批次复核 —— 4/4 PASS

1. **WindowsBuildText 函数级 static 缓存**：AboutUi.h:129-139 `static const std::wstring cached
   = [...]`——magic static 单次初始化、UI 线程独享；注册表/Rtl 读取器只跑一次，AboutUi.cpp:169
   每帧调用变纯返回。注入式测试走 WindowsBuildTextWith（:56-70）不受影响，ui_about_test:234
   的真机探测每进程一次无缓存语义问题。V22-P2-2 的 23µs/帧注册表读取消除。**PASS**
2. **主题单选重复点击守卫**：Pages.cpp:2486-2493 `if (ImGui::RadioButton(...)) { if (m.mode !=
   cur) { SetInt + Apply + toast; } }` —— 点击已选项不再重复 Apply/弹「主题已切换」。V21-P2-3
   落地。**PASS**
3. **AdapterInfo 链速 UINT64_MAX 归一化**：AdapterInfo.cpp:189-190
   `rawSpeed=max(Tx,Rx); linkSpeedMbps = rawSpeed==UINT64_MAX ? 0 : rawSpeed/1e6` —— GAA
   "未知"(-1) 归一为 0，配合 netadapter_speed_format 钉住 0→"—"；单方向 -1 时 max 取到有效值
   亦正确。V20-P1-1 保持落地。**PASS**
4. **AboutUi closeValid 每帧重置**：AboutUi.cpp:185-188 模态体内每帧先 `closeValid/closeHovered
   = false` 再由 IsItemVisible/IsItemHovered 重发布；消费者 AutotestDialog.cpp:359/317 均 gated
   于 `modalOpen`，模态关闭早退路径（:135-147）不残留可被误读的状态。**PASS**
   （注：模态未开分支仍不清 close* 字段，V21-P2-7 的"对称清理"未做——无消费路径，维持观察项。）

## 四、实跑汇总

| 项目 | 结果 |
| --- | --- |
| stm_selftest.exe 第 1 轮 | **103 通过 / 0 失败**，exit 0（collect_tick_latency PASS） |
| stm_selftest.exe 第 2 轮 | **103 通过 / 0 失败**，exit 0 |
| chart_* 四条 | window_clamp / ring_view / readout_text / **plot_ring_linearized(新)** 全 PASS |
| SuperTaskMgr.exe --smoke 150 | exit 0 |
| --autotest kill / tree / startup / dialogclick / about / wallpaper | 6/6 exit 0，autotest_result.log 尾 6 条全 PASS（about：模态≥4 帧+真实点击「关闭」闭环；wallpaper：帧顶点 7134/7150、壁纸绘制命令=1） |

## 新发现问题

### P1-1（新）X 轴上限/窗口 tick 数未随 tickSec 换算：intervalMs≠1000 时图表被裁剪或半空，「N 秒」窗语义仍失真

- 见"一、③"。文件:行：Pages.cpp:1988/2018/2032/2046/2067/2085/2102（轴上限）、:1919+1874
  （window 按秒传给按 tick 消费的 CopyRingTail）、:1895（tickSec 来源）。
- 触发条件：刷新间隔滑条 500ms 或 5000ms 档（:2547）；默认 1000ms 不受影响。
- 后果：500ms 档图占左半幅且实际只显示一半时长的数据；5000ms 档最新数据大部分被裁出可视区
  ——用户在最需要"实时"的低频采集档位反而看不到最新曲线。与 V22-P2-1 的"诚实数据"口径仍有
  残差距。

### P2-1（新）ui_chart_test.cpp 回归用例注释推演算错（代码正确，注释误导）

- `src/selftest/ui_chart_test.cpp:288-289`：「逻辑首下标 = 620-60 = 560，物理 = (20+560)%600 =
  580」——实际实现用 Count()=600：start=600-60=540、物理=(20+540)%600=**560**（v[560]=560 恰为
  窗首值）。注释把"值 560"误当"逻辑下标 560"代入，得 580，与断言无涉但会误导后续维护者对
  offset 公式的理解。断言本身正确（PASS）。建议改正注释为 start=540/offset=560。

### 观察项（不判级）

1. AboutUi.cpp:135-147 模态未开的早退分支仍不复位 closeValid/closeHovered（消费端 gated，
   无实害；V21-P2-7 对称清理顺延）。
2. ptMinTrackSize.x=760 是物理像素常量，隐含依赖"UI 不随 DPI 缩放"这一现状；未来若引入
   DPI 字体缩放，该值需按缩放系数重推（建议届时改为 `760 * dpiScale`）。

## 结论

V21-P0-1 的线性化修复（CopyRingTail + Offset=0 + thread_local 缓冲）数学正确、调用面收敛于
UI 线程、与 vendored ImPlot 的即时渲染语义吻合，并有新回归用例钉死 —— **本体 PASS**；但
xscale 收尾不完整，X 轴上限与窗口 tick 数未同步换算，**遗留新 P1-1**（非默认刷新间隔下图表
可视性破坏）。V22-P1-1 最小窗宽 PASS（handled 语义安全、760px 在任意 DPI 下够用）。P2 批次
四项全部落地 PASS。产物实跑 103/0×2 + smoke + autotest 6/6 全绿。发布建议：修掉 P1-1（约
8 行 + 一个变体用例）后即可闭环。

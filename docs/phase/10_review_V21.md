# 终审评审 V21（2026-09-18）

对象：工具条统一（Pages.cpp A1）/ 关于页运行环境修复（AboutUi）/ 网络页适配器区
（Pages3 + NetAdapterUi）/ 图表 Phase A（PerfChart + PerfPage）。
方法：全量通读相关源码 + ImPlot vendored 实现核对 + 重新构建 Release + 实跑。
git 未使用；除本报告外未修改任何仓库文件。

## 实跑结果

| 项目 | 结果 |
| --- | --- |
| 重编译（scripts\build.bat Release） | 成功，无新告警输出 |
| stm_selftest.exe | **102 通过 / 0 失败**（含 collect_tick_latency PASS，无环境性失败） |
| SuperTaskMgr.exe --smoke 150 | exit 0 |
| --autotest kill / tree / startup / dialogclick / about / wallpaper | 六条全 exit 0，autotest_result.log 全 PASS |

（注：首次 --smoke exit 1 为单实例互斥体被一个并发残留的 `--autotest about` 进程占用所致，
main.cpp:288 `si.TryAcquire(3000)` headless 返回 1 —— 属环境竞争，非缺陷；互斥体释放后重跑通过。）

---

## P0（必须修复）

### P0-1 PlotRing 的 spec.Offset 消费方式与 ImPlot 语义不符：稳态下图表画的是旧数据，不是尾窗

- 位置：`src/app/ui/Pages.cpp:1869-1876`（PlotRing）；全部调用点 1998-1999、2015-2016、
  2029-2030、2043-2044、2065、2082、2098；根因假设写在 `src/app/ui3/PerfChart.h:118-127`
  （MakeRingView 注释「ImPlot 经 spec.Offset 逐块消费」）。
- 证据（vendored ImPlot 实现）：
  - `third_party/implot/implot_items.cpp:501-509`：`IndexData` 取值
    `data[(offset + idx) % count]` —— **模的是绘制点数 count，不是环容量 kHistCap**；
  - `implot_items.cpp:517`：`Offset(ImPosMod(offset, count))` —— offset 先被归一化到
    `[0, count)`；`implot_items.cpp:1981/2030`：getter 以 `count`（PlotLine 的点数）构建。
  - 而 `MakeRingView`（PerfChart.h:118-127）返回 `count = min(n, window)`、
    `offset = (head + n - count) % 600`。两者仅在 `count == 600`（满环+600s 全窗）或
    `offset == 0`（n ≤ window 的预热期，走 `data[idx]` 快路径）时正确。
- 用 selftest 自己的用例数字复算（ui_chart_test.cpp:102-120：满环 head=50、60s 窗）：
  `view.count=60, view.offset=590`。ImPlot 归一化 `ImPosMod(590,60)=50`，第 i 点读
  `data[(50+i)%60]` = data[50..59,0..9]；正确应为 `data[(590+i)%600]` = data[590..599,0..49]。
  **60 点中 50 点是错的**：x=0 画的是 10 分钟前最老的样本，x=59（"现在"）画的是 590 秒前
  的样本。触发条件即常态：默认 120s 窗在运行 2 分钟后（60/300s 分别为 1/5 分钟后）开始
  整段画错；预热期 n>window 时同样错。CPU/每核/内存/磁盘/网络/硬故障/上下文切换全部
  经 PlotRing 受影响。Phase A 的核心卖点（600 环 + 60/120/300/600 尾窗）在稳态下不可信。
- 为什么 selftest 没抓住：ui_chart_test.cpp:95-150 用 **mod-600** 的手工拼接验证纯视图
  （正确），ImPlot 路径只断言 `view.data == r.v.data()`（零拷贝指针），从未复算 ImPlot
  的 mod-count 索引 —— 纯函数正确 + 集成失配的典型盲区。
- 修法（推荐）：PlotRing 不再把 offset 交给 ImPlot。线程局部暂存缓冲：
  `thread_local std::vector<float> tmp;`（≤600 float = 2.4KB），用 `RingViewChunks`
  两段 memcpy 拼成线性尾窗后 `PlotLine(label, tmp.data(), view.count, 1.0, 0.0, spec)`
  （spec.Offset 保持 0）。备选：两段 PlotLine（第一段 xstart=0 长度 na，第二段 xstart=na
  读 data[0..nb)）——但会在图例产生双条目，需配合 NoLegend，较繁琐。
- 回归建议：在 ui_chart_test.cpp 增加「ImPlot 消费模拟」用例：按
  `data[ImPosMod(offset,count)]` 复算每个绘制点并断言 == 逻辑序列（当前实现下该用例必红，
  修复后转绿），防止再次回归。

---

## P1（无）

本轮未发现 P1 级缺陷。候选均被证伪：
- 外观/关于模态生命周期（每帧 Begin、Esc 清理、请求标志消费）逐路径核对无僵尸模态路径；
- AsyncFetch jobs 生命周期（State shared_ptr + app 捕获 + Submit==0 回滚）正确；
- WindowsBuildText 组合逻辑/动态绑定/缓冲正确（详见下）。

---

## P2（建议，不阻塞）

1. **wallpaperBusy「加载中…」永远不可见**（Pages.cpp:2364-2367、2381-2382）。
   `Ui().wallpaperBusy = true → WallpaperLoad（阻塞）→ = false` 在同帧内完成，期间无帧
   渲染，状态行的「加载中…」是死文案。建议删除该标志或在注释注明仅为占位；真实反馈
   已由完成后 toast 承担，无用户可见危害。
2. **关于模态打开期间每帧 2 次 RegOpen + 2 次 RegGet**（AboutUi.cpp:169 → AboutUi.h:74-108）。
   注册表调用有系统缓存、微秒级，无实测卡顿；但模态常开时是纯粹的每帧重复 I/O。建议
   打开时算一次缓存（模态无「运行中版本会变」的语义）。
3. **点击已选中的主题单选钮会重复 Apply + 弹「主题已切换」toast**（Pages.cpp:2479-2484）。
   ImGui::RadioButton 对已选中项点击仍返回 true。建议 `if (m.mode != cur && RadioButton...)`
   抑制无效反馈。
4. **外观模态缺单帧化哨兵与自动化覆盖**（Pages.cpp:2440-2506）。##confirm/##about 都有
   framesOpen/modalFrames 哨兵并被 --autotest 覆盖；##appearance 没有 sentinel，六条
   autotest 与 --smoke 均不驱动模态本体（smoke 只离屏画 DrawAppearancePanel 体）。
   建议补 about 式点击驱动或至少补 sentinel。
5. **IF_TYPE_PPP 注释值错误**（AdapterInfo.h:36-37 注释「12 PPP」、AdapterInfo.cpp:121
   同注）。SDK ipifcons.h:66 `IF_TYPE_PPP = 23`。代码用枚举常量，行为正确，仅注释误导。
6. **适配器排序用 std::sort 非稳定**（Pages3.cpp:320-321）。AdapterSortKey 是合法严格
   弱序；仅在「up+物理+友好名全相等」的等价类内顺序未指定（理论闪烁）。Windows 友好名
   通常唯一，影响极小；求稳可换 stable_sort。
7. **DrawAboutUi 关闭按钮矩形在模态关闭后不清**（AboutUi.cpp:185-195 只在模态体内重置）。
   modalOpen=false 时驱动不消费 close* 字段，无实际误读路径；对称起见可在模态未开分支
   一并复位。

---

## 已查无问题清单（逐项核对点）

- 工具条（Pages.cpp:2508-2595）：[暂停/继续采集][间隔滑条][提权][清理待机缓存][内存加速…]
  [主题…][关于] 一级流式排列，无绝对偏移/右缘布局；间隔滑条 500-5000 写 cfg + SetInterval；
  提权按钮走 jobs（UAC 期间 UI 不冻结）；「内存加速」工具条入口与性能页入口同模态。
- 模态模式：##confirm/##about/##appearance 三者均为「请求长期有效 + 每帧 BeginPopupModal」，
  OpenPopup 只发一次；Esc 路径均复位打开态（Pages.cpp:345、2457-2459、AboutUi.cpp:135-139），
  无单帧化回归、无隐形僵尸模态；##about 有 modalFrames 哨兵且 --autotest about 消费
  （AutotestDialog.cpp:313-317，实测 PASS）。
- 壁纸选图（Pages.cpp:2342-2375）：GetOpenFileNameW 自带模态消息泵，UI 线程不出现
  「未响应」（模态对话框 ≠ 卡死）；OFN_NOCHANGEDIR 防串工作目录；扩展名二次校验诚实拒绝；
  WallpaperLoad 有 512MB 文件闸 + 4096×4096 像素闸（Wallpaper.cpp:52-54），同步解码+
  上传几十 ms 量级、用户主动触发、失败安全（旧壁纸不受损、存档副本后写），评估为可接受；
  R-Fix Bug2 链路（背景列表 + approot 透明 + mask）自洽。
- 遮罩实时生效链路：滑条写 cfg（内存）→ main.cpp:515-516 每帧 NewFrame 后
  `ClampMask(cfg.GetDouble("wallpaperMask"))` → WallpaperDrawBackground 画遮罩矩形；
  NaN/越界在 ClampMask 归 0/0.85。链路成立。
- 主题三态 × System 监听共存：radio 写 cfg + Theme::Apply（Pages.cpp:2479-2484）；
  main.cpp:366-372 仅当 cfg==System 时响应 WM_SETTINGCHANGE/"ImmersiveColorSet"
  （lstrcmpiW 大小写不敏感），uiThemeReady 防 ImGui 初始化前 Apply。两者不冲突。
- 「恢复默认列宽」：SoftDeleteColWidthKeys（内存置 ""）+ StripColWidthKeysFromFile(full)
  （文件剔除）+ colWidthResetGen 换代表格 id（Pages.cpp:2491-2500、709-712、1025-1027）；
  退出时 Save 后 onlyEmptyValues 再剔除（main.cpp:583-586），无 "" 残留；键清单与
  PersistWidths 一一对应并由 colw_strip_file 钉死（PASS）。
- AboutUi：WindowsBuildTextWith 分支（reg+UBR / reg only / rtl 兜底 / build=0 不伪造 /
  全败「不可用」）正确；真实读取器 KEY_WOW64_64KEY 固定 64 位视图、RegGetValueW 保证
  终止符、缓冲 32 wchar 充裕；RtlGetVersion 动态绑定（ntdll 恒加载、不受 manifest 垫片
  影响）、RTL_OSVERSIONINFOW.dwOSVersionInfoSize 正确。UptimeText 起点移到命名空间作用域
  （AboutUi.cpp:28），CRT 动态初始化阶段取 steady_clock 安全，不再「从首次打开起计」。
  windows_build_text_ok 断言质量高：真机探测注册表可读性做门控（锁注册表环境不误报）、
  真机 rtl>=9201 自证、5 组注入分支全覆盖（小缺口：reg 失败优先于 rtl 的优先序未被独立
  区分，因伪读取器两路返回同值，不影响正确性结论）。Logo 资源管线失败静默、
  ShutdownAboutUi 先于设备销毁且可复活。
- 适配器区：AsyncFetch 生命周期（busy 门 + force 旁路 + 10s 最小间隔 + 提交失败回滚 +
  State/app shared_ptr 活过拆除）正确；EnumAdaptersNet 在 jobs 线程执行，UI 不阻塞；
  回环隐藏且不计入统计（Pages3.cpp:269、309）；已连接物理卡直显、其余折叠「更多适配器」
  ——误杀（IF_TYPE_OTHER 物理卡）只降级到折叠区不被隐藏，漏杀（隧道/PPP）进折叠区，
  可接受；速度 0→"—"（且数据层 V20-P1-1 把 GAA 全 1 归一化为 0，无 1.8e13 Mbps 假速度）；
  prefixLen==0 不画假 "/0"；IPv6 >39 字符截断加 "…" + 原值 tooltip；复制 IP 首选 IPv4
  回退首条地址、空则禁用、按钮闪「已复制」1s（copyFlash_ 按 ifIndex，有界）；「刷新适配器」
  与「刷新」ID 已区分。AdapterSortKey 严格弱序、up>物理>名称，selftest 钉死。
- 图表 Phase A 其余点：Ring::Push/Offset/LogicalAt 环绕正确（mod 600，越界 NaN 诚实缺省）；
  MakeRingView/RingViewChunks 拼接正确（na+nb==count 恒成立，selftest 逐点核对）；
  时间窗切换只改视图不清历史（摄取唯一入口 AppendHistory 与窗口无关）；AutoFit
  （ImPlotAxisFlags_AutoFit 每 tick 跟随）修复字节类削顶，CPU/每核恒定 0-100 保持扫视
  可比；X 轴恒定 0..window-1；DragLineY 阈值双向同步：cfg 只在取整变化时写（内存键值，
  非文件 I/O，拖拽中无 I/O 风暴），1..100 双向钳制，松手回弹语义正确；NaN 读数三处
  守卫（FmtBytesAxis、BytesAt、FormatPercent/Number 内部）齐全；hasData 诚实空态
  （硬故障/上下文切换不可用计数器）保留；CSV 记录失败自动停 + toast 路径。
- 每帧摄取一致性：AppendHistory 所有系统级 Ring 同 tick 同步 Push，PerfReadoutText 用
  cpuTotal.Count() 代表全体的假设成立；核心数变更重建历史（assign）符合注释。
- 实跑通道：--autotest 六条驱动（kill/tree/startup 走 RunAutotest，
  dialogclick/about/wallpaper 走合成点击驱动并在 Finish 置 wantExit，main.cpp:530-581 +
  AutotestDialog.cpp:71/82/202/210/454/463），无滞留退出路径；本轮全部 PASS。

## 结论

**P0×1（PlotRing × ImPlot Offset 语义失配，稳态图表数据错误），P1×0，P2×7。**
P0-1 修复点集中在 Pages.cpp::PlotRing 单函数（约 10 行改动）+ 一个回归用例，建议
修复后重跑 selftest + --smoke + --autotest about/wallpaper 即可闭环。

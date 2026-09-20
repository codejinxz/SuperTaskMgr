# 终审评审报告 V30（2026-09-20）

评审人：V30（独立终审，与其他评审者互不知情）。禁 git；未修改任何产品文件。
对象：全仓回归 + 本轮新增面（StatusLayout/PageLayout 纯函数、colOrder 持久化、netcol_* 列宽持久化、layoutScale 运行时缩放、EllipsizeTextUtf8、P1⑤ 传感器收缩）。

## 测试与采样执行记录

| 项目 | 结果 |
| --- | --- |
| stm_selftest.exe 双轮 | 两轮均 **159 通过 / 0 失败**（v28 为 146，本轮新增 13 项 P1 契约测试） |
| SuperTaskMgr.exe --smoke 150 | exit=0，无输出/无告警（与 v28 同形态） |
| --autotest 六条 | about / dialogclick / kill / startup / tree / wallpaper 全部 exit=0，autotest_result.log 末六条全 PASS（wallpaper 帧顶点≈7.1k、壁纸绑定绘制命令=1） |
| 内存 60s 双采样（真实实例 120s 三点） | 128,228 K → 127,824 K → 126,872 K（≈127–130MB，基线带内，无增长）；句柄 685；运行期日志 0 ERROR |
| 二进制新鲜度 | build/Release 产物（Sep 20 18:35）晚于全部 src 源文件，实测即当前代码 |

---

## P0（无）

未发现崩溃、越界、数据损坏、死锁类缺陷。

## P1

### P1-1 「重置布局」不重置 netcol_* 的内存缓存：7 张网络/GPU 表本次会话内不生效，且拖动会把旧宽写回

- 证据：
  - `src/app/ui3/Pages3.cpp:105-149`：`NetColCacheFor` 是 `static std::map<std::string, NetColCache>`，`NetColCache::loaded` 置位后整个进程生命周期不再读 cfg；无任何失效路径（全仓 grep `loaded = false`/`clear` 仅命中 Wallpaper 与声明处）。
  - `src/app/ui/Pages.cpp:2713-2747`：`GpuColCaches()` 同构（gpuadapters/gpuprocs）。
  - `src/app/ui/Pages.cpp:3241-3249`：`ResetLayout` 只做 `SoftDeleteLayoutKeys`（cfg 内存置 ""）+ `StripLayoutKeysFromFile`（文件剔除）+ `SetLayoutScale(1)` + `++colWidthResetGen`；**未触碰两个缓存 map**。
  - 后果：点击「重置布局」后进程表立即回默认宽（换代机制），但 netmon_events/netmon_top/netmon_dns/pcap_pkts/netconn/gpuadapters/gpuprocs 七表在本次会话继续使用旧列宽，与 toast「已重置布局（清除 N 项布局设置）」（Pages.cpp:3246-3248）承诺不符；此后用户拖动任一列，`NetColSaveWidths` 以旧宽为「变化检测基线」把该列旧值后的新值立即写回 cfg，形成单列"复活"。
- 修法：把 `colWidthResetGen` 升格为布局重置代际（或在 UiState 增加 `layoutResetGen`），`NetColWidths/GpuColWidths` 记录加载时的 gen，gen 变化时 `loaded=false` 重读 cfg（或直接 `caches.clear()`）；两处缓存实现同步改。

## P2

### P2-1 EllipsizeTextUtf8 循环内每码点一次堆分配 + O(N²) 拷贝
- 证据：`src/app/ui3/StatusLayout.h:152` — `measure(std::string(u8, keep + step).c_str())` 每个码点构造一次 `std::string`；降级模式下状态栏每帧调用（anchorBuf 512B，原因文本通常 ≤60 字节 → 每帧约 60 次短命分配）。
- 影响：微小（远低于帧预算），但位于每帧路径。修法：`measure` 改为接受 `(const char*, size_t)`，或先用 `strlen` 上界剪枝再逐码点。

### P2-2 退出路径只剔 colW_* 软删残留，netcol_*/layoutScale/colOrder/perfZoom 的 "" 行残留 config.json
- 证据：`src/app/main.cpp:596-598` 退出仅 `StripColWidthKeysFromFile(onlyEmptyValues=true)`；「重置布局」后这些键被软删为 ""，退出 `cfg.Save`（main.cpp:595）把约 38 行 `"键": ""`（netcol 35 + 3 精确键）写回文件，直到下次「重置布局」（onlyEmptyValues=false 全剔）才消失。功能无害（`Cfg::GetDouble/GetString` 对空值回默认，core/Cfg.cpp:160-181），属文件卫生问题。
- 修法：退出改调 `StripCfgKeysFromFile(path, {"colW_","netcol_"}, {"layoutScale","colOrder","perfZoom"}, true)`。

### P2-3 「一键优化布局」跨屏语义：分辨率因子恒取主视口（主显示器），且无 WM_DISPLAYCHANGE 跟随
- 证据：`src/app/ui/Pages.cpp:3226-3239` — `GetMainViewport()->WorkSize.y`（ImGui 1.92.9 本应用未启用多视口，ImGuiLayer.cpp:29 仅 NavEnableKeyboard，主视口 = 主显示器）+ `GetDpiForWindow`（窗口所在屏）。窗口在副屏时分辨率因子可能取错屏；`max(res, dpiF)` 策略下混合 DPI 场景通常由 DPI 因子兜底，但"主屏 4K + 副屏 1080p@100%"时窗口在副屏会得到 1.30 过大缩放。显示器变更后也不会自动重算（一键按钮可重按）。
- 修法：DPI 取窗口屏、分辨率因子改用 `MonitorFromWindow` 对应显示器工作区；至少在 tooltip/toast 注明「移动显示器后可重按一次」。

### P2-4 会话恢复 `MoveWindow` 无工作区钳制：显示器拔出/降分辨率后窗口可整体落屏外
- 证据：`src/app/main.cpp:481-483` 直接 `MoveWindow(session.winX/winY/...)`，全仓无 `MonitorFromWindow`/`SPI_GETWORKAREA` 钳制（V11-P2-5 只防了最小化 -32000 坐标，未防配置变化后的合法但屏外矩形）。恢复后标题栏不可达且托盘无「移回主屏」。属前轮均未覆盖的漏网项（非本轮引入）。
- 修法：恢复后用 `MonitorFromWindow(..., MONITOR_DEFAULTTONEAREST)` 工作区把矩形钳回可见范围（保留尺寸、只平移）。

### P2-5 colOrder 相关小项：`colOrderAppliedGen_` 只写不读；`TableSetColumnDisplayOrder` 的防护是结构性的
- 证据：`src/app/ui/Pages.cpp:1976,1216` 声明并赋值但无读者（死字段）。`ApplyPersistedColumnOrderOnce`（Pages.cpp:1211-1223）对 `slot/d` 均做 `[0,kProcColSlots)` 显式钳制，且仅在 `BeginTable` 成功 + 13 次 `TableSetupColumn` 之后的本表作用域内调用（Pages.cpp:1118-1124），列数恒等，imgui_tables.cpp:731 的 `IM_ASSERT(…< ColumnsCount)` 即使 Release 失效也不会越界——当前安全；但若未来把该循环复用到别的表，需补 `table->ColumnsCount == kProcColSlots` 显式校验。
- 修法：删除死字段或消费之；可选加显式列数断言性防御。

---

## 已查无问题清单

1. **StatusLayout.h / PageLayout.h 纯函数无状态**：除 `LayoutScaleRef()` 有意的进程级单槽 static（UI 单线程；app 与 selftest 双入口共用同一份），其余函数全部仅依赖入参；`FormatSlotMs/FormatSlotCount` 对 NaN/负值/超上限钳制完备（StatusLayout.h:40-53）；`ui_status_slots_test` 15 项契约实测通过。
2. **EllipsizeTextUtf8 正确性**：outCap 0/1/4 边界、keep==0 只返回「…」、截断 4 字节序列收尾防御（keep+step>len）、非码点首字节 0x80-0xBF 按 ASCII 边界、快路径整串拷贝 NUL 安全——无越界、无未初始化输出（out[0] 恒先置 '\0'）。
3. **colOrder 全链路**：`ProcColumnOrderFromCfg` 拒绝个数/越界/重复/空字段/尾逗号（SortKey.h:183-212）；`TableSetColumnDisplayOrder` 应用点结构安全（见 P2-5）；恢复串与 `colOrderLastSaved_` 相等故恢复不触发回写；排序回调/列宽键/列顺序三者全部按槽位 UserID 映射，拖动重排后互不错位。
4. **进程表 重排+过滤+树形+排序 四叠加**：`RebuildRows`（Pages.cpp:981-1024）先过滤→按槽位排序→BuildTreeOrder DFS；colOrder 只改显示序不影响 ColumnUserID/SortSpecs 映射；树形模式禁表头排序并明示、回平铺 `sortReflected_=false` 重同步表头指示。
5. **netcol_ 读写配对与越界防御**：7 表（8+5+3+5+6+4+4=35 键）全部 `NetColWidths→TableSetupColumn→NetColSaveWidths` 成对出现在同一表作用域；persistMask 长度与列数一致、拉伸列跳过；`kNetColMaxCols=12` 在读/写两端双向钳制（Pages3.cpp:121,142）；liveCtx 为空（离屏 smoke）退默认且跳过写；GPU 侧同构（Pages.cpp:2721-2747）。
6. **键数量与写入频率**：config.json 新增键约 51 个（netcol 35 + colW 13 + layoutScale/colOrder/perfZoom 3），Cfg 为扁平键值表、运行期 SetX 只写内存、退出一次性 Save；colOrder/netcol 写入均为变化驱动 + ~1Hz 节流，layoutScale 仅点击时一次。
7. **layoutScale 每帧重算**：DrawShell（Pages.cpp:3643-3658）每帧仅 10 次浮点乘 + 5 次结构体写，无累计（基准常量同 Theme::ApplyLayout），scale=1 逐位恒等；全仓无其它对这 5 个 style 字段的运行期写（仅读），与 Theme::Apply 运行期切换无冲突；帧预算增量估算 <0.01ms，远低于 3ms 预算。字体不随缩放为既定设计（只缩放间距/定高/图表）。
8. **缩放版定高钳制**：`AdapterRegionHeightScaled` 对负/零/NaN 可用高退化为正小值；`SensorGroupsRegionHeightScaled` 在 scale>1 时钳回可用高度、scale=1 逐位保持原语义（含「极矮窗口接受父级滚动」的已声明降级）。
9. **传感器收缩 + 「详细模式」切换**：收缩只用上一帧内容高且仅当 contentH<region；内容超定时含滚动的实测值 ≥ region，无收缩-滚动振荡；详细/精简切换一帧收敛，区外元素 y 恒定（顶栏固定契约不破）。「传感器页白色块」根因修复在位。
10. **外观模态生命周期**：`ResetLayout/ApplyOneClickLayout` 在模态内执行安全——软删→文件剔除（格式异常返回 -1 且诚实提示）→缩放槽回 1→进程表换代；模态 AlwaysAutoResize 随样式自适应，Esc 复位逻辑（Pages.cpp:2972-2978）与兼容模态残留防护（V24 P2-1，Pages.cpp:3072-3080）均在位。
11. **内存加速三入口**：工具条（Pages.cpp:3309）、性能页 GPU 块（2578）、性能页内存块（2619）全部汇入同一 `RequestConfirmMemCleanup`→确认模态→`MakeMemCleanupJob` 单执行路径，无旁路。
12. **历史缺陷零复发抽验**：确认框点击（dialogclick autotest 真实管线 PASS）、壁纸渲染（wallpaper autotest 顶点>0 + 绑定命令=1 + Exit 0）、关于点击（about autotest 开≥4帧+关闭 PASS）、兼容模式说明模态（每帧渲染 + Esc 复位 + 迟到完成不弹 stale toast）、状态栏槽位防抖（常量样本槽宽 + AnchorEndX 不读光标 + LayoutFlowSlots 隐藏语义，实码在位）、页面顶栏固定（网络/传感器定高区 + 定高区内部渲染提示行）、进程表排序（UserID 映射 + 不可得恒排尾 + pid 破平，159 项含 ui_test/ui_p1 全过）——本轮 UserID/锚点/缩放改动叠加后各修复路径均完整保留。
13. **内存与稳定性**：真实实例 120s 三点采样 128.2→127.8→126.9MB 无增长；句柄 685、线程数正常；运行期与全部测试轮次日志 0 ERROR（selfcheck WARN 为 autotest 环境下预期自校验行为）。

## 结论

P0=0，P1=1（netcol 缓存不随「重置布局」失效），P2=5。P1 修复面小（一处代际失效机制覆盖两个缓存实现），不阻塞；建议下一维护轮优先处理 P1-1 与 P2-2（同一主题：布局键的会话内一致性）。

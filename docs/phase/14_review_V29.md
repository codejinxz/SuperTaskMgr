# 终审评审 V29（2026-09-20）——UI 交互变更（状态栏锚点 / 列拖动重排 / netcol 持久化 / 一键布局 / 传感器收缩）

评审人：V29（独立终审，未与其他评审者互通）。禁 git；除本报告外未修改任何文件。
实跑产物：`build\Release\stm_selftest.exe`、`build\Release\SuperTaskMgr.exe`（构建 18:35，晚于全部源码 18:22，非陈旧产物）。

## 实跑结果

| 项 | 结果 |
|---|---|
| stm_selftest.exe 第 1 轮 | 159 通过 / 0 失败 |
| stm_selftest.exe 第 2 轮 | 159 通过 / 0 失败 |
| SuperTaskMgr --smoke 150 | 退出码 0，日志干净收尾（采集/自检门/PawnIO/壁纸正常） |
| --autotest about | PASS（模态打开≥4帧并点击关闭） |
| --autotest dialogclick | PASS（真实管线点击生效） |
| --autotest kill | PASS |
| --autotest startup | PASS |
| --autotest tree | PASS（终止进程树 3 个） |
| --autotest wallpaper | PASS（壁纸绑定绘制命令=1） |

## P0（0 项）

无。未发现崩溃、数据损坏、越界写（在生产调用路径上）或功能全失效。

## P1（2 项）

**P1-1 「重置布局」不清 netcol 内存缓存：网络/GPU 7 表在本次会话内不回默认列宽**
- 位置：`src/app/ui3/Pages3.cpp:111-130`（`NetColCacheFor`/`NetColWidths`，`loaded` 一次性惰性加载）、`src/app/ui/Pages.cpp:2717-2747`（`GpuColCaches`/`GpuColWidths` 同模式）；对照 `src/app/ui/Pages.cpp:3241-3249`（`ResetLayout` 仅软删 cfg + `colWidthResetGen` 换代，只覆盖进程表）。
- 证据：`NetColCache::loaded` 置 true 后不再读 cfg；`ResetLayout` 把 `netcol_*` 软删为 ""（`ThemeCfg.h:110-118`）但两处缓存无世代号概念。点「重置布局」后 toast 声称「已重置布局（清除 N 项布局设置）」，网络页/netmon/GPU 明细表仍按旧宽渲染，重启才回默认。进程表却当场复位（`Pages.cpp:777-782` 换代分支），行为不对称。后续若仅拖动其中一列，`NetColSaveWidths` 只回写变化列，缓存与文件进一步脱节。
- 修法：`NetColCache`/`GpuColCache` 增记 `uint64_t gen`，加载时缓存 `Ui().colWidthResetGen`（或进程级 g_layoutResetGen）；`NetColWidths`/`GpuColWidths` 发现 gen 不匹配则丢弃缓存重新从 cfg 加载（cfg 已软删 → 回默认，`loaded` 重置）。

**P1-2 退出清理只剔 colW_：重置布局软删的 netcol_*/layoutScale/colOrder/perfZoom 空值行残留在 config.json**
- 位置：`src/app/main.cpp:593-599`（退出仅 `ui3::StripColWidthKeysFromFile(..., onlyEmptyValues=true)`）；`src/app/ui3/ThemeCfg.h:100-118,218-223`（`SoftDeleteLayoutKeys`/`StripLayoutKeysFromFile` 声明的两步语义）。
- 证据：`ResetLayout` 点击时已 `StripLayoutKeysFromFile`（全量剔除）；但之后每次正常退出 `cfg.Save` 会把内存里软删为 `""` 的键再写回文件（`Cfg.cpp:118-143,189-191`，`Escape(L"")` = `""` 两字符），退出剔除却不覆盖 `netcol_` 前缀与三个精确键 → 每轮「重置布局 + 退出」在 config.json 残留约 35 行 `"netcol_xxx_n": ""`、`"layoutScale": ""` 等。功能无害（`Cfg::GetDouble/GetInt` 对 `""` 原样值 `wcstod/wcstoll` 解析失败回默认，`Cfg.cpp:167-181`），但违背 ThemeCfg.h 头注声明的清理契约并持续累积垃圾行。
- 修法：main.cpp:598 改为一次 `ui3::StripCfgKeysFromFile(ConfigPath(), {"colW_", "netcol_"}, {"layoutScale", "colOrder", "perfZoom"}, /*onlyEmptyValues=*/true)`（该函数签名已支持，`ThemeCfg.h:125-211`）。

## P2（5 项）

**P2-1 EllipsizeTextUtf8 两处边界缺陷（共享头文件 API 契约；生产调用点 outCap=512 不可达）**
- 位置：`src/app/ui3/StatusLayout.h:164-168`。
- 证据：① 末尾 `const size_t n = keep < outCap - 4 ? keep : ...` —— `outCap<4` 时 `outCap - 4` 发生 size_t 回绕为天文数字，`n=keep` 可远超缓冲区，随后 `memcpy(out, u8, n)` + 写「…」越界（`keep==0` 分支在 159 行有 `if (outCap < 4) return 0;` 防御，末尾分支漏了同款）。② 快路径 `n = len < outCap - 1 ? len : outCap - 1`（135-138 行）在原文超缓冲但宽度放得下时按任意字节截断，可能切断 UTF-8 序列尾（仅当降级原因 > 511 字节，观感问题）。
- 修法：函数体开头统一 `if (outCap < 4) return 0;`；快路径按码点回退一个边界。补 selftest：outCap=1/2/3 用例。

**P2-2 netcol 加载宽度无合理域钳制（对比 colW_name 的 kMaxPlausibleWeight 先例）**
- 位置：`src/app/ui3/Pages3.cpp:121-127`、`src/app/ui/Pages.cpp:2725-2731`。
- 证据：cfg 损坏（手改/位翻转）写入 `netcol_netconn_1: -5` 或 `1e9` 时原样进 `TableSetupColumn`：负宽被 ImGui 静默当作 auto-fit，巨值产生超宽列（可横向滚动，不崩溃）。进程表加载有 `kMaxPlausibleWeight` 防御（`Pages.cpp:871-873`），netcol 两侧均无。
- 修法：加载时 `if (!(w > 0.0f) || w > 4096.0f) w = defaults[i];`（Fixed 列正常宽域 0..4096px，Stretch 权重域另有上限，可按列型分立）。

**P2-3 重置布局后表头排序箭头丢失（换代未复位 sortReflected_）**
- 位置：`src/app/ui/Pages.cpp:777-782`（换代分支）与 `1243-1250`（`ReflectPersistedSortOnce` 由 `sortReflected_` 门控，进程级一次）。
- 证据：换代后表格 id 变为 `procs#<gen+1>`，ImGui 新表所有列 `SortOrder=-1`（无箭头）；`sortReflected_` 仍为 true 不会重放 `TableSetColumnSortDirection`。数据排序仍按 `sortColumn_` 正确进行，仅指示丢失，直到用户再点表头。树形视图切换有复位（1085 行），换代分支漏了同款。
- 修法：换代分支加 `sortReflected_ = false;`。

**P2-4 从未拖动列的用户 config 里也会写入恒等 colOrder（噪音键）**
- 位置：`src/app/ui/Pages.cpp:1227-1241`（`PersistColumnOrder`）。
- 证据：`colOrderLastSaved_` 初始为空（无持久化值时），首帧读到的 ImGui 显示序为恒等 → `s != colOrderLastSaved_` 成立，1 秒节流后即写 `"colOrder": "0,1,...,12"`。功能等价默认，但所有用户都背上一个本可不存在的键。
- 修法：恒等排列时跳过写入（`IsIdentityOrder(byDisplay)` 提前 return），或在 `LoadPersistedOnce` 把空值情形的 `colOrderLastSaved_` 预置为恒等串。

**P2-5 布局缩放不含字体；DPI 为点击时单次快照（设计口径需明示）**
- 位置：`src/app/ImGuiLayer.cpp:23-42`（字体固定 16px，无 DPI 推导）；`src/app/ui/Pages.cpp:3226-3239`（`ApplyOneClickLayout` 一次性 `GetDpiForWindow`）；`src/app/ui3/PageLayout.h:65-80`。
- 证据：进程声明 PerMonitorV2（`Win32Window.cpp:11`），150%/200% 显示器上窗口按物理像素渲染而字形仍 16px；「一键优化布局」放大的是 padding/行高/定高，字体不随 `scale` 放大，高 DPI 下「间距大字小」的观感可能反直觉。`GetDpiForWindow` 在 PMv2 下返回窗口当前所在显示器 DPI（正确取点），但移动到异 DPI 显示器后不自动更新，需再点一次（一次性按钮语义，可接受）。
- 修法（后续轮）：字体随 DPI/缩放重建（1.92 动态图集可 `FontSize` 热更）；或至少在按钮 tooltip/说明模态写明「布局缩放不改变文字大小」。

## 已查无问题清单

1. **UTF-8 码点安全截断**（StatusLayout.h:126-169 + selftest ui_p1_test）：4 字节序列（emoji F0 头）按 4 步进；截断恒落码点边界（selftest 钉死 `(n-3)%3==0`）；截断结果含「…」实测宽 ≤ 预算；预算 ≤ 0/空串 → 空输出；极小预算连「…」都放不下 → 空输出；非码点首字节按 ASCII 边界不越界。生产调用 `anchorBuf[512]` + `minBudget` 保底（Pages.cpp:3399-3404）。
2. **锚点末端计算**：`AnchorEndX = 起点 + max(实测宽,0)`，不再读回被 ItemSize 重置的 CursorPos（根因正确）；`[已暂停]`/四槽/自由文本/右段全部沿 `leftEndX` 显式推进，`drawSlotText` 的 `x + pipeW + sp + textW == x + slotW` 恒等，无重叠路径；`lastDrawn` 在 firstBadSlot=0/CSV 隐藏等组合下均正确回退。
3. **ImGui 1.92.9 重排语义**（对照 third_party/imgui/imgui_tables.cpp）：
   - `TableSortSpecsBuild`（3205-3234）：`ColumnUserID = column->UserData`、`ColumnIndex = 提交序 column_n`，均与显示序无关 → 重排后 `Specs[0].ColumnUserID` 稳定，`ProcColumnFromUserId` 映射正确；
   - `TableSetColumnIndex(n)` 按提交序取 `Columns[n]`（cell 定位用 `column->MinX`，显示位置由布局阶段决定）→ 行数据列对应正确；
   - `TableSetColumnDisplayOrder`（731-755）为"移动+平移"语义：按 `d=0..N-1` 顺序把 `pending[d]` 移到位置 d 是选择排序式重建，数学上精确还原保存的排列；
   - `TableUpdateLayout` 惰性（首个 `TableNextRow`/`TableGetSortSpecs`/`EndTable`，1472/2067/3069 行）→ BeginTable 后 SetupColumns 阶段改 DisplayOrder 安全；
   - 列数恒 13（`kProcColSlots`，无列数变化路径）；`ProcColumnOrderFromCfg` 拒绝越界/重复/字段数错误/空字段/尾逗号（SortKey.h:183-212）→ `TableSetColumnDisplayOrder` 入参恒在界内，IM_ASSERT 不可能触发，恢复失败即整体放弃保默认序；
   - `ReflectPersistedSortOnce`/`PersistWidths` 均按提交序（`sortColumn_`/`Columns[i]`），与显示序解耦；列宽持久化在重排后仍写对键。
4. **netcol 持久化口径**：7 表 spec（ThemeCfg.h:75-83）与实际 `BeginTable` id/列数逐一核对一致；7 张表的 `kPersistCols` 拉伸位全部 false（netmon_events[3]、netmon_top[2]、netmon_dns[1,2]、pcap_pkts[1,4]、netconn[5]、gpuadapters[0]、gpuprocs[0]），拉伸权重不入文件；表名 ASCII 互异，`NetColCfgKey` 无冲突；`StripCfgKeysFromFile` 行形不认识即返回 -1 不重写（宁缺毋滥）；`removed<=0` 不重写保 mtime；tmp+MoveFileExW 原子替换；离屏 smoke（liveCtx 空）只回默认宽、跳过持久化。
5. **「刷新适配器」重名**：已不存在。工具栏无「刷新」按钮（仅「刷新间隔」滑块 Pages.cpp:3280，label 即 ID 不同名）；适配器行为「刷新适配器」（Pages3.cpp:1382）；连接页/启动项页「刷新」（1613/1852）分属互斥 TabItem，同窗不同帧提交，无 ImGui ID 撞名。外观模态与工具栏的「一键优化布局」分属不同 ImGui 窗口（popup），无撞 ID。
6. **一键优化布局数学**：`LayoutScaleFromEnv` 取 max(分辨率因子, DPI 因子) 防双重放大，[1,2] 钳制，0/负/NaN 回 1.0；`Scaled(1.0f)` 逐位恒等（selftest 回归断言钉死）；style 每帧自编译期基准重算（Pages.cpp:3643-3658），非累乘 → 无漂移；`layoutScale` 损坏值经 `SetLayoutScale` 防御回 1.0。**与主题切换叠加正确**：`Theme::Apply`（Theme.cpp:159-174）重置 FramePadding/ItemSpacing 为基准后，同帧 `DrawShell` 的逐帧重算立即以缩放值覆盖；ApplyLayout 不触碰 CellPadding/ItemInnerSpacing/IndentSpacing，两者无竞争字段。
7. **传感器页收缩（重点评估，结论：该策略在本布局下稳定，未引入新的不稳定）**：
   - 测量在内容空间：`GetCursorPosY()` 已减 `Scroll.y`（imgui），用户滚动不影响测量 → Child 高与滚动状态解耦；
   - 无反馈振荡：`DrawSensorBody` 及全部分组（BeginGroup=AutoResizeY 子窗、DrawReadings/DrawCoreTable/DrawDisks 普通表格与文本行）均无纵向 `GetContentRegionAvail` 依赖 → contentH 不是 child 高的函数，`h(N)=min(regionH, contentH(N-1))` 是开环映射，浮点同输入同输出，无累积漂移；
   - 滚动条二稳态收敛：内容含 `TextWrapped`（2730 行）时，宽度经滚动条出现/消失影响折行行数，但两个稳态（无滚动条 h=contentH；有滚动条 h=regionH 且内容更高）各自自洽，边界附近最多一帧滚动条闪现；
   - 下方无元素：区位于页尾，收缩/伸展不改变任何区外 y 坐标 —— 用户要消除的「下方元素移动」在结构上不可能复发；顶部三段行数恒定契约不受影响；
   - 数据 10s 节拍变化时一帧滞后（长高一帧内滚动条闪现/缩短一帧余白），幅度为行高、单帧，不可感知。**替代方案评估**：滞后带（如变化 < 一行高不跟随）或「只增不减 X 帧」钳制可消除这最后一帧瞬态，但代价是收缩响应延迟；在当前页尾布局下收益不成立，维持现状合理。若未来该区下方再放内容（区高变化会推移下方元素），必须先行加滞后带或恢复定高 —— 此为唯一需要注意的约束。
8. **实跑与回归**：selftest 159/159 双轮稳定（含 p1_status_anchor/ellipsize、p1_proc_column_userid/colOrder 校验、p1_netcol_key_names/剔除、status_slots 坐标恒定/窄窗降级等本轮契约测试）；smoke 150 与六条 autotest 全 PASS（见上表）。

## 备注

- `Pages3.new`（仓库根，0 字节，2026-09-18）为历史遗留空文件，非本轮产物，建议清理（不计入 P 级）。
- 本报告为唯一新增文件：`docs/phase/14_review_V29.md`。

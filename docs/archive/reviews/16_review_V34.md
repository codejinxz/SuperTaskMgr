# 终审评审报告 V34（2026-09-21）——U1 网络页纵向分栏 + v1.0.0 发布准备终审

评审人：V34（独立终审，与其他评审者互不知情）。禁 git；除本报告外未修改任何文件；可运行 build/Release 产物。
对象：①U1 网络页纵向可拖拽分栏（SplitterUi.h + PageLayout.h ClampRegionH/NetRegionHeights + Pages3.cpp NetworkPage）；
②v1.0.0 发布准备（AboutInfo.h/app.manifest、redist/npcap-1.89.exe、20+ 构建目录清理）；
③全仓回归（selftest 双轮、--smoke 150、--autotest 六条、历史缺陷抽验）。

## 测试与采样执行记录

| 项目 | 结果 |
| --- | --- |
| 产物新鲜度 | build/Release/*.exe（Sep 21 06:45–06:47）晚于全部最新源码（ui_m2_test.cpp 06:20:52）——实跑即当前代码 |
| stm_selftest 第 1 轮 | **176 通过 / 0 失败**（exit 0；V33 时 173 → 本轮新增 U1 分栏 3 项：契约 5/6/7） |
| stm_selftest 第 2 轮 | **176 通过 / 0 失败**（exit 0，双轮稳定）；`--json` exit=0 |
| SuperTaskMgr.exe --smoke 150 | exit=0，无输出无告警（离屏退化路径全走通） |
| --autotest 六条 | about / wallpaper / dialogclick / kill / startup / tree 全部 exit=0；autotest_result.log 末六条全 PASS（wallpaper 帧顶点=7182、壁纸绑定绘制命令=1；dialogclick 真实管线点击生效；kill 终止 cmd；startup 禁用「stm_f1_autotest」；tree 终止 3 个；about 模态 ≥4 帧后点击关闭） |
| 运行期日志 | 本轮全部运行 stm.log 末段无 ERROR（仅 netmon/etw 会话正常启停 INFO） |

---

## 一、U1 纵向分栏逐项审查

**状态机（SplitterUi.h）**：`RegionSplitterY` 由 ImGui item 激活态承载 Idle/Hovered/Dragging，无额外状态存储。
`InvisibleButton` 激活期间鼠标出带不中断拖动（ImGui 按钮语义）；`IsItemDeactivated()` 恰在松手帧为 true 一次，
且此前最后一次 `active` 帧（MouseDelta 累计）已写入 `*value`——松手写 cfg 丢至多一帧亚像素增量，无害。
纯点击（未拖动）也返回 true，但值未变、写回等值，无害。键盘 Nav 激活时 MouseDelta=0，不产生漂移。
`IsItemDeactivated` 紧随 `InvisibleButton`，中间仅 DrawList/光标调用，不改变「上一 item」——语义正确。

**钳制与守恒（PageLayout.h）**：`ClampRegionH` 对 0/负/NaN 一律回下限（NaN 比较恒 false 的写法正确），
cfg 读回与拖动增量共用唯一入口。`NetRegionHeights` 三态：充裕（connTable = usable−want，总和逐位=availY，
ui_m2_test:316 断言）、紧张（等比压缩保 connTable 保底，比例断言 362–366、单调断言 368–372）、
极小/离屏（全正非 NaN，断言 381–390）。cfg 三位小数往返误差 <0.001px。

**cfg 持久化**：读回一次性（`heightsLoaded_`，与 `etwLoaded_` 同模式），缺键默认经 Scaled 与改版前显示一致；
拖动中只改内存、松手 `SetDouble`（内存写），文件由 main 退出统一 Save——与 netcol/netEtw 同口径。
损坏/空值经 `Cfg::GetDouble` 尾部解析失败回默认 + `ClampRegionH` 双重收敛。

**与 V28「连接表标题 y 稳定」叠加**：无拖拽帧三区高度入参只有（cfg 两值、##netscroll 顶部实测 availY），
数据/展开状态绝非输入；availY 只随窗口缩放变化 → 无拖拽帧三区高度逐位恒定，V28 契约保持。
拖动中连接表随动 = 用户主动行为（契约已声明）；抓包段折叠/展开为唯一形态、展开定高 Scaled(340) 照旧由
##netscroll 滚动消化——与 U1 分配正交，未引入新位移源。

**嵌套滚动**：两条手柄位于 ##adapters / ##netmonsection 两个滚动 Child **之外**的父区（##netscroll），
拖拽与卡区内滚动互不抢夺；手柄激活期间 ImGui 持有 ActiveId，滚轮不会中断拖动。

**监视段折叠退化**：折叠帧 `DrawNetMon` 在 `CollapsingHeader` 失败处画 `ImGui::Separator()` 后早退
（Pages3.cpp:485-487）——手柄②不提交（无幽灵交互带），分隔线保留，netmonH 值保留待复用。正确。

## 二、发现

### P0（0 项）
未发现崩溃、越界、数据损坏、死锁或功能全失效类缺陷。

### P1（1 项）

- **P1-N1（新，「重置布局」未覆盖 U1 新布局键 netAdapterH / netmonH）**
  - 文件:行：`src/app/ui3/ThemeCfg.h:106-109`（LayoutResetExactKeys 仅 layoutScale/colOrder/perfZoom）、
    `ThemeCfg.h:111-119`（SoftDeleteLayoutKeys 仅 colW_*/netcol_*/三精确键）、`ThemeCfg.h:221-224`
    （StripLayoutKeysFromFile 同清单）、`src/app/ui/Pages.cpp:3536-3545`（ResetLayout 全链）、
    `src/app/main.cpp:628-629`（退出剔除清单，注释 V29-P1-2「范围扩到全部布局键」）；NetworkPage
    （`src/app/ui3/Pages3.cpp:313-325`）仅首次 Draw 读回，且未订阅 `LayoutResetGeneration()`。
  - 证据：全仓 grep `netAdapterH|netmonH` 仅命中 Pages3.cpp / PageLayout.h——重置布局的软删除、文件剔除、
    代际失效三条链路均不含新键。后果：拖动过分栏后点「重置布局」，toast 报「已重置布局（清除 N 项布局设置）」
    但网络页两区高度不变；会话内 `heightsLoaded_` 已置位也不会重读；下次拖动还会把旧值重新写回 cfg。
    与 V29-P1-1（重置布局不清 netcol 缓存，判 P1）完全同类，属 1.0.0 新功能与既有按钮的功能性矛盾。
  - 修法：①两键加入 `LayoutResetExactKeys()` 与 `StripLayoutKeysFromFile`/main.cpp:628 精确键清单
    （同步更新 ui_p1_test.cpp:299/464 的清单断言）；②NetworkPage 记录 `LayoutResetGeneration()` 快照，
    变化即重读 cfg（仿 NetColCache.loadedGen，Pages3.cpp:105-136 同构）。

### P2（4 项新）

- **P2-N1（文档数字漂移复发，V33-P2-N1 同类）**：`README.md:34`「自测（173 项）」、`一键编译.bat:16`
  「运行自测（173 项）」——实测 176 项（本轮新增 3 项未同步）。纯文字失真。
- **P2-N2（U1 名义数学与实际布局的偏差，无视觉后果）**：①`SplitterUi.h:31` 手柄画 `Scaled(6)`，
  而 `PageLayout.h:257` 分隔条占位减的是未缩放 `2*kNetSplitterThickness`=12 → 缩放 s>1 时充裕判定/
  connTable 名义值偏差 12(s−1)px；②名义 connTable 保底未扣抓包头（折叠 ≈22px）、工具栏（≈26px）、
  两条 Separator 与 ItemSpacing ≈50–60px——默认 200+340 下 1280×800 视口实际已入「紧张」压缩态，
  实际连接表可见高可低于名义保底 120px。因连接表 `BeginTable(0,0)` 自动填充吃剩余空间，无错位/无多余
  滚动条，仅保底声明与实际的失真；建议注释声明「名义口径」或把 chrome 行数纳入纯函数入参。
  （cfg 存真实像素不随缩放重标定为已声明设计，与 netcol 同口径，不另计。）
- **P2-N3（清理遗留）**：`scripts/rf_build.bat:7` 仍 `--build build_rf`（目录已删，运行即失败）；
  `scripts/rf_probe.bat` 自建 build_rf 尚可运行；根目录遗留 `build_u1/`、`build_log.txt`、`build_log_d.txt`、
  `v26_selftest_r1.json` 未纳入本轮清理。无功能引用损坏，仅发布打包时需排除。
- **P2-N4（1.0.0 发布 polish）**：`src/app/app.rc` 无 VERSIONINFO 资源（仅图标/徽标）——资源管理器
  「详细信息」页读不到 FileVersion/ProductVersion；`redist/npcap-1.89.exe` 已核对（见下），但 README/发布
  说明未记载其用途、来源与 SHA256，且 Npcap 许可对再分发有专门条款，发布前建议在 README 注明
  「来源 npcap.com、版本 1.89、SHA256、许可注意」或改为发布时按需下载。

## 三、发布准备核查（无问题项）

1. **版本一致性**：`AboutInfo.h` kAppVersion=1.0.0 = `app.manifest` assemblyIdentity 1.0.0.0；
   窗口标题（main.cpp:412）、关于页（AboutUi.cpp:155）、兼容诊断报告（Pages.cpp:3233/3359）均单一来源
   kAppVersion，无第二处硬编码版本。
2. **redist/npcap-1.89.exe**：1,325,224 字节；Authenticode **签名 Valid**（CN=Nmap Software LLC，
   有效期至 2027-06-10）；SHA256 = 8AED85E900D783D1308506E919587D3E540451947AF8A82F2D04F819E44305CC。
3. **残留引用**：src/、CMakeLists.txt、scripts/build.bat、一键编译.bat、README.md 均无 build_d1/build_c2_npcap
   等已删路径引用（唯 rf_*.bat 两条遗留 → P2-N3）；一键编译.bat → scripts/build.bat → build\Release 链路完好，
   本轮实测全链可跑。

## 四、已查无问题清单（本轮新眼终扫）

1. **SplitterUi.h 状态机**：激活/悬停/松手帧语义、出带不中断、纯点击等值回写无害、键盘 Nav 无漂移、
   `IsItemDeactivated` 归属 item 正确（见 §一）。
2. **ClampRegionH/NetRegionHeights 纯函数**：区间内逐位恒等、越界收敛、NaN/0/负全防御、充裕严格守恒、
   紧张等比压缩、极小/离屏全正——ui_m2_test 契约 5/6/7 断言齐备，176 套件双轮通过。
3. **cfg 往返**：一次性读回 + 唯一钳制入口 + `{:.3f}` 写入误差 <0.001px + 解析失败回默认；松手才写、
   退出统一 Save，与 netcol/netEtw 同口径。
4. **V28 叠加**：无拖拽帧连接表标题 y 逐位恒定（三区高度与内容无关）；拖动/折叠展开均为声明容忍的用户主动位移。
5. **嵌套滚动**：手柄在两个滚动 Child 之外；拖拽与卡区内滚动互不干扰；激活期滚轮不打断。
6. **折叠退化**：手柄②折叠帧不提交、Separator 保留（Pages3.cpp:486）、netmonH 复用无丢失。
7. **离屏 smoke 防御**：`FillScrollRegionHeight`→1px、connTable=1px、手柄 w<8 保正——smoke 150 exit=0 实证。
8. **历史缺陷抽验**：确认框/壁纸/关于（--autotest dialogclick/wallpaper/about 本轮实跑 PASS）；
   兼容模态（compat_test + 诊断报告带 kAppVersion）；内存加速入口（ui_memcleanup_test + fix_confirm_*）；
   状态栏槽位（ui_status_slots_test）；顶栏固定（ui_header_test + 本轮 NetworkPage 头行仍在 ##netscroll
   之外，Pages3.cpp:333-338，未破坏 P-A 口径）；列重排（ui_p1_test colOrder 全链路）——全部在 176 套件内通过。
9. **运行稳定性**：本轮 selftest×3、smoke、autotest×6 期间 stm.log 无 ERROR，无孤儿 ETW 会话
   （退出日志均为「会话已停止」INFO）。

## 五、结论

**P0=0，P1=1（P1-N1 重置布局漏 U1 两键），P2 新 4 条。** U1 分栏交互/钳制/守恒/持久化/契约叠加实现质量高，
纯函数与 selftest 覆盖完备；发布准备项（版本三处一致、npcap 签名、构建链路）核查通过。

**发布判定：需修复**——修复 P1-N1（约 4 处清单 + NetworkPage 代际重读，一处小改）并回归 ui_p1_test 后即可发布；
P2 四条不阻塞，建议发布说明与 README 数字随 P1 同轮刷新。

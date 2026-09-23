# 终审评审报告 V33（2026-09-21）——UI/UX 与历轮用户报告问题终验

评审人：V33（独立终审，与其他评审者互不知情）。禁 git；除本报告外未修改任何文件。
对象：全仓 UI/UX 终扫 + 历轮用户报告①–⑫逐条终验 + 交付文档核对 + 实跑。
产物新鲜度：`build\Release\*.exe`（Sep 21 04:55）晚于全部 src 源文件（最新 04:47），实测即当前代码。

## 测试与采样执行记录

| 项目 | 结果 |
| --- | --- |
| stm_selftest.exe 第 1 轮 | **173 通过 / 0 失败**（exit 0；v30 为 159，本轮前新增 logfile/ui_logviewer 等 14 项） |
| stm_selftest.exe 第 2 轮 | **173 通过 / 0 失败**（exit 0，双轮稳定） |
| SuperTaskMgr.exe --smoke 150 | exit=0，无输出无告警（首次运行 exit=1 为并行会话遗留实例持有单实例互斥量的预期 CI 守卫，main.cpp:288-292；清除后复跑通过） |
| --autotest 六条 | about / dialogclick / kill / startup / tree / wallpaper 全部 exit=0；autotest_result.log 末六条全 PASS（about 模态≥4帧+关闭；dialogclick 真实管线点击生效；tree 终止 3 个；wallpaper 帧顶点=7054、壁纸绑定绘制命令=1） |
| 真实实例 60s 双采样 | WS 128,868 KB → 128,188 KB（60s 间隔，无增长）；私有 114,252 → 114,288 KB；句柄 698 → 688；本轮全部运行日志 **0 条 ERROR** |

---

## 一、历轮用户报告终验表（①–⑫）

| # | 用户报告 | 判定 | 证据（代码路径 + 实跑） |
| --- | --- | --- | --- |
| ① | 确认框点击无反应 | **已解决** | `ui/Pages.cpp:360-385`「请求长期有效 + 模态每帧渲染」：OpenPopup 仅发一次（confirmOpenRequested 消费后清零），BeginPopupModal 每帧执行，Esc 路径复位不留僵尸模态；无每帧 SetNextWindowFocus（全仓 grep 无）。实跑：`--autotest dialogclick` 真实输入管线点击 PASS（模态连续≥4帧 + 点击生效 + 子进程退出），代码含「单帧化回归」哨兵（AutotestDialog.cpp:80-100） |
| ② | 壁纸不生效 | **已解决** | 帧序 `main.cpp:535-544`：BeginFrame → NewFrame → **WallpaperDrawBackground** → Render → Present（背景绘制列表必须 NewFrame 后追加，V18 探针口径）；外壳 `Pages.cpp:3962-3969` WallpaperActive() 时推全透明 WindowBg/ChildBg；退出 `Wallpaper.cpp:255-265` WallpaperShutdown 只释放纹理、保留 %LOCALAPPDATA% 副本供 WallpaperAutoRestore 重载（main.cpp:630、437）。实跑：`--autotest wallpaper` PASS（顶点=7054、绑定绘制命令=1） |
| ③ | 关于点不到（菜单矩形遮挡） | **已解决** | HeaderLayout.h 纯函数装箱（右→左、优先级隐藏、`x >= leftFlowEndX` 不遮左流契约，ui_header_test 断言）；工具条右侧仅剩「关于」按钮（`Pages.cpp:3625-3646`），每帧发布真实按钮矩形进 AboutAutotestState。实跑：`--autotest about` PASS（模态打开≥4帧、点「关闭」退出） |
| ④ | 兼容模式不可解释 | **已解决** | 状态栏降级锚点可点击（`Pages.cpp:3712-3730`：手型光标 + tooltip 带完整未截断原因）→ `##compatdiag` 模态（3108-3270）：原因摘要 + 6 项自检逐项表（√/×/—）+「为什么会这样」+ 受影响功能清单 + [重新自检]（代际检测 V24-P1-1、暂停时禁用）+ [复制诊断报告]（剪贴板 + logs\diagnostics_*.txt，不自动上传）；Esc 复位不留残留（3107-3113） |
| ⑤ | 内存加速不可见 | **已解决** | 三入口同路：工具条「内存加速…」（Pages.cpp:3605，任意页签常驻）+ 性能页顶部固定操作行（2074/2614，图表区之前）+ 性能页内存块（2655）→ 全部 `RequestConfirmMemCleanup()` 单执行路径。文案三处统一「内存加速…」 |
| ⑥ | 工具条挤成一团 | **已解决** | 工具条改流式 SameLine 序列（DrawToolbar 3517+）；状态栏两段式：左段锚点+定宽槽（LayoutFlowSlots 依约隐藏），右段 LayoutHeaderRight 装箱（热键先藏、徽标次之、帧耗时永不藏）；窄窗降级不重叠（ui_header_test / ui_status_slots_test 在 173 套件内通过） |
| ⑦ | 帧毫秒漂移 | **已解决** | StatusLayout.h 定宽槽位：槽宽只来自编译期常量样本 `MaxSampleWidth(kMsNumberSamples,…)`，数值位数变化只改槽内文本、坐标逐帧恒定（`FormatSlotMs` 钳制 + 头注释契约）；状态栏不再用当前数值实测宽定位 |
| ⑧ | 页顶栏随内容移动 | **已解决** | 三页固定：性能页「内存加速+速览/时间窗」固定页头（2074-2118，空数据早退分支画在顶栏之下）；网络页适配器卡区定高 `AdapterRegionHeight(Scaled)`（Pages3.cpp:1421-1463）；传感器页工具行/显隐行/LHM 区恒定行数 + `FillScrollRegionHeight`「页高减顶栏」口径（2683-2726） |
| ⑨ | 传感器白块 | **已解决** | `PageLayout.h:83-127`：定高 Child 改「精确填满」口径，内容收缩不再留随主题变白/变的空隙；V29 复核收缩-滚动无振荡、区外 y 恒定 |
| ⑩ | 列不能重排/网络列不能调宽 | **已解决** | 进程表 colOrder 全链路：`SortKey.h:183-212` 校验解析 + `TableSetColumnDisplayOrder` 恢复 + 按槽位 UserID 映射（拖动后排序/列宽/列序互不错位，ui_p1_test 钉死）；网络/GPU 7+2 表 `NetColWidths→TableSetupColumn→NetColSaveWidths`（Pages3.cpp:117-155，35 键 + GPU 同构），变化驱动 + 节流回写 |
| ⑪ | 主题/壁纸/关于/日志按钮缺失 | **已解决** | 工具条常驻：「主题…」（外观模态：主题三态/壁纸/遮罩滑条/一键优化布局/重置布局/恢复默认列宽）、「日志」（日志查看器：级别过滤/刷新/自动刷新/诊断报告/打开目录）、「关于」、「一键优化布局」均带 tooltip；实跑 about/wallpaper autotest 全 PASS |
| ⑫ | 一键优化布局 | **已解决** | `ApplyOneClickLayout`（Pages.cpp:3517-3531）：max(分辨率因子, DPI 因子) 防双放大、[1,2] 钳制、损坏值回 1.0；每帧自基准重算无漂移；工具条 + 外观模态双入口同函数；toast 明示「重置布局还原」。字体不随缩放为既定设计（V29-P2-5 已声明口径） |

**统计：12/12 已解决，0 部分解决，0 未解决。**

## 二、前轮遗留 P1/P2 修复核验（V29/V30 → 本轮）

| 项 | 判定 | 证据 |
| --- | --- | --- |
| V29-P1-1 重置布局不清 netcol 缓存 | **已修复** | `NetColCache.loadedGen`/`GpuColCache.loadedGen` 代际失效重读（Pages3.cpp:105-136、Pages.cpp:2752-2790）；`ResetLayout` 调 `ui3::NotifyLayoutReset()`（Pages.cpp:3536） |
| V29-P1-2 / V30-P2-2 退出残留布局键 | **已修复** | main.cpp:610-616 `StripCfgKeysFromFile({colW_, netcol_}, {layoutScale, colOrder, perfZoom}, onlyEmptyValues=true)` |
| V30-P2-4 恢复窗口可落屏外 | **已修复** | main.cpp:481-498 `MonitorFromRect(MONITOR_DEFAULTTONEAREST)` + 工作区钳制（保尺寸只平移，至少露 120×80 可抓取） |
| V29-P2-1 EllipsizeTextUtf8 outCap<4 回绕 | **已修复** | StatusLayout.h 末尾分支 `(outCap >= 4 ? outCap - 4 : 0)`；keep==0 分支已有 `outCap < 4` 防御（快路径残留见 P2-4） |
| V30-P2-3 一键布局跨屏 DPI 取样 | **未修（P2 维持）** | 仍取主视口 WorkSize + GetDpiForWindow（Pages.cpp:3517-3524）；tooltip 未注明「移动显示器后可重按」；混合 DPI 副屏场景可能偏大 |
| V29-P2-2 netcol 加载宽度无合理域钳制 | **未修（P2 维持）** | Pages3.cpp:126-131 / Pages.cpp:2759-2768 原样透传 cfg 值；进程表有 `kMaxPlausibleWeight`（Pages.cpp:894-896）而 netcol/GPU 侧无 |
| V29-P2-3 重置布局后表头排序箭头丢失 | **未修（P2 维持）** | 换代分支（Pages.cpp:799-806）未复位 `sortReflected_`（仅 1108 树形切回落位）；换代后新表无指示，点表头即自愈 |
| V29-P2-4 恒等 colOrder 噪音键 / V30-P2-5 colOrderAppliedGen_ 死字段 | **未修（P2 维持）** | PersistColumnOrder 仍写恒等串（1255-1261）；`colOrderAppliedGen_` 仍只写不读（1239/1999） |

## 三、新发现

### P0（0 项）
未发现崩溃、越界、数据损坏、死锁或功能全失效类缺陷。

### P1（0 项）
本轮未发现新的 P1。历史两项 P1（netcol 缓存失效、退出布局键残留）均已确认修复且回归通过。

### P2（4 项新 + 4 项维持）

- **P2-N1（新，文档漂移）交付文档数字与实际不一致，一处 PR 可全部修正**：
  ① `一键编译.bat` 第 2 步文案「运行自测（**93 项**）」——实测 173 项；
  ② `README.md`「构建」节写 stm_selftest「自测（**40 项**）」——实际 173 项；
  ③ `README.md` 开头「便携 exe（约 **1.7MB**）」——现主程序 2.83MB（2,909,696 字节；1.7MB 现为 stm_selftest.exe 的体积）；
  ④ `README.md` CI 节仅列 `--autotest kill|tree|startup|dialogclick` 四条，**about / wallpaper 两条已存在且本轮实跑通过但未记载**；
  ⑤ `README.md`「空载内存 ≈124MB」——本轮实测 WS ≈128.2–128.9MB（仍满足 01 文档 ≤150MB 门禁，但具体数字已漂移约 4MB）。
  均为文字失真、无功能影响；建议维护轮统一刷新。
- **P2-N2（新，微）**外观模态「恢复默认列宽」按钮无 tooltip（同排其余按钮均有），且其作用域（仅进程表 colW_+colOrder）与「重置布局」（全部布局键）的差异只能靠后者 tooltip 间接推断；首次使用易混淆。建议补一行 tooltip。
- **P2-N3（新，观察，不计缺陷）**壁纸遮罩仅提供黑色加深（0–0.85，默认 0.45）：浅色主题 + 高亮度壁纸区域时深色文字可读性依赖用户调遮罩；选项旁已有性能开销提醒劝导默认关，属可接受设计口径，建议后续可考虑「提亮遮罩」选项。
- **P2-C1…C4（维持）**＝上表 V30-P2-3、V29-P2-2、V29-P2-3、V29-P2-4/V30-P2-5 四条历史 P2 原样保留（均低危、有自愈路径或纯卫生问题）。

## 四、已查无问题清单（本轮新眼终扫）

1. **文案一致性**：「内存加速…」三入口统一、模态题「内存加速」、完成 toast「内存加速完成」；「一键优化布局/重置布局」为独立布局语义，无与内存优化混用的残留命名；用户可见字符串无旧称「一键优化」（仅存于代码注释）。
2. **按钮可达性**：工具条按钮为全局外壳常驻（任意页签）；状态栏/工具条窄窗按优先级隐藏而非重叠（LayoutHeaderRight 单调可见性契约 + ui_header_test 实测 ≥900px 全可见）；最低 600px 宽度契约由探针覆盖。
3. **三态完整**（空/加载/错误）：netmon 事件/端点/DNS（暂无+等待语义）、崩溃记录「近期没有崩溃/挂起记录」、启动项「未发现启动项」、适配器「未发现网络适配器」、磁盘「本机未发现可查询的磁盘」、日志查看器「无法读取日志」（原因+确切路径+重试指引）、兼容诊断「尚未自检」、服务/事件「部分读取失败：原因」——均诚实占位不伪造。
4. **快捷键与焦点**：Ctrl+Alt+M 默认关（main.cpp:474 GetBool("hotkeyEnabled", false)），开启态写 cfg、失败自动回退；NavEnableKeyboard 开启；全部模态 Esc/「关闭」双路径且复位打开态，无僵尸隐形模态。
5. **浅色主题对比度抽查**：StyleColorsLight 基础 + 浅灰窗体/深色文字；强调色（Done/Fail/Warn/Info/五类进程着色）浅色下全部换成加深变体（Theme.cpp ThemeAccentColor）；TextDisabled=0.42 灰于 0.96 底可辨；斑马纹 4.5% 黑两主题可辨。
6. **壁纸开启下的可读性**：遮罩钳制 [0,0.85]、每帧叠加黑色矩形；WindowBg/ChildBg 仅在 WallpaperActive 时透明、关闭壁纸即恢复不透明；mask 滑条实时生效（每帧读 cfg）。唯一残余即 P2-N3 观察项。
7. **图标与悬停提示**：状态栏锚点（手型光标+完整原因 tooltip）、仓库链接（手型+下划线提示+悬停 tooltip）、工具条/模态内全部动作按钮带中文 tooltip，含「不会自动上传」等诚实边界说明。
8. **本轮新增日志面（LogFile.h/LogViewer.h，04:09-04:47 最新改动）**：只读尾部窗口（512KB/500 行）、严格 UTF-8 校验（过长编码/代理区/超界拒绝）、半行丢弃与窗口首行保守丢弃、未解析行原样保留 + 统计行如实标注、「仅警告+错误」过滤宁缺勿滥（未知级别不放行）、诊断报告系统信息由 UI 注入不伪造——header-only 纯函数，logfile_test/ui_logviewer_test 在 173 套件内通过；stm.log 写方为 `_wfopen_s "ab"`（_SH_DENYNO），外部工具可读（本轮 cmd/.NET 实读成功，期间个别读取报错为评审环境工具瞬时占用，非产品互斥）。
9. **单实例/headless 守卫**：TryAcquire 失败时 headless 立即 return 1 不弹框（V11-P2-7），本轮实测与文档行为一致。
10. **实跑与稳定性**：selftest 173/0 双轮；smoke 150 exit=0；六条 autotest exit=0 全 PASS；真实实例 60s 双采样 128.9→128.2MB 无增长、句柄 698→688；运行期日志 0 ERROR。

## 结论

P0=0，P1=0（历史 P1 两项均已修复并核验），P2：新 2 条（文档漂移、tooltip 缺失）+ 观察 1 条 + 维持 4 条历史 P2。历轮用户报告 12/12 已解决，V29/V30 遗留修复 4/6 项落地（其余 4 条低危 P2 维持）。文档数字漂移（P2-N1）建议下一维护轮一次性刷新；不阻塞发布。

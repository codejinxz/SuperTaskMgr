# 终审报告 V19 —— R-Fix 三 bug 修复 / Logo / 内存加速可见性 / 注释中文化

- 评审人：V19（独立终审 subagent，与其他评审者互不知情；未用 git；除本报告外未修改任何仓库文件；
  临时探针程序放 %TMP%\v19probe，不在仓库内）
- 日期：2026-09-19
- 范围：①工具条重构（HeaderLayout.h 实测宽度定位 + 窄窗折叠 + 状态栏右段）；②壁纸"不生效"根因修复
  （透明 bg + WallpaperShutdown）；③关于「?」点击次序 + autotest about 哨兵；④Logo（gen_logo.cpp 手写
  ICO/PNG 容器 + app.rc RCDATA + AboutUi 纹理 + WM_SETICON/托盘）；⑤内存加速入口可见性；⑥注释中文化抽查
- 方法：通读全部相关源码 + 独立验证器实证（用仓库自带 stb_image 解码产物资产）+ RFC 1951 距离码表
  全量暴力校验 + 修法在临时副本上复现验证 + 全量产物实跑
- 产物基线：build/Release（2026-09-19 07:59 构建，晚于全部源码最新 mtime 07:48，无需重建）

## 统计

- **P0：1（gen_logo 的 PNG deflate 距离码双重缺陷 → 本轮交付的 logo_256.png 与 app.ico 256px 块全部损坏，
  关于页徽标 100% 静默不显示）**
- P1：1（工具条 leftFlowEndX 传的是菜单"起点"而非"终点"，窄窗「?」与「外观/⋮」标签重叠）
- P2：5
- 实跑：stm_selftest **93/93 通过 0 失败**（exit 0）；`--smoke 150` exit 0（stm.log 235 条 smoke 记录）；
  `--autotest dialogclick/kill/tree/startup/about/wallpaper` 六条全 PASS（exit 0×6，
  autotest_result.log 尾行逐一核对）
- 编码：124 个源文件全量 `file` 核验均为 UTF-8/ASCII，无 U+2028/2029、中途 BOM、控制字符、mojibake

---

## P0

### P0-1 gen_logo.cpp 的 deflate 距离码写入走错码表 + 距离码公式偏移 −2 → 生成的 PNG 全部损坏，而工具报 OK

- 文件:行：
  - `tools/gen_logo.cpp:393` —— `PutFixedSym(bw, dsym);  // 距离符号 0..29 落在 8 位字面量码区间`
  - `tools/gen_logo.cpp:335` —— `*sym = 2 * hb - 2 + ((v >> (hb - 1)) & 1);`（DistToSym）
- 证据（三重实证，非推测）：
  1. **码表用错（主因）**：RFC 1951 §3.2.6 中固定 Huffman 的距离码是**独立 5 位码表**，与
     字面量/长度码表无关。`PutFixedSym` 把 0..29 的距离符号写成 8 位**字面量**码（0x30+sym）；
     解码器在长度码之后按 5 位读距离码 → 首个匹配即位流失步。注释本身就是对规范的误解。
  2. **公式偏移**：对 RFC 1951 §3.2.5 距离表（base/extra 全 30 项独立建表）在 1..32768 全距离上暴力
     校验：原式 `2*hb-2+bit` **32764/32768 个距离错误**（d≥5 全错）；`2*hb+bit` 0 错。
  3. **产物实测**：用应用同款 `third_party/stb/stb_image.h` 编译独立验证器解码仓库资产：
     `assets/logo_256.png` → `stb decode failed: bad dist`；`assets/app.ico` 目录解析正常
     （6 个 BMP 块 + 1 个 256px PNG 块，偏移自检通过），但 256px PNG 块 → `bad dist`。
- 影响：`AboutUi.cpp:95`（EnsureAboutLogo）stbi 解码失败 → `g_logoSrv` 恒空 → **关于模态徽标静默不显示**
  （无崩溃、无重试风暴，g_logoTried 单次尝试）；资源管理器大图标视图 256px 条目失效（回退 128 BMP 放大）；
  工具打印 `OK ... (54022 bytes)` 且自带自检（:592-611 只校验 ICO 目录偏移连续）**无法发现**，属静默发布。
- 修法（已在 %TMP% 副本上验证）：
  - `:393` → `bw.PutCode(static_cast<uint32_t>(dsym), 5);`（固定距离码 5 位、MSB 先行）
  - `:335` → `*sym = 2 * hb + ((v >> (hb - 1)) & 1);`
  - 两处都改后重新生成：stb 解码 `logo_256.png` 与 ICO 内嵌 PNG 均通过（stb 含 zlib Adler32 校验），
    生成物 51793/153943 字节、ICO 自检 OK、256x256 RGBA 正常。
  - 重生成 `assets/app.ico`、`assets/logo_256.png`（`--previews` 的 preview_*.png 若要保留也需重生成）。
  - 建议给 gen_logo 自检加"独立解码 PNG + 像素抽查"一步，防止再次静默损坏（见 P2-4）。

---

## P1

### P1-1 工具条右缘布局把 leftFlowEndX 传成菜单"起点"，窄窗（约 <810px）时「?」压住「外观/⋮」标签，违反 HeaderLayout 契约

- 文件:行：`src/app/ui/Pages.cpp:2524-2525`（`menuX`/`leftEnd = menuX`）、`:2533-2541`；
  契约声明 `src/app/ui/HeaderLayout.h:23-24`（"任意宽度 >= 600 验证：摆位条目绝不遮盖左侧流"）
- 证据：LayoutHeaderRight 只保证 `place.x >= leftEnd`。此处 leftEnd=menuX 是菜单标签的**起始 x**，
  而菜单标签实际占据 `[menuX, menuX+menuW]`（SpanAvailWidth 的悬停延伸不影响可见标签）。
  右对齐的「?」位于 `winW - WindowPadding.x - aboutW ≈ winW-32`。以默认样式+中文字体估测：
  非提权工具条左流（滑条 160+标签、暂停采集、以管理员身份重启、清理待机缓存、内存加速… + 间距）
  终点 menuX≈740 → winW≲810 时「?」与标签部分重叠，winW≲770 时退化钳制把「?」钉在 menuX 上**整块盖住**
  「⋮」标签；提权（少一个按钮）下阈值约 ≲650。窗口无最小宽度限制（Win32Window.cpp 无 WM_GETMINMAXINFO），
  800px 级吸附窗口即可触发。缓解因素：「?」先提交仍可点（Bug3 不复活），菜单延伸区仍可点击，
  故为可见重叠/标签遮挡而非不可达。
- 为何测试没拦住：ui_header_test.cpp 与 UiFixProbe --mode=layout 均用**合成 leftEnd**（560/700），
  从未用外壳真实测量（菜单宽度未计入）；`--autotest about` 只在默认 1280 宽跑，且它只断言「?」可点，
  无法发现菜单被盖。
- 修法（一行）：
  ```cpp
  const float menuW = ImGui::CalcTextSize(narrow ? U8(L"⋮") : U8(L"外观")).x + st.FramePadding.x * 2.0f;
  const float leftEnd = menuX + menuW + st.ItemSpacing.x;  // 左侧流的"终点"，而非菜单起点
  ```
  并给 ui_header_test 补一条"外壳同款测量"用例把 menuW 计入 leftEnd 后复验 600..900 区间。

---

## P2

### P2-1 HICON"共享资源句柄"声明不成立（LoadImageW 未带 LR_SHARED）；TaskbarCreated 重注册每次泄 1 个图标句柄

- 文件:行：`src/app/main.cpp:388-403`、`src/app/ui/Tray.cpp:32-36`（注释）、`Tray.cpp:80-86,33-35`（重注册路径）
- 证据：不带 `LR_SHARED` 的 LoadImageW 每次调用返回**新的非共享句柄**（应 DestroyIcon）；"共享资源、
  进程生命周期内有效、无需销毁"的注释只有加 LR_SHARED 后才成立。进程一次性加载两枚无实害；但
  Explorer 重启 → TaskbarCreated → AddIcon() 再次 LoadImageW 且旧句柄不释放，每次泄 1 个小图标句柄。
- 修法：三处 LoadImageW(IMAGE_ICON) 一律加 `LR_SHARED`（此后注释即真，且重复加载命中缓存不再泄漏）。

### P2-2 AboutUi 哨兵 closeValid/closeHovered 不随帧重置（脏读隐患）

- 文件:行：`src/app/ui/AboutUi.cpp:126-127`（只重置 btnValid/btnHovered）、`:201-208`（模态打开才更新 close*）
- 证据：模态关闭后 `AboutAutotestState` 仍保留上一帧关闭按钮矩形/悬停值。当前 AboutClickDriver 仅在
  CloseHover 阶段读取（必经模态打开），无实害；但属哨兵脏数据，模式与 btn* 不一致。
- 修法：进入 DrawAboutUi 时一并 `at.closeValid = at.closeHovered = false;`。

### P2-3 注释中文化残留约 10 处句子级漏翻（多行句子的续行被跳过）

- 证据（抽查 15 文件 + 全树 `//` 行无 CJK 扫描，共 16 行，剔除命令行/JSON 示例后为真漏翻）：
  `src/app/ui3/Wallpaper.h:36-37`（"next launch (deleting it at exit made ... dead code). Use ..."）、
  `src/app/ui3/Wallpaper.cpp:258`（"the「关闭壁纸」action, is the one that deletes)"）、
  `src/app/ui/ConfirmAction.h:13`（"of failing silently (...)"）、`src/app/ui3/Pages3.cpp:3,8-9`
  （"plus the optional threshold alert watcher..." / "pages render a ... instead"）、
  `src/app/ui/HeaderLayout.h:11` 与 `src/selftest/ui_header_test.cpp:8`（同句 "bar right segment packs..."）、
  `src/collect/Sensors.cpp:12,491`、`src/collect/GpuCollector.cpp:33`（整句英文）。
- 修法：补翻上述行；可复用本轮的"无 CJK 纯英文注释行"扫描清单核对到 0。
- 已确认无问题面：未发现注释吞代码（唯一命中为 TestFramework.h:24 文档示例）；未发现字符串被注释化/
  代码被改动的可疑模式；全部用户可见文本仍走 U8()；15 文件抽查注释与代码语义一致。

### P2-4 gen_logo 自检盲区：只验证 ICO 目录偏移，不验证 PNG 可解码性

- 文件:行：`tools/gen_logo.cpp:592-611`
- 证据：本次 P0 的产物即从该自检"通过"发布。修法：自检追加独立解码（或至少用自己的 zlib 流做
  round-trip + Adler32 断言 + 一次 stb 解码像素抽查）。

### P2-5 AboutUi 的 g_logoContext 为死参数

- 文件:行：`src/app/ui/AboutUi.cpp:81,119-122`
- 证据：SetAboutGraphics 存了 context，但纹理创建只用 device（SUBRESOURCE_DATA 初始化不需要上下文，
  同 Wallpaper.cpp:164 的结论）。留着易误导后续维护。修法：删除该参数或注释说明为何不用。

---

## 已查无问题清单

1. **HeaderLayout.h 算法**：右→左装箱、victim 选择（最大 priority 先隐）、priority-0 钳制 +
   压住条目清理、两个 priority-0 不会互叠（后者被清理隐藏）、隐藏后不产生新重叠、count>16 有文档化截断；
   退化分支（全放不下→仅留 p0 钳到 leftFlowEndX）由 `header_layout_degenerate_clamp` 覆盖；fuzz
   （6 宽度集 × 5 leftEnd × 5 尺寸组 × 200..2000）全过。FlowSegmentFits 边界（恰放/超出/游标推进）正确。
2. **DrawStatusBar 两段式**：左段逐段 segFits 降级（含分隔符宽计入）、右段 [热键(2)][徽标(1)][帧耗时(0)]
   实测宽（含 | 与间距）、帧耗时恒显；窄窗热键折叠进「⋮」菜单等价开关（DrawAppearanceMenuBody narrow 分支，
   同走 ApplyHotkeyEnabled）。
3. **坐标一致性**：vendored imgui 1.92.9 的 `SameLine(offset)` 以 `window->Pos` 为原点（imgui.cpp:11644），
   与 `GetCursorPosX()`（:11684）同坐标系 —— 实测宽度定位无二次 padding 偏移（老版本语义陷阱在本仓库不存在）。
4. **壁纸 Bug2 修复链**：WallpaperDrawBackground 在 NewFrame 之后（main.cpp:507-508，V18 顺序保留）；
   DrawShell 推透明 WindowBg/ChildBg 精确包住 ##approot（Pages.cpp:2771-2775，Pop 于 End 后 :2805），
   toast/确认框/GC 模态/外观离屏预览窗在 Pop 之后绘制不受影响；WallpaperLoad 失败安全（提交点后才写副本
   与换 SRV；坏图不可能毁旧副本）；`--autotest wallpaper` PASS（DrawData 含非字体图集绘制命令=1、active=1，
   真实渲染路径实证）；退出 `WallpaperShutdown`（main.cpp:592）日志实证"保留持久化副本"；
   启动 `WallpaperAutoRestore`（main.cpp:418）→ AutoRestore 不再是死代码。
5. **关于「?」Bug3 修复链**：DrawAboutUi（含矩形哨兵）先于 BeginMenu 提交（Pages.cpp:2532-2541），
   与"菜单 SpanAvailWidth 悬停矩形遮挡、先提交者赢悬停"根因一致；AboutClickDriver 哨兵严谨
   （btnValid=本帧真实提交、真实光标+合成事件双通道、悬停确认 ≥2 帧才注入、模态 ≥4 帧、真实点击「关闭」闭环）；
   `--autotest about` PASS。
6. **内存加速可见性**：三入口（性能页顶部固定行 :1902——位于 lastTick 早退与图表显隐之前；
   内存条块 :2162；工具条 :2507——任意页签常驻）全部走 `RequestConfirmMemCleanup` → 同一
   ConfirmKind::MemCleanup → ExecuteConfirmedAction 单一 job 路径，未新建执行路径；
   TopN 选取 `SelectTopCleanupCandidates` 为纯函数（stable_sort 稳定序、systemRoot 注入可测、
   kUnavailU64 不可选、Critical/Windows/ServiceHost 排除与用户过滤同规则）；零勾选禁用执行按钮。
7. **Logo 接线**：app.rc（`1 ICON` + `200 RCDATA`，相对 src/app/ 路径正确）↔ AboutUi
   `FindResourceW(200, RT_RCDATA)` 一致；ICO 容器结构（ICONDIR/ICONDIRENTRY/256→0 编码/BMP 块
   biHeight=2h/BGRA 自底向上/AND 掩码 4 字节行距与 MSB 位序/偏移连续）除 P0 的 PNG 块内容外全部正确；
   纹理创建路径与 Wallpaper 同款（裸 SRV 直绑，1.92 无注册表）；headless：图标加载 `smokeFrames == 0`
   才执行（main.cpp:390）、托盘 `!headless`（:422）、EnsureAboutLogo 设备缺失静默、WallpaperDrawBackground
   headless 编译期剔除。
8. **实跑**：stm_selftest.exe 93/93（exit 0；含 header 5 项、memcleanup、about、wallpaper、closeaction 用例）；
   `--smoke 150` exit 0；六条 autotest 全 PASS（结果行：dialogclick"真实管线点击生效"/kill"已终止进程
   cmd.exe"/tree"终止 3 个"/startup"已禁用启动项 stm_f1_autotest"/about"模态打开并保持>=4帧，点击「关闭」
   后退出"/wallpaper"帧顶点=7088 壁纸绑定绘制命令=1 active=1"）。

## 结论

本轮四个交付面中，R-Fix 三修复（工具条结构、壁纸根因、关于「?」次序）与内存加速可见性验证成立且实测全绿；
注释中文化质量总体良好（仅 10 处续行漏翻）。**发布阻塞项为 P0-1**：Logo 资产实际损坏且工具静默报成功，
需按给定两行修法修 gen_logo.cpp 并重新生成 assets/app.ico 与 assets/logo_256.png 后复跑验证
（复验命令：用任意 stb/PNG 解码器断言 logo_256.png 与 ICO 256px 块可解码）。

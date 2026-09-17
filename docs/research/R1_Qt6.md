# R1 调研报告：Qt 6 (C++) 技术路线 —— Windows 超级任务管理器

## ① 一句话结论
**强烈推荐：Qt 6 Widgets（非 QML）+ 独立纯 C++ 采集层 + LGPL v3 动态链接合规路径 + QCustomPlot/KDChart 画曲线**，是本类数据密集型系统工具在「C++ 匹配度、开发效率、功能覆盖、长期可维护」上的最均衡选择；主要代价是便携包 50~80 MB 和 Qt Company 许可政策需持续盯紧。

## ② 八维评分表（加权总分 4.20 / 5）
| 维度 | 权重 | 得分 | 依据（一句话） |
|---|---|---|---|
| 采集层集成 | 20% | 5 | 采集层可写为无 Qt 依赖纯 C++ 库，经 QObject 适配器 + 队列信号槽/快照交接，模式成熟零障碍 |
| UI 能力与体验 | 20% | 4 | QTableView/QTreeView、系统托盘、右键菜单、悬浮窗全部一等公民且支持 10 万+行虚拟化；扣分在默认观感偏旧，需 Fusion+调色板/DWM 暗色定制 |
| 性能开销 | 15% | 4 | Model/View 只绘制可见单元格，300~800 进程 × 1Hz 全表刷新远在能力范围内；运行时内存基线约 30~60 MB（估算，待验证） |
| C/C++ 匹配度 | 15% | 5 | 原生 C++ API，与 Win32/NtQuerySystemInformation 采集代码同语言零胶水，CMake+MSVC 官方一等支持 |
| 权限与提权兼容 | 10% | 4 | Qt 本身不阻碍提权（manifest requireAdministrator 全程提权即可），托盘/菜单在提权会话下正常；无按操作 UAC 细粒度控制 |
| 生态与许可证 | 8% | 3 | 生态庞大文档最全；但 QtCharts 等 add-on 仅 GPL（传染）、LTS 补丁版商业先行，需刻意选型规避 |
| 分发体积 | 7% | 3 | windeployqt 便携目录约 50~80 MB（含 ICU 约 35 MB），LGPL 下不便静态裁剪，中等偏大 |
| 长期维护 | 5% | 4 | Qt 6.8 LTS 支持到 2029-10-08；风险点是开源用户只能拿到 LTS 初期补丁（6.8.3 之后商业先行） |

## ③ 逐问题回答

### Q1 Widgets vs QML：选 **Widgets**
- 本类需求（≥10 万行虚拟化表格、1Hz 实时曲线、进程树、右键菜单、托盘、悬浮窗）全部是 Widgets Model/View 的传统强项：QTableView/QTreeView 只实例化/绘制可见区域，10 万行无压力（官方 Model/View 文档：doc.qt.io/qt-6/model-view-programming.html）。
- QML 的 ListView/TreeView（TreeView 至 Qt 6.4 才稳定）适合触屏流式 UI，宽表格、多列排序、行级右键菜单、单元格着色等在 QML 中需大量自造轮子，且 delegate 常驻创建销毁在大表上有开销；另需多带 QtQuick/QML 运行时模块（体积更大）。
- QSystemTrayIcon、QMenu、QToolTip、无边框悬浮窗（Qt::Tool|FramelessWindowHint）在 Widgets 均开箱即用。
- 结论：主体 Widgets；图表用 QCustomPlot/KDChart（同为 Widgets 生态）。QML 仅在未来需要现代仪表盘局部时再评估。

### Q2 许可证现状（2025-2026，已查证）
- Qt 6 开源 = LGPL v3 / GPL 系双许可（qt.io/development/download-open-source；官方 LGPL 义务页：qt.io/development/open-source-lgpl-obligations）。qtbase 及常用核心模块均可 LGPL v3。
- **LGPL v3 动态链接 + 便携分发的义务**：①随附 LGPL v3 许可文本与版权声明；②声明使用了 Qt 并提供 Qt 源码/获取途径（指向 download.qt.io 即可）；③保证用户能替换 Qt 库重新链接——便携目录里 Qt DLL 与 exe 同目录天然满足；④不得静态链接 Qt（除非整个应用开源且允许重链接）。**应用自身代码可任选许可证（如 MIT），无需传染**。
- **近年变化**：Qt 6.5（2023-04）起向个人免费提供「非商业许可」（Qt Non-Commercial License）：仅限个人/非商业用途、禁止组织商用、需 Qt 账号在线安装、非 OSI 开源许可、不能替代 LGPL 做分发（对本应用无优势）。LTS 补丁版对开源用户仅开放初期若干个（6.8.0~6.8.3 开放，6.8.4+ 官方标注 commercial only，见 doc.qt.io/qt-6/qt-releases.html；6.5.4 起亦商业先行，Phoronix 有报道）；Qt 5.15 已于 2025-05-26 停止开源支持。
- **本应用（本机自用+开源）最省事路径**：LGPL v3 动态链接 + 便携目录分发 + 附 LICENSES/qt-LGPLv3 文本；不碰 QtCharts 的 GPL、不静态链接、不使用非商业许可。

### Q3 本机获取方式对比与推荐
| 方式 | 预计耗时 | 磁盘占用 | 说明 |
|---|---|---|---|
| vcpkg 源码编译 qtbase | 1~2+ 小时（估算） | 构建全程可达 30+ GB（官方讨论区：github.com/microsoft/vcpkg/discussions/32347） | 可裁剪特性、无 ICU 可瘦身，但耗时/占盘大、每次升版本重编 |
| **aqtinstall 拉官方 MSVC 二进制（推荐）** | 安装 10~25 分钟（估算） | 下载约 1 GB 级，装后约 2.5~3 GB（估算，待验证） | `pip install aqtinstall` 后：`aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtcharts`（qtcharts 仅在确认接受 GPL 时加装） |
- 版本选择：公开在线仓库（download.qt.io/online/qtsdkrepository）当前含 6.8.0~6.8.3（LTS 开源窗口）、6.9.0~6.9.3、6.10.0~6.10.3、6.11.x、6.12.0。**推荐 6.8.3**（开源可得的最后 LTS 补丁，架构 win64_msvc2022_64 = MSVC 2022 x64 v143，与本机 VS2022 17.14 ABI 完全匹配）；新特性需求可上 6.10.3。
- 长期可用性：官方归档长期保留旧版本二进制，aqt 可指定 `--base` 走清华 TUNA 镜像（mirrors.tuna.tsinghua.edu.cn/qt）加速；比 vcpkg 每次重编可控得多。
- **最快可用路径**：装 Python→pip 装 aqtinstall→上述命令→CMake 里 `find_package(Qt6 COMPONENTS Widgets REQUIRED)` + `CMAKE_PREFIX_PATH` 指向 Qt 目录，约半小时内可出 Hello World。

### Q4 表格与图表
- **1Hz 全表刷新防闪烁/防抖惯用法**：①进程行以 PID 为稳定索引，**绝不用 beginResetModel 做每秒刷新**（会闪烁+滚动位置重置）；②值变更只发**批量 dataChanged(topLeft, bottomRight, {Qt::DisplayRole})**——按"列×连续行区间"合并为少数几次信号，而非每单元格一次；③增删行按排序差分批量夹在 beginInsertRows/endInsertRows 中；④排序只在用户点列头或显式刷新时做，避免每 tick 调 QSortFilterProxyModel::invalidate()（O(n log n) 全量重排导致抖动）；⑤QTreeView 开 setUniformRowHeights(true)，列宽固定/Interactive，勿每 tick ResizeToContents；⑥数字格式化在采集/快照阶段完成，UI 侧只读字符串，800 进程×10 列×1Hz ≈ 8k 单元格/秒，model/view 轻松胜任。
- **图表库对比**：
  - **QtCharts**：仅 **GPL v3 + 商业**双许可、**无 LGPL**（doc.qt.io/qtcharts 模块页）——用了它并对外分发，整个应用必须按 GPL v3 发布，传染风险明确。
  - **QCustomPlot**：**GPL + 商业双许可**（已查证官网/论坛：GPL 默认，闭源需向作者 Emanuel Eichhammer 付费）；官网只写"GNU GPL"未标具体版本（以包内 LICENSE 文本为准，待验证），GPL 传染性与 QtCharts 同级。
  - **KDChart（KDAB）**：3.0.0（2022）起**已从 GPL 改为 MIT** 并取消商业授权，支持 Qt5/Qt6（KDAB 官方博客 kdab.com/kdchart-3-0-0/；github.com/KDAB/KDChart）——**许可证最干净**，基于 Model/View 的商务图表，API 较重。
  - 建议：曲线图需求简单（时间序列+区域填充）时**自绘 QPainter 或选 KDChart(MIT)**；若接受 GPL 传染（本项目开源则无所谓），QtCharts/QCustomPlot API 体验更好。

### Q5 采集线程与 UI 交接：**非常适合**
惯用模式：采集层为**无 Qt 依赖的纯 C++ 库/静态库**（可单测、可复用于服务/驱动模块），输出不可变快照结构（std::vector<ProcSample>，800 进程约几百 KB，拷贝廉价）；经 **QObject 适配器 + 跨线程队列信号槽**（跨线程自动 Qt::QueuedConnection；自定义类型 Q_DECLARE_METATYPE/qRegisterMetaType）或 **UI 侧 1Hz QTimer 拉取互斥锁保护快照**（最简、天然节流）交给模型；模型与上一帧 diff 后发批量 dataChanged。提权用清单 requireAdministrator 整体提权，无需拆分进程；若未来要"低权 UI+高权引擎"再用 QLocalSocket 做 IPC，Qt 支持良好。

### Q6 windeployqt 便携体积估算（估算值，待实测）
Qt6Core(~6MB)+Qt6Gui(~8MB)+Qt6Widgets(~6MB)+qwindows.dll(~2.5MB)+样式/图像格式插件(~2MB) ≈ **25~30 MB**；官方 MSVC 二进制默认链 ICU，windeployqt 会拷 icudt/icuin/icuuc 三 DLL（**约 35 MB**）→ **总量约 55~70 MB**；加 MSVC 运行时 4 个 DLL 约 +2 MB；加图表库 +1~5 MB。**结论：便携目录 50~80 MB 量级**；若自编译去 ICU 可压到 ~30-40 MB（LGPL 动态链接仍合规）。

### Q7 暗色主题、HiDPI、中文渲染
- **暗色**：Qt 6.5+ 官方支持 `app.styleHints()->setColorScheme(Qt::ColorScheme::Dark)`（官方博客 "Dark Mode on Windows 11 with Qt 6.5"），Windows 上 Qt 内部已代调 DWMWA_USE_IMMERSIVE_DARK_MODE，**原生暗色标题栏直接生效**；稳妥组合 = Fusion 风格 + 暗色 QPalette + 上述 API；第三方 QDarkStyle(MIT) 备选但通常不再必需。
- **HiDPI**：Qt 6 默认开启高 DPI 缩放（Windows 走 Per-Monitor V2），AA_EnableHighDpiScaling 等属性已废弃为默认行为，无需额外代码（doc.qt.io/qt-6/highdpi.html）。
- **中文**：Qt 6 Windows 端默认 DirectWrite 字体引擎，中文环境自动落到微软雅黑，CJK 回退开箱即用，无需 ICU 也无需额外配置；必要时 QFont 显式指定 "Microsoft YaHei"。

## ④ 依赖清单 + 许可证
| 依赖 | 版本建议 | 许可证 | 备注 |
|---|---|---|---|
| Qt 6 (qtbase+Widgets+Gui) | 6.8.3 (LTS) | LGPL v3（另有 GPL/商业） | 动态链接，随附许可文本 |
| QCustomPlot（可选曲线） | 2.1.x | GPL + 商业双许可 | 传染：分发则应用须 GPL |
| QtCharts（可选曲线） | 6.8.3 | GPL v3 + 商业（无 LGPL） | 同上传染风险 |
| KDChart（可选曲线，许可证最优） | 3.x | MIT | Qt5/Qt6 双支持 |
| ICU（随官方二进制） | — | ICU License（宽松） | 占 ~35 MB，自编译可去 |
| aqtinstall | 最新 | MIT | 仅构建期工具，不分发 |

## ⑤ 风险与降级方案
1. **QtCharts/QCustomPlot GPL 传染**：若应用想用宽松许可证发布 → 改用 KDChart(MIT) 或 QPainter 自绘曲线（工作量约数天）。
2. **LTS 补丁获取受限**（6.8.4+ 商业先行，安全修复滞后）：风险低（桌面本地工具，非远程攻击面大项）；降级 = 停留 6.8.3 并自盯公开 CVE，或跟进开源新 minor（6.10.x/6.11.x）。
3. **Qt Company 政策持续收紧**（非商业许可条款、安装器账号门槛）：aqtinstall + 归档/镜像已规避；极端情况可切 vcpkg 源码编译（慢但自主可控）。
4. **1Hz 全表刷新卡顿/闪烁**：严格执行 Q4 批量信号纪律；若仍不足，把刷新降到可见区优先（只更新视口内行）或 2Hz 半刷新。
5. **体积不满意**：vcpkg 自编译 qtbase 去 ICU/WebKit 系依赖可省 ~35-40 MB；LGPL 下不可静态链接，此项为硬下限。

## ⑥ 参考链接
- LGPL 义务：https://www.qt.io/development/open-source-lgpl-obligations
- Qt 许可总览/开源下载：https://www.qt.io/development/qt-framework/qt-licensing 、https://www.qt.io/development/download-open-source
- 版本与 LTS 支持期/商业先行策略：https://doc.qt.io/qt-6/qt-releases.html
- Qt 6.5 发布与非商业许可：https://www.qt.io/blog/qt-6.5-lts-released ；6.5.4 商业先行：https://www.phoronix.com/news/Qt-6.5.4-LTS-Out
- 公开二进制仓库（版本清单实证）：https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/
- aqtinstall：https://github.com/miurahr/aqtinstall 、https://aqtinstall.readthedocs.io/en/stable/getting_started.html
- Model/View：https://doc.qt.io/qt-6/model-view-programming.html ；部署：https://doc.qt.io/qt-6/windows-deployment.html ；HiDPI：https://doc.qt.io/qt-6/highdpi.html
- 暗色模式官方博客：https://www.qt.io/blog/dark-mode-on-windows-11-with-qt-6.5
- QtCharts 仅 GPL：https://forum.qt.io/topic/112675/what-modules-are-under-what-license
- QCustomPlot 许可：https://www.qcustomplot.com/ 、https://www.qcustomplot.com/index.php/support/forum/404
- KDChart MIT 化：https://www.kdab.com/kdchart-3-0-0/ 、https://github.com/KDAB/KDChart
- vcpkg qtbase 体积/耗时讨论：https://github.com/microsoft/vcpkg/discussions/32347

# 阶段 0 报告交叉审查 —— 审查 V1（正确性维度）

- 审查人：subagent V1（独立审查，与其他审查者互不知情）
- 日期：2026-09-18
- 审查对象：`docs/phase/00_技术选型评估报告.md` + `docs/research/R1~R6`
- 方法：对最影响决策的事实断言独立复核 —— 官方源（Microsoft Learn / GitHub Releases·源码 / doc.qt.io / qt.io / KDAB / 官方下载）逐一抓取比对；WinRing0 黑名单 XML 独立下载重算；本机只读命令复现（`cmd ver`、`netstat -ano`、`python --version`）；主报告加权分数全部手算重算。
- 结论先行：**未发现 P0（必须改）问题；发现 P1（应改）1 项、P2（建议）6 项。** 被抽查的全部决策级断言（ImGui/ImPlot 版本与许可证、WinRing0 判死证据、Qt 四项许可证断言、R5 五项架构断言、R6 三项传感器断言、加权分数计算）均经独立证据坐实。

---

## 一、发现清单

### P1（应改）

**P1-1 R5：`SystemProcessIdInformation` 信息类号写错为 `0x88`，应为十进制 88**
- 位置：R5 表A #4 行"兜底"句（`SystemProcessIdInformation(0x88) 免句柄取全路径=未文档化`）及表B #4 行（`SystemProcessIdInformation(0x88) 免句柄取路径：对 PPL/系统进程是否可行…`）。两处同错。
- 问题：该未文档化信息类号为十进制 **88**（=0x58）。`0x88` = 136，传给 `NtQuerySystemInformation` 会命中错误/未定义类号，阶段 1 按报告字面写验证代码将直接失败并可能误导排查方向。
- 证据：社区实现以 `(SYSTEM_INFORMATION_CLASS)88` 调用取进程映像路径（https://gist.github.com/TheWover/242b6037056f9e11281fc11dc1d4cc5b ）；NtDoc PROCESSINFOCLASS 枚举 `SystemProcessIdInformation = 88`（https://ntdoc.m417z.com/ntquerysysteminformation ）。
- 建议修正：两处 `(0x88)` 改为 `(88)`；可在表B #4 补一句"（类号十进制 88，勿写 0x88）"。

### P2（建议）

**P2-1 R2 §8：imgui_impl_dx11.cpp 着色器模型行号已漂移（L472/L525 → 实际 L480/L533）**
- 位置：R2 §8 首句"（imgui_impl_dx11.cpp L472/L525，已核对源码）"。
- 问题：docking 分支（1.93.0-WIP）当前 `vs_4_0` 在 L480、`ps_4_0` 在 L533。行号会随版本继续漂移；结论本身（vs_4_0/ps_4_0 → Feature Level 10.0+）经源码复核无误。
- 证据：`tar` 解包 docking 分支 tarball 后 `grep -n "vs_4_0\|ps_4_0" backends/imgui_impl_dx11.cpp` → L480/L533（2026-09-18 快照）。
- 建议：删去行号，改为"已核对源码：D3DCompile 使用 vs_4_0/ps_4_0"。

**P2-2 R1 Q4：未提及 QtCharts 已被官方标注 deprecated（Qt 6.10 起）**
- 位置：R1 Q4"图表库对比"QtCharts 条目及"长期维护"评分依据。
- 问题：doc.qt.io Qt Charts 模块页明确"deprecated since Qt 6.10，建议新项目用 Qt Graphs"。对 6.8.3 选型无影响，但 R1 以"长期维护"为维度给 Qt 打分时未计入该模块处于维护收缩轨道，属可补强的遗漏。
- 证据：https://doc.qt.io/qt-6/qtcharts-index.html （本次抓取含 "deprecated since Qt 6.10, with Qt Graphs recommended"）。
- 建议修正（Q4 QtCharts 条目末补一句）："另：QtCharts 自 Qt 6.10 起官方标注 deprecated（推荐 Qt Graphs）；6.8.3 内使用不受影响，但作为长期图表依赖已入收缩轨道，进一步支持'图表优先 KDChart(MIT)/自绘'的结论。"

**P2-3 R1 Q3：公开仓库版本枚举中的 "6.12.0" 未能证实**
- 位置：R1 Q3"版本选择"句"…6.10.0~6.10.3、6.11.x、6.12.0"。
- 问题：doc.qt.io/qt-6/qt-releases.html 当前最新发布为 **6.11.2**（支持至 2027-03-17）；"6.12.0 已在公开在线仓库"无法从官方页证实（不排除仓库先行挂出，但报告未给直接证据）。
- 证据：https://doc.qt.io/qt-6/qt-releases.html 。
- 建议：核实 download.qt.io/online/qtsdkrepository 后保留或删除"6.12.0"（推荐路径 6.8.3 不受影响）。

**P2-4 R6 §2：黑名单 XML 的引用渠道建议补官方权威入口**
- 位置：R6 §2"微软漏洞驱动黑名单"段及参考链接第 2 条（WDAC-Toolkit 仓库路径）。
- 问题：所引 `MicrosoftDocs/WDAC-Toolkit` 内文件是 WDAC Policy Wizard 的模板副本，随向导版本更新；Learn 官方页给出的权威下载渠道是 Microsoft Download Center（aka.ms/VulnerableDriverBlockList）。**引用的数据本身经我独立下载复核完全一致**（见已核无误清单），仅渠道权威性可补强。
- 证据：Learn 页 "Vulnerable driver blocklist XML" 节：https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/microsoft-recommended-driver-block-rules 。
- 建议：参考链接补"官方发布渠道：https://aka.ms/VulnerableDriverBlockList （Download Center）"，WDAC-Toolkit 路径标注"向导模板副本"。

**P2-5 R6 §6：PROCESSOR_POWER_INFORMATION 引用可加强为 Learn 官方结构页**
- 位置：R6 §6"结构体字段在微软官方 windows-rs 绑定文档中列出（CurrentMhz 等）"。
- 问题：该结构体在 Microsoft Learn 本身有官方文档页（CallNtPowerInformation 页 ProcessorInformation=11 条目内直链 `/windows/desktop/Power/processor-power-information-str`），R6 却绕道 windows-rs 绑定文档佐证"公开可依赖"，弱化了论据。
- 证据：https://learn.microsoft.com/en-us/windows/win32/api/powerbase/nf-powerbase-callntpowerinformation （ProcessorInformation 条目）。
- 建议：改为"PROCESSOR_POWER_INFORMATION 有 Learn 官方结构页（CallNtPowerInformation 页内链接），公开可依赖"。

**P2-6 主报告 §九/R5 头部："python 不可用"宜注明是商店占位别名，防阶段 1 误判**
- 位置：主报告 §九"vcpkg/pip/python 均不可用"；R5 头部环境说明。
- 问题：本机 PATH 中存在 WindowsApps 商店占位别名 `C:\Users\admin\AppData\Local\Microsoft\WindowsApps\python`，`command -v python` 能找到但执行 `python --version` 无输出、退出码 49（功能不可用）。报告结论正确，但阶段 1 若用 `command -v python`/`where python` 探测会得出"存在 python"的误判（例如尝试 aqtinstall 路径时）。
- 证据：本机实测（2026-09-18）：`command -v python` → `/c/Users/admin/AppData/Local/Microsoft/WindowsApps/python`；`python --version` → 无输出，`exit=49`；`pip`、`vcpkg` 不存在。
- 建议：措辞精确为"仅存在 WindowsApps 商店占位别名，执行退出码 49，实际不可用"。

---

## 二、已核无误清单（抽查覆盖与证据）

### 1. 主报告加权分数计算 —— 全部一致（手算重算）
权重合计 20+20+15+15+10+8+7+5 = 100，加权总分 = Σ(分数×权重)/100：
- Win32+ImGui+ImPlot：100+80+75+75+50+40+35+20 = **475 → 4.75** ✓
- Qt 6：100+80+60+75+40+24+21+20 = **420 → 4.20** ✓
- Avalonia：80+80+45+45+40+40+14+20 = **364 → 3.64** ✓
- WinUI 3：80+60+30+45+30+24+14+15 = **298 → 2.98** ✓
- Tauri：40+60+45+30+30+32+35+15 = **287 → 2.87** ✓
且与 R1/R2/R3/R4 各自评分表逐维一致，主报告转录无失真。

### 2. ImGui 版本/1.92 破坏性变更/docking 状态（R2、主报告）—— 全部属实
- 最新稳定版 **v1.92.9b（2026-07-31T14:33Z）**；其后无更新 tag（GitHub Releases API 实查：v1.92.9=2026-07-25、v1.92.8=2026-05-12…）。
- docking 分支当前 `#define IMGUI_VERSION "1.93.0 WIP"`（imgui.h L32，本次解包 tarball 实查）；docking 独立分支存在、未并入主线。
- v1.92.0（2025-06-25）release notes 原文核实：①"Fonts may be rendered at any size. Glyphs are loaded and rasterized dynamically."（动态字体）②API 改用 `ImTextureRef` ③后端纹理协议 + `ImGuiBackendFlags_RendererHasTextures` ④"THIS VERSION CONTAINS THE LARGEST AMOUNT OF BREAKING CHANGES SINCE 2015!"。与 R2/主报告"2015 年以来最大变更"表述一致。
- `ImGuiListClipper` 注释原文 "lists with tens of thousands of items without a problem" 在 docking 源码 imgui.h L3038 实查属实（R2 的"10 万行可行"是对该口径的外推，未失实）。

### 3. ImPlot 许可证与最新版本（R2、主报告）—— 全部属实
- 最新 release **v1.0（2026-04-05）**，引入 ImPlotSpec 样式 API（GitHub releases 实查）；仓库 LICENSE 文件为 **MIT**（"Copyright (c) 2020 Evan Pezent"，本次下载实查）。
- README FAQ 原文 "You can plot tens to hundreds of thousands of points without issue" 实查属实；"experimental backends branch（GPU 加速）"亦在 README 中。

### 4. WinRing0 漏洞驱动黑名单（R6、主报告 §五）—— 证据扎实，独立复核数字完全一致
本次独立下载 XML（raw.githubusercontent.com/MicrosoftDocs/WDAC-Toolkit/.../Recommended_Driver_Blocklist.xml）重算：
- 文件 525,766 字节（R6 称 525KB ✓）；`<Deny ` 计 **1701** 条（R6 称 1701 ✓）；含 "WinRing0"（忽略大小写）的 Deny 条目 **29** 条（R6 称 29 ✓）；
- FileAttrib 全版本规则原文实存：`<FileAttrib ID="ID_FILEATTRIB_WINRING0" FriendlyName="WinRing0.sys" FileName="WinRing0.sys" MinimumFileVersion="0.0.0.0" MaximumFileVersion="2.0.0.0" />`（R6 引文一字不差 ✓）；
- Learn 官方机制页原文核实：**"Since the Windows 11 2022 update, the vulnerable driver blocklist is enabled by default for all devices"**（=22H2 起默认启用 ✓）；"The blocklist is updated quarterly…delivered through the monthly Windows updates"（季度刷新 ✓）；"enforced when either memory integrity (HVCI), Smart App Control, or S mode is active"（强制场景 ✓）；"HVCI is on by-default for most new Windows 11 devices"（✓）；
- Defender 官方检测页 VulnerableDriver:WinNT/Winring0 存在、链接 NVD CVE-2020-14979、明言 "This detection is valid"（✓）。
结论：主报告"WinRing0 判死证据（R6 实锤）"成立。

### 5. Qt 许可证四连（R1、主报告）—— 逐条属实
- **LGPL v3 动态链接义务**：qt.io 官方 LGPL 义务页核实（提供许可文本与版权声明、提供源码或获取途径、允许修改并重链接、动态链接方可使应用代码保持专有）——与 R1 Q2 四点归纳相符（R1 的"应用自身代码可任选许可证"是"动态链接且构成 work that uses the library"前提下的简化表述，方向正确）。
- **QtCharts 仅 GPL**：doc.qt.io 模块页原文 "available under commercial licenses from The Qt Company" + "GNU General Public License, version 3"，无 LGPL（✓，R1 称"GPL v3+商业、无 LGPL"准确）。
- **QCustomPlot GPL+商业**：官网原文仅写 "GNU GPL"（未标版本）+ "Please get in contact if you need a commercial license"——与 R1 描述（含"官网只写 GNU GPL 未标具体版本"这一细节）一字不差地吻合（✓）。
- **KDChart 3.x MIT**：KDAB 官方博客（2022-09-01）原文 "We've relicensed KDChart from the GPL to the MIT license and removed our commercial offering"，Qt5/Qt6 双支持（✓）。
- 附加核实：Qt 6.8 LTS 标准支持至 **2029-10-08**（qt-releases 页 ✓，主报告"支持到 2029"与 R1 精确日期均属实）；6.8.8 标注 "LTS, commercial only"，开源用户仅获 LTS 初期补丁（✓，支持 R1"6.8.4+ 商业先行"的实质判断）。

### 6. R5 架构级断言 —— 逐条与官方原文吻合
- **NtQSI(SystemProcessInformation)**：Learn 页（页面 updated_at 2025-08-27）含完整 `SYSTEM_PROCESS_INFORMATION` 布局（`Reserved1[48]`/`Reserved7[6]` 等，CPU/IO 时间字段确实未点名）+ 页首 "[NtQuerySystemInformation] may be altered or unavailable in future versions" 警告 + "must use LoadLibrary/GetProcAddress dynamically link to Ntdll.dll"。R5"函数+结构布局已上 Learn、页首警告、CPU/IO 偏移靠社区"三段表述全部属实。
- **SystemBasicProcessInformation**：Learn 原文 "Available as of Windows 11 version 26100.4770"，`SYSTEM_BASICPROCESS_INFORMATION` 含 `SequenceNumber`，官方明言"用于检测 UniqueProcessId 复用（替代 CreateTime）"。R5/主报告表述属实。
- **StartupApproved**：R5 明确标注"未文档化（官方无页）+ 社区一致"，此定性诚实且正确；12 字节、首字节 0x02=启用/0x03=禁用、字节 4-11 为 FILETIME 的语义与社区取证来源（windowsir.blogspot.com Harlan Carvey 系列、TenForums 等）一致。R5 将"写入后 Explorer 是否识别"列入表B 待验证而非当作定论——处理方式恰当。
- **UWP StartupTask State**：Learn `StartupTaskState` 枚举页实查：Disabled=0、DisabledByUser=1、Enabled=2、DisabledByPolicy=3、EnabledByPolicy=4，与 R5 #14d 引用完全一致（枚举文档化、注册表映射非官方的定性也正确）。
- **EnumDeviceDrivers 24H2**：Learn 原文 "Starting in Windows 11 Version 24H2, EnumDeviceDrivers will require SeDebugPrivilege to return valid ImageBase values. The function will still succeed… but the returned lpImageBase array will contain addresses that are all NULL." —— R5"调用成功但地址全 NULL（文档原文）"属实。
- **GetExtendedTcpTable(OWNER_PID)**：Learn 页无任何管理员特权要求（仅返回码说明），"普通权限可取 OWNER_PID 表"的定性成立；OWNER_MODULE 非管理员空串之说在 `GetOwnerModuleFromTcpEntry` 页有原文："If the GetOwnerModuleFromTcpEntry function is called by a user that is not a member of the Administrators group, the function call will succeed but the pModuleName and pModulePath members will point to… an empty string for the TCP connections started by protected applications"，且需 manifest requireAdministrator——与 R5 #10 引文一致。

### 7. R6 传感器断言 —— 三项全部属实
- **MSAcpi_ThermalZoneTemperature**：猜测的 Learn URL（windows/win32/wmicoreprov/msacpi-thermalzonetemperature）本次抓取返回 404，R6"无正式文档页"的结论成立；"非管理员查询被拒/常无有效值"与本机类已注册但拒绝访问的实测及社区证据一致。
- **CallNtPowerInformation(ProcessorInformation=11)**：Learn 函数页文档化，"lpOutputBuffer receives one PROCESSOR_POWER_INFORMATION structure for each processor"，无管理员要求——R6"官方文档化、免管理员"属实。
- **NVMe Health log**：Learn "Working with NVMe Drives" 页（Applies to: Windows 10/Server 2016）文档化 `IOCTL_STORAGE_QUERY_PROPERTY` + `StorageDeviceProtocolSpecificProperty` + `ProtocolTypeNvme` + `NVMeDataTypeLogPage`（SMART/health data，示例用 `NVME_LOG_PAGE_HEALTH_INFO` 即 Get Log Page 0x02），另载 `IOCTL_STORAGE_PROTOCOL_COMMAND`（Windows 10 引入）——R6"Windows 10 起文档化"属实。

### 8. R3 抽查（WASDK 生命周期）—— 与 release-channels 页一致
稳定版 **2.5.1（2026-09-16 发布）**、2.0 发布 2026-04-29 且 End of servicing 2027-04-29、1.8 于 2026-09-09 出保、1.7/1.6 Out of Support、Modern Lifecycle、"需始终使用最新补丁"、兼容 Win10 1809 —— R3 §2 各项全部吻合。

### 9. 本机复核（只读命令）
- `cmd ver` → 10.0.**22631.3296**（主报告 §九一致）；
- `netstat -ano` 非管理员成功输出 PID 列（主报告 §九/R5 实测复现）；
- `pip`/`vcpkg` 不存在；`python` 为商店占位别名、退出码 49（见 P2-6）。

---

## 三、未覆盖范围（诚实声明）
以下未逐一复核（决策影响小或需重装/管理员操作），不构成通过与否的依据：
- R3 的 GitHub issue 逐条核对（#896/#6268/PR #2066 等）；R4 的 sysinfo 0.39 Process API 逐字段实测、Tauri/WebView2 内存数字；R1 的 download.qt.io 在线仓库版本全枚举（见 P2-3）；R6 的 IGCL/ADLX 许可 PDF 条款细节、NVML 再分发 EULA（R6 已自行标注待验证）；主报告中 PDH/传感器"本机实测"数据点未逐项重跑（仅复现 netstat/OS build/python 三项）。

## 四、总体评价
七份文档的被抽查事实断言与官方源吻合度极高（含多处"文档原文"级引用，逐字核对无误）；版本号、日期、枚举值、许可证条款均经受住独立复核。唯一实质错误是 P1-1 的信息类号进制笔误，建议随 P2 各项一并修订后定稿。

# R6 硬件传感器监控调研 —— 传感器逐项可达性矩阵（方案A 用户态 / 方案B 内核驱动）

调研日期：2026-09-18 ｜ 目标：Win10 21H2+/11 x64 ｜ 本机实测环境：Win11 23H2 zh-CN，非管理员 shell，16 逻辑核 Intel Iris Xe，SATA SSD，无独立显卡

## ① 一句话结论

方案A（用户态 + 管理员）可全覆盖 CPU 频率/占用、SMART 磁盘健康、GPU 温度/频率/占用（各厂商 SDK，依赖其驱动安装）；CPU 温度只能拿到 ACPI 热区（消费级台式机普遍拿不到有效值）；**每核 CPU 温度与风扇转速用户态不可达**，必须方案B——但现成内核驱动 WinRing0 已进入微软官方漏洞驱动黑名单（本调研直接在其 XML 中 grep 命中 29 条 Deny 哈希）并被 Defender 报毒，方案B 只能走 PawnIO 或自研签名驱动，且不得随包分发。

## ② 传感器可达性矩阵（核心）

| 传感器 | 方案A 最佳实现 | 方案A可达性 | 所需权限 | 方案B 补齐 |
|---|---|---|---|---|
| CPU 温度（整包） | WMI `root\wmi\MSAcpi_ThermalZoneTemperature`（单位 0.1K） | **部分**：笔记本/OEM 常见；台式机主板普遍缺失或恒值（社区证据+本机类存在但非管理员被拒） | 管理员（本机实测非管理员=拒绝访问） | 每核 DTS 温度（MSR） |
| CPU 每核温度 | 无文档化用户态 MSR 路径（RDMSR 为 ring0 指令，无官方 API） | **不可达** | — | WinRing0/PawnIO/自研驱动读 IA32_THERM_STATUS(0x19C)/IA32_TEMPERATURE_TARGET(0x1A2) |
| CPU 频率（实时） | `CallNtPowerInformation(ProcessorInformation)`（CurrentMhz/MaxMhz/MhzLimit）+ PDH `% Processor Performance` / `% of Maximum Frequency` | **全** | 免管理员（本机实测 hr=0、PDH 正常） | — |
| CPU 占用 | `GetSystemTimes` / PDH `Processor Information\% Processor Time` | **全** | 免管理员 | — |
| GPU 温度/频率/占用/风扇 | NVIDIA：NVML（nvml.dll 随驱动）；AMD：ADLX；Intel：IGCL；占用/显存另有 PDH `GPU Engine` 计数器 | NVIDIA/AMD/Intel 各自**全**（前提：装了对应厂商驱动；无对应硬件则该项不可达） | 免管理员（厂商 SDK 用户态） | —（方案A已够） |
| 磁盘 SMART/健康/温度 | `IOCTL_STORAGE_QUERY_PROPERTY`+`StorageDeviceProtocolSpecificProperty`（NVMe Get Log Page 0x02；ATA 协议路径）；兜底 `IOCTL_SCSI_PASS_THROUGH`；简化版 `MSFT_PhysicalDisk`+`MSFT_StorageReliabilityCounter` | **全**（完整 SMART）；简化版**部分**（只有 Healthy/Warning 级别） | 管理员（本机实测 StorageReliabilityCounter 非管理员拒绝访问） | —（方案A已够） |
| 风扇转速（主板/CPU） | 无用户态文档化途径：SuperIO 需直接端口 IO（0x2E/0x4E），用户态 in/out 被内核拦截；无文档化风扇 WMI 类 | **不可达** | — | 内核驱动读 SuperIO 寄存器（LHM/PawnIO 路径） |

## ③ 逐项查证详情

### 1. CPU 温度
- `MSAcpi_ThermalZoneTemperature`：**Microsoft Learn 无该类的正式文档页**（learn 搜索 API 仅命中社区 Q&A 与 Win32_TemperatureProbe；猜测 URL wmicoreprov/msacpi-thermalzonetemperature 返回 404）。类由 ACPI 驱动实现，属性 CurrentTemperature/CriticalTripPoint 单位为 0.1K（开尔文）。本机实测：类已注册（Get-CimClass 可见），非管理员查询报"拒绝访问"；管理员下是否返回有效值**待验证**。
- 支持率证据：StackOverflow #9101295 与 Reddit r/PowerShell 均记录该类"经常返回静态值或不暴露，消费级台式机尤其不可靠"；它反映的是 ACPI 热区而非 CPU 核心温度，通常每板只有 1~2 个 zone。
- 每核温度为何必须内核驱动：每核温度来自 MSR（IA32_THERM_STATUS 0x19C、DTS 相关 IA32_TEMPERATURE_TARGET 0x1A2），RDMSR/WRMSR 是特权指令，Windows **没有文档化的用户态 MSR 通道**（不存在官方 NtWx/DeviceIoControl 路径）；WinRing0 正是靠"用户态→IOCTL→内核 rdmsr"实现。旧版利用 \Device\PhysicalMemory 的路径在现代 Windows 已被内核封锁。
- Intel DPTF/Thermal Zone：无公开的用户态温度 API 文档（其 WMI/接口为 OEM/驱动私有，待验证），不能作为可靠途径。

### 2. WinRing0 驱动与安全态势（方案B 的核心风险）
- CVE：CVE-2020-14979——WinRing0.sys/WinRing0x64.sys 1.2.0（随 EVGA Precision X1）NULL DACL + 未文档化 IOCTL，任意低权进程可映射 \Device\PhysicalMemory 提权到 SYSTEM；CVE-2020-14980——同一驱动内核态任意代码执行 LPE（NVD 均有条目）。微软官方 Defender 检测页 VulnerableDriver:WinNT/Winring0 明确引用 CVE-2020-14979，且声明"非误报"。
- **微软漏洞驱动黑名单**：本调研直接下载微软 Recommended_Driver_Blocklist.xml（525KB，1701 条 Deny）并 grep：命中 WinRing0 系列 29 条哈希 Deny（WinRing0.sys、WinRing0x64.sys、WinRing0_1_2_2.sys、WinRing0a64.sys、iFlyWinRing0x64.sys），另有 FileAttrib `FileName="WinRing0.sys" MinimumFileVersion="0.0.0.0" MaximumFileVersion="2.0.0.0"`（全版本特征）。官方文档确认：黑名单自 Win11 22H2 起**对所有设备默认启用**、随月度更新季度刷新；在 HVCI/Smart App Control/S 模式激活时**强制执行**；"HVCI 在大多数新 Win11 设备默认开启"。命中后加载失败表现为 0xC0000603 (STATUS_INVALID_IMAGE_HASH)/服务启动失败。
- 本机实测（2026-09-18）：`Win32_DeviceGuard` VirtualizationBasedSecurityStatus=0、SecurityServicesRunning={0}；HVCI scenario 注册表键不存在；UEFISecureBootEnabled=0；`HKLM\...\CI\Policy\VerifiedAndReputablePolicyState=2`（0=Off/1=On/2=评估，社区确认值含义；本机为评估模式）。结论：本机当前可加载 WinRing0，但**不能假设用户机器如此**——新 Win11 默认 HVCI 设备将直接加载失败。
- 杀软/SAC：2025 年初起 Defender 将 WinRing0x64.sys 报为 HackTool:Win32/Winring0 / VulnerableDriver，影响 FanControl/OpenRGB/MSI Afterburner/LHM 等；**添加杀软排除项只能消除 AV 报警，不能解除内核黑名单对加载的拦截**。SAC 开启时也会拦截（SAC 隐含强制黑名单）。
- 行业去向：LHM 官方讨论确认正迁移到 **PawnIO**（数字签名、只跑经审计的 Pawn 脚本模块的受限内核驱动）；FanControl v239 已完全弃用 WinRing0、改为提示用户单独安装 PawnIO。→ 方案B 若做，PawnIO 是当前最低风险路径；自研驱动则需 EV 代码签名+WHQL，成本高且驱动不随包分发符合产品决策。

### 3. LibreHardwareMonitor（sidecar 集成可行性）
- 许可证：**MPL-2.0**（GitHub LICENSE 确认；部分第三方组件另列 notices）。MPL-2.0 为文件级 copyleft：作为外部 sidecar 引用其库/进程，专有宿主可行，修改其源文件需开源。
- 集成接口：GUI 自带 **Remote Web Server**（Options 启用，默认端口 8085，`http://localhost:8085/data.json` 输出 JSON，Home Assistant 官方集成即基于此）；README 未提供 CLI。v0.9.4 曾有 web server 启动回归（Issue #1855）。两种 sidecar 形态：a) 直接跑 LHM GUI 开 web server 轮询 data.json——实现最快但多带一个 GUI；b) 用 LibreHardwareMonitorLib 自写无界面 sidecar exe，轮询间隔自控（推荐）。实时性：由其内部更新定时器/我们自定轮询决定，1~2s 粒度足够任务管理器。
- 关键限制：LHM 读取每核温度/风扇/电压同样依赖内嵌 WinRing0（打包为 LibreHardwareMonitorLib.sys，被 Defender 报毒见 Issue #1660/#1844），新版本在向 PawnIO 迁移（Discussion #2149）。→ **sidecar 方案同样继承方案B 的全部风险**，本质上是"方案B 的分发外包给 LHM/PawnIO"。

### 4. GPU（用户态、免驱动内嵌）
- NVIDIA NVML：官方文档 docs.nvidia.com/deploy/nvml-api（nvmlDeviceGetTemperature、nvmlDeviceGetFanSpeed、nvmlDeviceGetClockInfo、nvmlDeviceGetUtilizationRates 等全部所需 API）；nvml.dll **随显卡驱动分发**（装了 N 卡驱动才存在；本机无 N 卡，System32 无 nvml.dll/nvapi64.dll，符合预期）；头文件官方渠道为 CUDA Toolkit（NVIDIA/gpu-monitoring-tools 镜像已归档，Apache-2.0）；nvml.dll 随应用再分发的 EULA 条款**待验证**，稳妥做法是直接加载驱动自带的 System32\nvml.dll。NVAPI：官方开源头 NVIDIA/nvapi + nvapi-open-source-sdk 可下载，但仅 DRS 子集文档化，温度/频率 ID 半公开；NVML 优先。
- AMD ADLX：GPUOpen-LibrariesAndSDKs/ADLX，仓库根为 **"ADLX SDK License Agreement.pdf" 专有许可（GitHub license API 返回 404/无 OSI 许可；并非社区传言的 MIT）**；SDK（头+样例）可下载，含 PerformanceMonitoring（温度/频率/利用率/风扇）与 GPU Tuning；运行时随 AMD 驱动分发、需 ADLX 兼容驱动。
- Intel IGCL：intel/drivers.gpu.control-library，LICENSE 解码为 **"Intel Software License Agreement 10.07.21"（GitHub 标 Other/NOASSERTION，非 MIT）**——允许使用与再分发但"soldly for use on Intel platforms"（仅限 Intel 平台）；README 确认 Engine/Fan/Telemetry/Frequency/Memory/Power/Temperature API 均有（64-bit 限制），二进制随 Intel 显卡驱动包分发。本机 System32 无 igcl64.dll（实际安装位置待验证，可能在 DriverStore）。
- 本机兜底实测：PDH 英文路径 `\GPU Engine(*engtype_3D)\Utilization Percentage`、`\GPU Adapter Memory(*)\Dedicated Usage` 经 PdhAddEnglishCounterW 返回 hr=0（Win10 1709+ 文档化性能计数器）→ GPU 占用/显存有免厂商 SDK 的用户态途径；但 GPU **温度**没有文档化性能计数器，必须走厂商 SDK。

### 5. SMART / 磁盘健康
- NVMe：微软官方文档 "Working with NVMe drives"：`IOCTL_STORAGE_QUERY_PROPERTY` + `StorageDeviceProtocolSpecificProperty` + `ProtocolTypeNvme` + `NVMeDataTypeLogPage`（Get Log Page **0x02** SMART/Health）——Windows 10 起文档化；另有 `IOCTL_STORAGE_PROTOCOL_COMMAND` 直发。返回字段含 Composite Temperature/Warning/Critical/可用备件百分比/已用寿命等。
- SATA：ATA 协议路径（StorageDeviceProtocolSpecificProperty 的 ATA 分支发 IDENTIFY/SMART）或 `IOCTL_SCSI_PASS_THROUGH` 直发 ATA SMART 命令；后者需 GENERIC_READ/WRITE 句柄 → **管理员**；0 权限句柄能否做纯查询**待验证**。CrystalDiskInfo `AtaSmart.cpp` 旁证：SMART_RCV_DRIVE_DATA/SMART_SEND_DRIVE_COMMAND、SCSI pass-through（SAT/USB 桥）、NVMe 走 SendNvmeQueryPropertyCommand，与我们设计一致。
- 简化替代：`root\Microsoft\Windows\Storage` 的 `MSFT_PhysicalDisk`（官方 drivers/storage 文档，HealthStatus=Healthy/Warning/Unhealthy）+ `MSFT_StorageReliabilityCounter`（Temperature/Wear/PowerOnHours 等，官方类文档存在）。局限：HealthStatus 只给粗粒度结论，拿不到具体 SMART 属性值。本机实测：`Get-PhysicalDisk` 正常（SATA SSD，Healthy）；`Get-StorageReliabilityCounter` 非管理员报 PermissionDenied → **需管理员**。

### 6. CPU 频率与占用
- `CallNtPowerInformation(ProcessorInformation)`：官方文档（powerbase.h，CallNtPowerInformation 函数页），lpOutputBuffer 为每逻辑核一个 `PROCESSOR_POWER_INFORMATION`（Number/MaxMhz/CurrentMhz/MhzLimit/MaxIdleState/CurrentIdleState）；结构体字段在微软官方 windows-rs 绑定文档中列出（CurrentMhz 等），**公开可依赖**。本机实测 hr=0：16 核 CurrentMhz=2700/MaxMhz=2700，免管理员。
- PDH：本机中文系统实测 `PdhAddEnglishCounterW` 全部成功（解决中文计数器名问题）：`% Processor Performance`=140、`Processor Frequency`=2700、`% of Maximum Frequency`=100、`Processor Information\% Processor Time`=9 → 频率相对值与绝对值、占用率全覆盖，免管理员。

### 7. 风扇转速
- SuperIO 芯片（ITE/Nuvoton/Winbond 等）寄存器通过 index/data I/O 端口（0x2E/0x4E+0x2F/0x4F）访问，用户态 in/out 被内核拦截（无 HAL 文档化例外）；ACPI 热区不提供转速；WMI 无文档化风扇转速类 → **方案A 不可达，如实标注"需要驱动支持"**；方案B（内核驱动读 SuperIO，即 LHM/PawnIO 的做法）为唯一途径。笔记本 EC 风扇同理。GPU 风扇转速不受此限（走 NVML/ADLX/IGCL）。

## ④ 方案A/B 覆盖对比与默认建议

| 维度 | 方案A（用户态，默认） | 方案B（内核驱动，可选、不随包分发） |
|---|---|---|
| 覆盖 | CPU 频率/占用、GPU 温度/频率/占用/风扇（按厂商 SDK）、SMART 全量 | +每核 CPU 温度、主板/CPU 风扇转速、电压 |
| 权限 | 频率/占用免管理员；热区温度、SMART 需管理员 | 驱动加载需管理员+签名 |
| 风险 | 无内核风险 | WinRing0=黑名单+Defender+CVE(2020-14979/14980)；PawnIO=签名但需用户单独安装；自研=WHQL 成本 |
| 失效面 | 新 Win11 默认 HVCI 不影响方案A | HVCI/黑名单/SAC 设备上 WinRing0 直接加载失败（0xC0000603） |

**默认建议**：方案A 实现"管理员可拿到的全部"（SMART、GPU、频率、占用、ACPI 热区温度）；CPU 每核温度与主板风扇在方案A 下显示"需要驱动支持（可选安装组件）"，绝不显示假数据/0 占位；方案B 以 PawnIO 为候选内核通道（或自研签名驱动），单独可选安装并在 UI 明示风险；GPU 路径按显卡型号动态探测 NVML→ADLX→IGCL，无对应 SDK 时该项显示不可用而非隐藏整卡。

## ⑤ 参考链接

- 微软漏洞驱动黑名单机制与默认启用：https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/design/microsoft-recommended-driver-block-rules
- 黑名单 XML（本调研 grep 的原始文件，含 WinRing0 29 条 Deny）：https://github.com/MicrosoftDocs/WDAC-Toolkit/blob/main/WDAC-Policy-Wizard/app/Resources/policyTemplates/Recommended_Driver_Blocklist.xml
- Defender VulnerableDriver:WinNT/Winring0 官方页（引用 CVE-2020-14979）：https://support.microsoft.com/en-us/windows/security/threat-malware-protection/microsoft-defender-antivirus-alert-vulnerabledriver-winnt-winring0
- CVE-2020-14979：https://nvd.nist.gov/vuln/detail/CVE-2020-14979 ；分析 https://medium.com/@matterpreter/cve-2020-14979-local-privilege-escalation-in-evga-precisionx1-cf63c6b95896
- LHM WinRing0 被拦截 Issue #1660：https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/issues/1660 ；PawnIO 迁移 Discussion #2149：https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/discussions/2149
- LHM 仓库（MPL-2.0）：https://github.com/LibreHardwareMonitor/LibreHardwareMonitor ；Web Server/data.json 实践：https://www.home-assistant.io/integrations/libre_hardware_monitor/
- FanControl v239 迁移 PawnIO：https://github.com/Rem0o/FanControl.Releases/issues/3480
- MSAcpi_ThermalZoneTemperature 支持面社区证据：https://stackoverflow.com/questions/9101295/msacpi-thermalzonetemperature-class-not-showing-actual-temperature
- NVML 官方文档：https://docs.nvidia.com/deploy/nvml-api/ ；NVAPI 开源头：https://github.com/NVIDIA/nvapi
- ADLX（专有许可 PDF）：https://github.com/GPUOpen-LibrariesAndSDKs/ADLX ；IGCL（Intel EULA）：https://github.com/intel/drivers.gpu.control-library
- NVMe 健康日志官方文档：https://learn.microsoft.com/en-us/windows/win32/fileio/working-with-nvme-devices
- MSFT_PhysicalDisk：https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/msft-physicaldisk
- CallNtPowerInformation：https://learn.microsoft.com/en-us/windows/win32/api/powerbase/nf-powerbase-callntpowerinformation
- CrystalDiskInfo AtaSmart.cpp（实现旁证）：https://github.com/hiyohiyo/CrystalDiskInfo/blob/master/AtaSmart.cpp

### 本机实测数据附录（2026-09-18，Win11 23H2 zh-CN，非管理员 shell）
- MSAcpi_ThermalZoneTemperature：类已注册（Get-CimClass 命中），查询报"拒绝访问"（需管理员）
- VBS/HVCI：全关（SecurityServicesRunning={0}，HVCI 键不存在）；SecureBoot=0；SAC=2（评估模式）
- PDH 英文计数器：5 条测试路径全部 OK（含 % Processor Performance=140、Processor Frequency=2700）
- CallNtPowerInformation(11)：hr=0，16 核 CurrentMhz=2700（免管理员）
- 磁盘：HYS512 SATA SSD，HealthStatus=Healthy；StorageReliabilityCounter 非管理员被拒
- GPU：Intel Iris Xe（31.0.101.5445）；System32 无 nvml.dll/nvapi64.dll/igcl64.dll；PDH GPU Engine 计数器可用（hr=0）
- driverquery：无 WinRing0 系驱动安装

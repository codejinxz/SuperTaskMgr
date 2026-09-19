# 11 · 内核能力调研：MSR 温度 / SuperIO 风扇电压 / 真抓包（D3，2026-09-20）

用户已明确授权"支持调用内核，只要能力达得到"。目标能力：
①CPU 每核 DTS 温度（MSR）；②主板风扇转速/电压（SuperIO 芯片端口 IO）；③真·抓包含载荷。
本轮只做调研与本机只读探测 + 尖峰代码（**未安装任何驱动/软件**）；安装与实测在下一轮经用户确认后进行。

## 0. 本机实测快照（只读探测，2026-09-20）

| 项目 | 结果 | 检测方式 |
|---|---|---|
| CPU | 12th Gen Intel(R) Core(TM) i5-12600H（Alder Lake-H，20 线程） | CPUID 品牌叶（KernelProbe::CpuBrand） |
| PawnIO | **未安装**（无 Services\PawnIO、无 %ProgramFiles%\PawnIO\PawnIO.sys） | 注册表+文件属性 |
| Npcap | **未安装**（无 NPCAP/npf 服务、无 wpcap.dll） | 注册表+文件属性 |
| 测试签名 | 未开启（SystemStartOptions 无 TESTSIGNING） | 注册表 |
| HVCI | 未运行（SecurityServicesRunning={0}，VBS=0） | WMI+注册表 |
| 漏洞驱动阻止名单 | 开启（VulnerableDriverBlocklistEnable=1，Win11 默认；HVCI 关闭时按 cert 基线执行） | 注册表 |
| OS | Windows 11 23H2（22631） | — |

尖峰验证（build_d3\kernel_probe_spike.exe，/std:c++20 /W4 /permissive- /utf-8，0 error 0 warning）：
4/4 PASS；`PawnIoTryGetVersion` 在未安装态温和返回 NTSTATUS **0xC0000034**（对象不存在），
证明 `\Device\PawnIO` + NtOpenFile + IOCTL 调用约定链路编译与运行均正确。
代码：`src/collect/KernelProbe.h/.cpp`、`src/selftest/kernel_probe_test.cpp`（stm_collect/stm_selftest 按目录 glob 自动纳入）。
参考快照：`build_d3/_d3_refs/`（PawnIOLib.h、pawnio_um.h、IntelMSR.p、LpcIO.p 等）。

## 1. 三条路线

### 路线 1：PawnIO（github.com/namazso/PawnIO；下载站 pawnio.eu）
- **是什么**：开源（GPL-2.0）可脚本化内核驱动，用户态加载**已签名 Pawn 字节码模块**进 ring0 沙箱 VM 执行，
  专为替代漏洞驱动的 WinRing0 而生。FanControl（V238 起内置）与 LibreHardwareMonitor（0.9.6+ 自动选用）、
  UXTU 均已采用。
- **安装要求**：官方签名安装包 `PawnIO_setup.exe`（github.com/namazso/PawnIO.Setup/releases，最新 2.2.0，3.4MB；
  winget 无官方包）。装一次，创建服务 PawnIO（设备 `\Device\PawnIO`），管理员权限安装，随后普通用户可调用。
- **签名与阻止名单风险**：官方版与开源代码一致、数字签名；**与 HVCI/内存完整性和 Secure Boot 兼容**
  （WinRing0 正是过不了 HVCI 才被淘汰）；不在微软漏洞驱动阻止名单（反例：WinRing0 被 Defender 报毒/拦截）。
- **C++ 调用方式**（已从源码核实，见 _d3_refs）：
  - 可完全自写最小 IOCTL（推荐，零新增依赖；LGPL 的 PawnIOLib.dll 是可选而非必须）：
    - 打开：`NtOpenFile(L"\Device\PawnIO")`（无 DOS 符号链接；ntdll 动态绑定，尖峰已实现）；
    - `IOCTL_PIO_LOAD_BINARY` = CTL_CODE(**41394**, **0x821**, METHOD_BUFFERED, FILE_ANY_ACCESS)，入参=模块 blob；
    - `IOCTL_PIO_EXECUTE_FN` = CTL_CODE(41394, **0x841**, ...)，入参 = `char[32]` 零填充函数名 + `UINT64 参数[]`，
      出参 = `UINT64[]`；
    - `IOCTL_PIO_VERSION` = CTL_CODE(41394, 0x861, ...)。
  - 官方模块（LGPL-2.1，pawnio.eu 下发已签名 blob，自定义模块需作者私钥签名=不可行，用官方即可）：
    - **IntelMSR**：`read_msr`（in=[msr]，out=[value]）——白名单只读寄存器含
      0x19C IA32_THERM_STATUS（每核 DTS）、0x1A2 TEMPERATURE_TARGET（TjMax）、0x198 PERF_STATUS 等；
      写操作单独白名单（我们**只用读**）；
    - **LpcIO**：`select_slot`（0x2E/0x4E）→ `find_bars`（SuperIO BAR 探测）→ 端口 `read_byte`/`write_byte`
      （端口白名单=已发现的 BAR，PCI 配置口 0xCF8-0xCFF 黑名单）——风扇转速/电压所需；
      官方要求先取全局互斥体 `\BaseNamedObjects\Access_ISABUS.HTP.Method`（与 LHM 同规）。
- **能拿到的能力**：每核 DTS 温度、TjMax、RAPL 能耗、风扇转速、主板电压、EC（部分厂商）。
- **成本**：用户一次性安装一个签名驱动；应用侧约 1 个 .h/.cpp（协议已尖峰验证）。几乎零维护。
- **许可证要点**：驱动 GPL-2.0 + **特殊例外**——"仅通过设备 IOCTL 通信的独立程序"不构成衍生作品（闭源可用）；
  模块与 PawnIOLib 为 LGPL-2.1；商业替代许可可联系作者。**我们不分发 PawnIO 本体，由用户自行安装**（更稳）。

### 路线 2：自研测试签名驱动 —— **不推荐**（附完整成本）
- **安装要求**：WDK（VS2022 BuildTools 已具备编译环境，WDK ≈1-2GB）+ 自签证书；
  `bcdedit /set testsigning on` **并重启**；桌面右下角永久"测试模式"水印。
- **签名/安全硬性冲突**：测试签名驱动要求 **Secure Boot 关闭**；HVCI（若用户开启）直接拒绝加载；
  Smart App Control 开启时拦截；裸 MSR+任意端口 IO 驱动正是微软"推荐驱动阻止规则"要封的形态
  （WinRing0 同类，AV 厂商标记为漏洞驱动，信誉随时间恶化）。
- **需要写的最小驱动规模**：WDM 骨架（DriverEntry/IRP_MJ_DEVICE_CONTROL 分发）+ `__readmsr`
  白名单 + `__inbyte/__outbyte` 端口读写 ≈ 300-600 行，1-2 个开发日；但**长期成本**在签名、
  每次 Win 更新后的回归、杀软误报处理与用户安全姿态劣化（关 Secure Boot 是最重代价）。
- **判断**：能力上与 PawnIO 完全重叠（温度/风扇/电压），风险与成本全面劣于路线 1。仅当 PawnIO
  政策性不可用时才考虑。**不推荐，不进下一轮。**

### 路线 3：Npcap（Wireshark 同款，用户自行安装）
- **安装要求**：用户从 npcap.com 自装（免费版 ≤5 台、**禁止再分发**；捆绑/静默部署需 OEM 商业许可）。
  安装时可选"WinPcap 兼容模式"与"仅管理员抓包"（默认开）。
- **签名/阻止名单风险**：零——Npcap 自带已签名驱动（NPCAP 服务），我们不碰内核。
- **C++ 调用方式**：动态 `LoadLibrary` wpcap.dll（默认在 `C:\Windows\System32\Npcap\`，兼容模式在 System32\；
  不新增链接依赖，项目已有 WinHTTP 动态绑定先例）：`pcap_findalldevs` → `pcap_open_live`（或
  pcap_create+pcap_activate）→ `pcap_compile/pcap_setfilter`（BPF）→ `pcap_next_ex` 循环取包 → `pcap_close`。
- **能拿到的能力**：真·抓包——完整链路层帧**含载荷**，全协议（IPv4/IPv6/TCP/UDP/DNS/TLS SNI 等），
  环回抓包（Npcap 特性）、每适配器统计、原始 802.11（可选）。
- **成本**：用户一次性安装（免费版个人使用合规）；应用侧抓包模块约 300-500 行（加载器+过滤+解析计数）。
- **许可证要点**：免费版=个人/研究用途、不得随应用分发；本应用**只检测并引导用户自装**，合规。

## 2. 对比矩阵

| 维度 | PawnIO | 自研测试签名驱动 | Npcap |
|---|---|---|---|
| 覆盖能力 | 温度+风扇+电压+能耗 | 温度+风扇+电压（自写全包） | 抓包含载荷 |
| 安装方 | 用户自装 1 个签名 exe | 用户改 bcdedit+关 Secure Boot+重启 | 用户自装（免费版） |
| 签名/黑名单风险 | 签名、HVCI 兼容、不在名单 | 测试签名：需关 SB；AV 高危误报 | 无（其驱动已签名） |
| C++ 接入 | 自写 3 个 IOCTL（已尖峰验证） | 自写驱动 300-600 行 | LoadLibrary wpcap.dll |
| 维护成本 | 极低（上游签名模块更新） | 高（签名/系统更新/杀软） | 低 |
| 许可证 | GPL+IOCTL 例外 / 模块 LGPL-2.1 | 全自担 | 免费=个人用，禁止再分发 |
| 结论 | **采用：硬件传感器** | **不采用** | **采用：抓包** |

**推荐组合**：PawnIO（温度/风扇/电压）+ Npcap（抓包含载荷）+ 自研驱动不推荐（成本见上）。
与现有 LhmSource（LHM HTTP 桥）互补：LHM 桥仍是零安装兜底；PawnIO 是进程内直读路径。

## 3. 每能力推荐实现路径（下一轮按此实现）

1. **每核 DTS 温度**：`SetThreadAffinityMask` 绑定目标逻辑核 → PawnIO+IntelMSR `read_msr(0x19C)`
   取 Digital Readout（bits 22:16）→ `read_msr(0x1A2)` 取 TjMax（bits 23:16）→ `温度 = TjMax - Readout`
   （°C，向下取整）。i5-12600H 为混合架构，0x19C 对 P/E 核均有效；逐核循环 20 线程（P/E 去重到物理核可后议）。
2. **风扇转速**：PawnIO+LpcIO：取 `Access_ISABUS.HTP.Method` 互斥体 → `select_slot(0/1)` → 读 chip id →
   `find_bars` → 按 SuperIO 型号（Nuvoton ITE 等，chip id 表）读 fan tach 寄存器（如 NCT6775 系 0x28/0x29 等）
   → RPM = 1350000/DIV/cnt（按芯片表换算）。
3. **电压**：同 LpcIO，读电压源 in0-inN 计数寄存器 × 芯片系数表 → mV/V。**只读寄存器，绝不写任何控制寄存器**。
4. **抓包含载荷**：KernelProbe 检测 Npcap → 动态加载 wpcap.dll → 枚举活动适配器 → BPF（默认只抓
   `ip or ip6`，排除本应用自身流量候选口）→ pcap_next_ex 逐帧取完整载荷 → 用户态解析（IPv4/IPv6 头、
   TCP/UDP 端口、可扩展 DNS/TLS），喂给现有 NetTables/流量页（聚合计数，载荷默认不落盘、不长期保存——隐私红线）。

## 4. 下一轮实现规格（经用户确认安装后执行）

- **新增/修改文件**：
  - `src/collect/PawnIoLink.h/.cpp`：模块加载/执行封装（IOCTL 已尖峰）、IntelMSR 温度读取、
    LpcIO 风扇/电压读取（含互斥体与芯片表）；复用 KernelProbe 打开设备的逻辑；
  - `src/collect/NpcapSource.h/.cpp`：wpcap.dll 动态绑定 + 抓包会话（不进主流程，按 LhmSource 的
    可选开关模式接入 options，默认关）；
  - `src/selftest/pawnio_link_test.cpp`、`src/selftest/npcap_source_test.cpp`：实机读数合理性断言
    （温度 0-120°C、RPM 0-6000 或 NoHardware，四态诚实模型；抓包回环 ≥1 包）；
  - `src/collect/KernelProbe.*` 保持现状（探测层已就绪）。
- **需要用户做的安装步骤**（各一次性，管理员）：
  1. 从 `https://pawnio.eu` 或 `https://github.com/namazso/PawnIO.Setup/releases` 下载 `PawnIO_setup.exe`
     安装（官方签名版，一路默认）；
  2. 从 `https://npcap.com` 安装 Npcap 免费版（勾选默认项即可；个人/研究用途）；
  3. 无需改 bcdedit/BIOS/任何安全设置。
- **验收标准**：
  - `kernel_probe_spike`/`stm_selftest` 全 PASS；`PawnIoTryGetVersion` 成功且版本 ≥ 2.0.0；
  - 温度页出现每核 DTS 读数（含 [PawnIO] 标记，与 LHM 交叉核对 ±3°C 内视为通过）；
  - 出现 ≥1 风扇转速与 ≥1 电压读数（主板支持范围内；读不到=NoHardware 而非 0）；
  - 回环 ping 或浏览器访问期间抓包计数增长且含载荷字节数 >0；
  - 卸载/未安装任一组件时，对应能力进入"未检测到组件"诚实态，主流程零影响。

## 5. 安全声明草稿（README 用）

> 本应用的可选深度硬件能力依赖两个第三方组件，**默认全部关闭、全部 opt-in、全部由用户自行从官方渠道安装**，
> 我们不捆绑、不下载、不静默安装任何驱动：
> - **PawnIO**（温度/风扇/电压）：仅通过其公开设备 IOCTL 接口调用官方已签名的只读模块；只执行寄存器"读"
>   操作（IntelMSR 白名单寄存器、SuperIO 传感寄存器），绝不写任何硬件控制寄存器、绝不干预调速；打开设备的
>   句柄仅在一次读取期间存在，无常驻内核上下文。读取同时打开 LHM 兜底桥时优先级可配置。
> - **Npcap**（抓包）：使用用户自行安装的 Wireshark 同款已签名驱动，应用只通过 wpcap.dll 用户态 API
>   （免驱调用）抓包；抓包仅在本机用户主动开启时进行，BPF 过滤可限定范围；载荷仅用于本机实时解析计数，
>   默认不写盘、不上传、退出即弃。
> 本应用与 PawnIO 仅通过设备 IO control 接口通信，符合其 GPL 例外条款；Npcap 免费版按其许可证仅用于
> 个人/研究用途且由用户自行安装。任一组件缺失时，对应功能如实显示"未安装/不可用"，不影响其余功能。
> 两种组件都可在"控制面板→程序"随时卸载。

## 6. 主要来源

- PawnIO 驱动/协议/许可证：github.com/namazso/PawnIO（README、pawnio_um.h、PawnIOLib.cpp）
- 模块：github.com/namazso/PawnIO.Modules（IntelMSR.p、LpcIO.p，LGPL-2.1）
- 安装包：github.com/namazso/PawnIO.Setup/releases（v2.2.0）；pawnio.eu（签名版/无限制版说明）
- HVCI 兼容与采用：Cleanmeter security 页、LibreHardwareMonitor MR "Add PawnIO as alternative to WinRing0"、
  FanControl V238 发行说明、poorlydocumented.com《Replacing WinRing0 in Fan Control with PawnIO》
- Npcap 许可与分发：npcap.com（免费版 ≤5 机、禁止再分发；OEM 许可说明）

#pragma once
// D3 阶段尖峰（2026-09-20，已获用户明确授权"支持调用内核，只要能力达得到"）：
// 内核能力探测层。目标能力（三选一路线见 docs/phase/11_kernel_research.md）：
//   1) CPU 每核 DTS 温度（MSR 0x19C/0x1A2，经 PawnIO IntelMSR 模块）
//   2) 主板风扇转速/电压（SuperIO 端口 IO，经 PawnIO LpcIO 模块）
//   3) 真·抓包含载荷（Npcap wpcap.dll，用户自行安装）
// 本单元只做【纯只读探测】与【调用约定尖峰】，绝不安装/启动/卸载任何
// 驱动或服务，绝不进入采集主流程（CollectService 不引用本文件）。
// 四态诚实模型：所有探测函数返回"事实"而非"假设"，未安装就如实报 false。
//
// PawnIO 调用约定（已从 PawnIOLib.cpp / pawnio_um.h 核实，v2.2.0）：
//   设备对象：\Device\PawnIO（用户态经 NtOpenFile 打开，无 DOS 符号链接）
//   IOCTL（全部 METHOD_BUFFERED / FILE_ANY_ACCESS，设备类型 41394）：
//     LOAD_BINARY = 0x821 槽，入参 = 模块 blob 原始字节
//     EXECUTE_FN  = 0x841 槽，入参 = char[32] 零填充函数名 + UINT64 参数[]
//                   出参 = UINT64 返回值数组
//     VERSION     = 0x861 槽
// 许可证结论（详见调研报告）：驱动 GPL-2.0 + IOCTL 例外条款——仅通过
// 设备 IOCTL 通信的独立程序不被 GPL 感染；官方模块 LGPL-2.1。
#include <string>

namespace stm {

// ---- 纯只读探测（注册表 / 文件属性 / CPUID，无句柄常驻，无常驻线程）----

// PawnIO 是否已安装：Services\PawnIO 服务键存在，或
// %ProgramFiles%\PawnIO\PawnIO.sys 文件存在（Setup 2.x 默认布局）。
bool PawnIOInstalled();

// Npcap 是否已安装：NPCAP（或 npf）服务存在，或
// System32\Npcap\wpcap.dll / System32\wpcap.dll（WinPcap 兼容模式）存在。
bool NpcapInstalled();

// 测试签名状态：读 SystemStartOptions（bcdedit testsigning 的运行时事实）。
// 返回非空中文状态串，如 "已开启（系统处于测试模式）" / "未开启"。
std::wstring TestSignStatus();

// HVCI（内存完整性）状态：DeviceGuard 注册表事实。返回非空中文状态串。
// 阻止名单随 HVCI 生效；本机实测 HVCI 关闭、阻止名单开启（默认值）。
std::wstring HvciStatus();

// CPU 品牌串（CPUID 0x80000002..4），如 "12th Gen Intel(R) Core(TM) i5-12600H"。
// 决定 MSR 布局路线（Alder Lake 用 IntelMSR 模块白名单寄存器）。
std::wstring CpuBrand();

// ---- 调用约定尖峰：仅当 PawnIO 已安装且驱动已运行时才会成功 ----

// 尝试打开 \Device\PawnIO 并执行 IOCTL_PIO_VERSION。
// 未安装/未运行时返回 false 且 *err 说明（不崩溃、不重试、不加载驱动）。
// ntdll（NtOpenFile/NtDeviceIoControlFile）动态绑定，不新增链接依赖。
bool PawnIoTryGetVersion(unsigned long* version, std::wstring* err);

}  // namespace stm

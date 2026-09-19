// kernel_probe_test：D3 内核能力探测尖峰的自测。
// 本机状态无关：只验证"不崩溃 + 返回事实状态"，绝不断言"必须已安装"。
// 安装/加载/IOCTL 实测在下一轮（用户确认安装后）进行。
#include "selftest/TestFramework.h"
#include "collect/KernelProbe.h"
#include <string>

// --- 探测函数在未安装/已安装两种本机状态下都必须安全返回事实 ---
STM_TEST(kernel_probe_pawnio_detect) {
    const bool installed = stm::PawnIOInstalled();  // 只读注册表/文件
    if (!installed) {
        // 本机（尖峰轮）预期未安装：PawnIoTryGetVersion 必须温和失败并给出原因。
        unsigned long version = 0;
        std::wstring ioErr;
        const bool got = stm::PawnIoTryGetVersion(&version, &ioErr);
        if (got) {
            // 未安装却拿到了版本 => 检测逻辑与设备事实矛盾。
            *err = L"PawnIOInstalled()==false 但 IOCTL 成功，检测自相矛盾";
            return false;
        }
        if (ioErr.empty()) {
            *err = L"IOCTL 失败时应给出中文原因，实际 err 为空";
            return false;
        }
    }
    return true;
}

STM_TEST(kernel_probe_npcap_detect) {
    // 只验证可调用且不崩溃；安装与否都不影响本轮通过（状态由 runner 输出）。
    (void)stm::NpcapInstalled();
    (void)err;
    return true;
}

STM_TEST(kernel_probe_status_strings) {
    const std::wstring ts = stm::TestSignStatus();
    if (ts.empty()) {
        *err = L"TestSignStatus 返回空串（应为非空中文状态）";
        return false;
    }
    const std::wstring hvci = stm::HvciStatus();
    if (hvci.empty()) {
        *err = L"HvciStatus 返回空串（应为非空中文状态）";
        return false;
    }
    return true;
}

STM_TEST(kernel_probe_cpu_brand) {
    const std::wstring brand = stm::CpuBrand();
    if (brand.empty()) {
        *err = L"CpuBrand 返回空串（CPUID 品牌叶应恒可用）";
        return false;
    }
    if (brand.size() > 48) {
        *err = L"CpuBrand 超过 CPUID 48 字符上限：" + brand;
        return false;
    }
    // Intel/AMD/Genuine 前缀至少应含商标词；仅提示不阻断（虚拟化环境可能特殊）。
    return true;
}

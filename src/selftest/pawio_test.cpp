// pawio_test：D6 PawnIO 内核通道（collect/PawnIoLink.h）的自测。
// 状态无关原则：本机装/未装 PawnIO 都必须安全通过——断言"不崩溃 +
// 返回事实 + 读数合理"，绝不断言"必须已安装"。
// 有读数时（官方 PawnIO + 签名模块已放置）额外做合理性断言：
//   每核 DTS 0-120°C、核数 ≥4（本机 i5-12600H：6P+4E=10 核 16 线程）；
//   风扇/电压无数据不算失败（笔记本常见：无经典 SuperIO 传感芯片）。
#include "selftest/TestFramework.h"
#include "collect/PawnIoLink.h"
#include <string>
#include <vector>

// --- 探针：不崩溃 + 状态诚实（detail 非空；版本语义一致）---
STM_TEST(pawnio_detect) {
    const stm::pawnio::PawnIoStatus st = stm::pawnio::GetPawnIoStatus();
    if (!st.probed) {
        *err = L"GetPawnIoStatus 后 probed 仍为 false";
        return false;
    }
    if (st.detail.empty()) {
        *err = L"状态 detail 为空（任何状态都应给出中文说明）";
        return false;
    }
    if (st.driverOpenable != (st.driverVersion >= 0x020000)) {
        *err = L"driverOpenable 与版本 ≥2.0 语义不一致";
        return false;
    }
    // 未安装时高层读数必须诚实返回 ≤0（而非 0 读数伪装成功）。
    if (!stm::pawnio::PawnIoAvailable()) {
        int temps[4] = {0};
        int lps[4] = {0};
        if (stm::pawnio::ReadCpuDtsTemps(temps, lps, 4) > 0) {
            *err = L"PawnIO 不可用却返回了 DTS 读数（伪造）";
            return false;
        }
        int rpms[4] = {0};
        stm::pawnio::ReadLpcIoFans(rpms, 4);   // 返回值任意：无数据合法（可选能力）
        double volts[4] = {0.0};
        stm::pawnio::ReadLpcIoVoltages(volts, 4);
    }
    return true;
}

// --- DTS 合理域：有读数时 0-120°C 且核数 ≥4；无 PawnIO 时诚实 ≤0 ---
STM_TEST(pawnio_dts_domain) {
    std::vector<int> temps(64, 0);
    std::vector<int> lps(64, -1);
    const int n = stm::pawnio::ReadCpuDtsTemps(temps.data(), lps.data(),
                                               static_cast<int>(temps.size()));
    if (n > 0) {
        if (!stm::pawnio::PawnIoAvailable()) {
            *err = L"读数成功但 PawnIoAvailable()==false，自相矛盾";
            return false;
        }
        if (n < 4) {
            *err = L"读数个数 <4（本机 12600H 至少应有 10 核）";
            return false;
        }
        for (int i = 0; i < n; ++i) {
            if (temps[i] <= 0 || temps[i] > 120) {
                *err = L"核心温度超出 0-120°C 合理域：核心 " + std::to_wstring(i) + L" = " +
                       std::to_wstring(temps[i]);
                return false;
            }
            // V26 P1-3：序号必须是真实逻辑处理器号（严格递增、不漂移）。
            if (lps[i] < 0 || (i > 0 && lps[i] <= lps[i - 1])) {
                *err = L"逻辑处理器序号非严格递增（标签会漂移）";
                return false;
            }
        }
    } else if (n == 0) {
        // 有应答但无有效读数：仅当 PawnIO 可用时才可能（MSR 全失败应为 -3）。
        if (!stm::pawnio::PawnIoAvailable()) {
            *err = L"PawnIO 不可用时应返回负错误码而非 0";
            return false;
        }
    } else {
        // 负错误码：{-1 不可用, -2 模块缺失, -3 MSR 失败} 都是诚实值。
        if (n < -3 || n > -1) {
            *err = L"负错误码不在 {-1,-2,-3} 诚实集合内";
            return false;
        }
    }
    return true;
}

// --- LpcIO 可选性：无数据/模块缺失都不算失败，读数域必须诚实 ---
STM_TEST(pawnio_lpcio_optional) {
    int rpms[7] = {0};
    const int nf = stm::pawnio::ReadLpcIoFans(rpms, 7);
    if (nf > 0) {
        for (int i = 0; i < nf; ++i) {
            if (rpms[i] <= 0 || rpms[i] > 8000) {
                *err = L"风扇转速超出 0-8000 rpm 合理域";
                return false;
            }
        }
        if (stm::pawnio::LpcIoChipName().empty()) {
            *err = L"有风扇读数但芯片名为空";
            return false;
        }
    } else if (nf < -3 || nf > 0) {
        *err = L"风扇返回值不在 {-3..0} 诚实集合内";
        return false;
    }
    double volts[9] = {0.0};
    const int nv = stm::pawnio::ReadLpcIoVoltages(volts, 9);
    if (nv > 0) {
        for (int i = 0; i < nv; ++i) {
            if (volts[i] <= 0.0 || volts[i] > 5.0) {
                *err = L"电压超出 0-5 V 合理域";
                return false;
            }
        }
    } else if (nv < -3 || nv > 0) {
        *err = L"电压返回值不在 {-3..0} 诚实集合内";
        return false;
    }
    // 模块加载接口的幂等性/非法参数诚实性（不依赖安装状态）。
    if (stm::pawnio::PawnIoLoadModule(nullptr, 0, nullptr)) {
        *err = L"非法参数（空 blob/空名）加载必须失败";
        return false;
    }
    return true;
}

// --- 真机安装验证（已安装时）：两次读数稳定性 + 每核合理性 ---
STM_TEST(pawnio_live_temps) {
    if (!stm::pawnio::PawnIoAvailable()) {
        return true;  // 未安装：诚实跳过（pawnio_detect 已覆盖该分支）
    }
    std::vector<int> a(64, 0);
    std::vector<int> aLp(64, -1);
    std::vector<int> b(64, 0);
    std::vector<int> bLp(64, -1);
    const int na = stm::pawnio::ReadCpuDtsTemps(a.data(), aLp.data(), static_cast<int>(a.size()));
    const int nb = stm::pawnio::ReadCpuDtsTemps(b.data(), bLp.data(), static_cast<int>(b.size()));
    if (na <= 0 || nb <= 0) {
        *err = L"PawnIO 可用但读不到 DTS（na=" + std::to_wstring(na) +
               L", nb=" + std::to_wstring(nb) + L"）";
        return false;
    }
    if (na != nb) {
        *err = L"两次读数核数不一致（绑定/枚举不稳定）";
        return false;
    }
    for (int i = 0; i < na; ++i) {
        if (aLp[i] != bLp[i]) {
            *err = L"两次读数逻辑处理器序号不一致";
            return false;
        }
        if (b[i] < a[i] - 25 || b[i] > a[i] + 25) {
            *err = L"两次读数相差超过 25°C（不稳定）：核心 " + std::to_wstring(i);
            return false;
        }
    }
    return true;
}

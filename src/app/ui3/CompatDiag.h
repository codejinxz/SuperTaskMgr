#pragma once
// 兼容模式诊断文本（维护轮 10）。header-only、不依赖 ImGui——与
// PageHelpers.h / MemCleanup.h 同一模式，使 stm_selftest 能在无 GUI 下
// 直接验证诊断报告的纯文本生成。
//
// 职责边界（隐私契约）：
//  - CompatDiagReportText：纯函数，生成一份人读的纯文本诊断报告
//   （系统版本 + 应用版本 + 降级原因 + 自检逐项结果 + 解释 + 隐私说明）。
//  - SaveDiagnosticsFile：把报告写到本机 %LOCALAPPDATA%\SuperTaskMgr\logs\
//    diagnostics_<时间戳>.txt。纯本地文件操作，绝不联网、绝不自动上传；
//    上传（如果有）永远留给用户手动操作该文件。
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "collect/CollectService.h"  // CollectService::SelfCheckItem（契约类型）
#include "core/FsUtil.h"
#include "core/Str.h"

namespace stm {
namespace ui3 {

// 系统版本一行。用 ntdll!RtlGetVersion（文档化导出）读取真实版本，
// 不受主程序清单兼容性声明的影响（GetVersionExW 在无兼容声明时会虚报 6.2）。
// V24 P2-3：家用/服务器以 wProductType 区分——build ≥ 22000 的 Server
// 不是"Windows 11"，按产品类型诚实标注。
inline std::wstring SystemVersionLine() {
    using RtlGetVersionFn = long(__stdcall*)(RTL_OSVERSIONINFOW*);
    RTL_OSVERSIONINFOEXW vi{};  // 需要 wProductType；RtlGetVersion 接受 Ex 尺寸
    vi.dwOSVersionInfoSize = sizeof(vi);
    bool ok = false;
    const HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
    if (nt) {
        const auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(nt, "RtlGetVersion")));
        ok = fn != nullptr && fn(reinterpret_cast<RTL_OSVERSIONINFOW*>(&vi)) == 0;
    }
    if (!ok) return L"Windows（版本读取不可用）";

    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    const wchar_t* arch =
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64
            ? L"ARM64"
            : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? L"x64" : L"x86";
    const wchar_t* family = vi.wProductType != VER_NT_WORKSTATION
                                ? L"Windows Server"
                                : (vi.dwBuildNumber >= 22000 ? L"Windows 11" : L"Windows 10");
    // 注意：stm::Fmt 走运行期 vformat（无可编译期检查），占位符个数/类型
    // 必须人工对齐——此处 5 个占位符对应 5 个实参。
    return Fmt(L"{} {}.{}.{}（{}）", family, static_cast<unsigned>(vi.dwMajorVersion),
               static_cast<unsigned>(vi.dwMinorVersion), static_cast<unsigned>(vi.dwBuildNumber),
               arch);
}

// 生成纯文本诊断报告（剪贴板与 diagnostics_*.txt 共用同一文本；测试直接调用）。
// items 为空时输出"尚未自检"占位；degradeReason 为空表示未降级。
inline std::wstring CompatDiagReportText(
    const std::wstring& appVersion,
    const std::vector<CollectService::SelfCheckItem>& items,
    const std::wstring& degradeReason) {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    std::wstring t;
    t += L"SuperTaskMgr 兼容模式诊断报告\n";
    t += L"========================================\n";
    t += Fmt(L"生成时间：{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}\n",
             static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
             static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
             static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    t += L"应用版本：" + appVersion + L"\n";
    t += L"系统版本：" + SystemVersionLine() + L"\n";
    t += degradeReason.empty() ? std::wstring(L"降级状态：完整模式（未降级）\n")
                               : L"降级状态：兼容模式 —— " + degradeReason + L"\n";
    t += L"\n自检逐项结果（NtQSI 快路径 vs 文档化 API 交叉比对，共 6 项）：\n";
    if (items.empty()) {
        t += L"  （尚未自检：采集服务尚未运行第一轮自检门）\n";
    } else {
        for (const CollectService::SelfCheckItem& it : items) {
            const wchar_t* mark = !it.ran ? L"[跳过]" : (it.passed ? L"[通过]" : L"[失败]");
            t += L"  " + std::wstring(mark) + L" " + (it.name == nullptr ? L"" : it.name) +
                 L"\n";
            if (!it.detail.empty()) t += L"          " + it.detail + L"\n";
        }
    }
    t += L"\n为什么会进入兼容模式？\n";
    t += L"  应用启动时把 NtQuerySystemInformation 的半文档化结构读数与官方 API\n";
    t += L"  交叉比对 6 项；自动重试（至多 2 次）后仍未达成连续两轮全部通过，\n";
    t += L"  才整体降级，绝不显示错误数据。常见原因：\n";
    t += L"  1) Windows 更新更改了内部结构布局；\n";
    t += L"  2) 安全软件挂钩 NtQuerySystemInformation 并修改其返回数据；\n";
    t += L"  3) 系统策略/运行环境（虚拟化、精简系统）限制读取进程内部计数器。\n";
    t += L"\n降级后受影响的功能：\n";
    t += L"  进程表「内存」列（私有工作集）与「上下文切换/s」列显示「—」；\n";
    t += L"  已挂起进程的徽标/详情状态不再显示；「内存加速」无法按私有工作集\n";
    t += L"  挑选候选。其余功能（CPU/磁盘/网络速率、进程管理等）不受影响，\n";
    t += L"  仅采集路径较慢（Toolhelp+PSAPI）。\n";
    t += L"\n隐私说明：本报告仅复制到剪贴板并保存在本机 logs 目录，"
          L"应用不会自动上传任何数据。\n";
    return t;
}

// 把报告写到 LogDir()\diagnostics_<时间戳>.txt（UTF-8 带 BOM，记事本直开）。
// V24 P2-4：同秒多次导出不再互相覆盖（冲突追加 _2/_3… 序号后缀）；写失败
// 时删除残留文件并返回空串。绝不联网。
inline std::wstring SaveDiagnosticsFile(const std::wstring& text) {
    const std::wstring dir = LogDir();
    if (dir.empty()) return L"";
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    const std::wstring base =
        dir + L"\\diagnostics_" +
        Fmt(L"{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}", static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond));
    std::wstring path = base + L".txt";
    for (int seq = 2; ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++seq) {
        path = base + L"_" + std::to_wstring(seq) + L".txt";  // 同秒已存在：序号后缀
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return L"";
    static const unsigned char kBom[3] = {0xEF, 0xBB, 0xBF};
    const std::string u8 = WideToUtf8(text);
    const bool ok = fwrite(kBom, 1, sizeof(kBom), f) == sizeof(kBom) &&
                    fwrite(u8.data(), 1, u8.size(), f) == u8.size();
    fclose(f);
    if (!ok) {
        ::DeleteFileW(path.c_str());  // 写失败不残留半截文件
        return L"";
    }
    return path;
}

}  // namespace ui3
}  // namespace stm

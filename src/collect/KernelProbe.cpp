// KernelProbe 实现：注册表 / 文件属性 / CPUID 只读探测 + PawnIO IOCTL 尖峰。
// 设计约束：任何失败路径只返回 false/状态串，绝不抛异常、绝不重试、
// 绝不创建句柄常驻（打开-查询-关闭），绝不写任何注册表键值。
#include "collect/KernelProbe.h"

#include <windows.h>
#include <winioctl.h>  // CTL_CODE/METHOD_BUFFERED（LEAN_AND_MEAN 下不含）
#include <intrin.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace stm {
namespace {

bool RegKeyExists(const wchar_t* subkey) {
    HKEY key = nullptr;
    const LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &key);
    if (st == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

// 读 REG_SZ/REG_EXPAND_SZ；不存在或类型不符返回 false。
bool RegReadSz(const wchar_t* subkey, const wchar_t* value, std::wstring* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    bool ok = false;
    if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t)) {
        std::wstring buf(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, value, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buf.data()), &bytes) == ERROR_SUCCESS) {
            const size_t len = wcsnlen(buf.c_str(), buf.size());
            *out = buf.substr(0, len);
            ok = true;
        }
    }
    RegCloseKey(key);
    return ok;
}

// 读 REG_DWORD；不存在返回 false。
bool RegReadDword(const wchar_t* subkey, const wchar_t* value, DWORD* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    bool ok = false;
    if (RegQueryValueExW(key, value, nullptr, &type,
                         reinterpret_cast<LPBYTE>(out), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && bytes == sizeof(DWORD)) {
        ok = true;
    }
    RegCloseKey(key);
    return ok;
}

bool FileExists(const wchar_t* path) {
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring EnvProgramFiles() {
    wchar_t buf[MAX_PATH] = {};
    const UINT n = GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"C:\\Program Files";
    }
    return buf;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

// ---- PawnIO IOCTL 协议常量（来源 PawnIO v2.2.0 pawnio_um.h）----
constexpr ULONG kPawnIODeviceType = 41394;  // 驱动自定义设备类型
constexpr ULONG kPawnIoLoadBinary = CTL_CODE(kPawnIODeviceType, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kPawnIoExecuteFn  = CTL_CODE(kPawnIODeviceType, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kPawnIoVersion    = CTL_CODE(kPawnIODeviceType, 0x861, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr wchar_t kPawnIoDevicePath[] = L"\\Device\\PawnIO";

constexpr LONG kNtStatusSuccess = 0;
constexpr LONG kNtStatusPending = 0x00000103;  // STATUS_PENDING

// 本地最小 NT 结构定义（避免依赖 winternl.h 的布局差异；与
// PawnIOLib.cpp 的做法一致）。x64 与 x86 均为指针尺寸对齐布局。
struct NtUnicodeString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};

struct NtIoStatusBlock {
    union {
        LONG status;
        void* pointer;
    } u;
    ULONG_PTR information;
};

struct NtObjectAttributes {
    ULONG length;
    HANDLE root_directory;
    NtUnicodeString* object_name;
    ULONG attributes;
    void* security_descriptor;
    void* security_quality_of_service;
};

constexpr ULONG kObjCaseInsensitive = 0x00000040;

using NtOpenFileFn = LONG(NTAPI*)(HANDLE*, ACCESS_MASK, NtObjectAttributes*,
                                  NtIoStatusBlock*, ULONG, ULONG);
using RtlInitUnicodeStringFn = void(NTAPI*)(NtUnicodeString*, PWSTR);
using NtDeviceIoControlFileFn = LONG(NTAPI*)(HANDLE, HANDLE, void*, void*,
                                             NtIoStatusBlock*, ULONG, void*, ULONG,
                                             void*, ULONG);
using NtWaitForSingleObjectFn = LONG(NTAPI*)(HANDLE, BOOLEAN, LARGE_INTEGER*);
using NtCloseFn = LONG(NTAPI*)(HANDLE);

template <typename Fn>
Fn NtdllProc(const char* name) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, name)));
}

std::wstring NtStatusHex(LONG status) {
    wchar_t buf[16] = {};
    swprintf_s(buf, L"0x%08X", static_cast<unsigned long>(status));
    return buf;
}

}  // namespace

bool PawnIOInstalled() {
    // Setup 2.x：创建服务 "PawnIO"（驱动位于安装目录），服务键为最可靠事实。
    if (RegKeyExists(L"SYSTEM\\CurrentControlSet\\Services\\PawnIO")) {
        return true;
    }
    const std::wstring sys = EnvProgramFiles() + L"\\PawnIO\\PawnIO.sys";
    return FileExists(sys.c_str());
}

bool NpcapInstalled() {
    // Npcap 1.x 服务名 NPCAP（WinPcap 兼容层可能注册 npf）。
    if (RegKeyExists(L"SYSTEM\\CurrentControlSet\\Services\\NPCAP") ||
        RegKeyExists(L"SYSTEM\\CurrentControlSet\\Services\\npf")) {
        return true;
    }
    // DLL 事实：Npcap 默认装到 System32\Npcap\；勾选 WinPcap 兼容模式则直装 System32。
    if (FileExists(L"C:\\Windows\\System32\\Npcap\\wpcap.dll") ||
        FileExists(L"C:\\Windows\\System32\\wpcap.dll")) {
        return true;
    }
    return false;
}

std::wstring TestSignStatus() {
    std::wstring opts;
    if (!RegReadSz(L"SYSTEM\\CurrentControlSet\\Control", L"SystemStartOptions", &opts)) {
        return L"未知（无法读取 SystemStartOptions）";
    }
    if (ToLower(opts).find(L"testsigning") != std::wstring::npos) {
        return L"已开启（系统处于测试模式，可加载测试签名驱动）";
    }
    return L"未开启（加载测试签名驱动会被拒绝）";
}

std::wstring HvciStatus() {
    // 场景键 Enabled：1=已启用，0=已显式关闭；键缺失=未配置（VBS 关闭时为关）。
    DWORD scenario = 0;
    const bool has_scenario =
        RegReadDword(L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios"
                     L"\\HypervisorEnforcedCodeIntegrity",
                     L"Enabled", &scenario);
    if (has_scenario) {
        return scenario != 0 ? L"已启用（HVCI 运行中，阻止名单强制生效）"
                             : L"未启用（显式关闭）";
    }
    DWORD vbs = 0;
    if (RegReadDword(L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
                     L"EnableVirtualizationBasedSecurity", &vbs) &&
        vbs != 0) {
        return L"由 VBS/组策略管理（需进一步确认运行态）";
    }
    return L"未配置（VBS 关闭时 HVCI 为关闭）";
}

std::wstring CpuBrand() {
    int regs[4] = {};
    __cpuid(regs, 0x80000000);
    const unsigned int max_ext = static_cast<unsigned int>(regs[0]);
    if (max_ext < 0x80000004u) {
        return L"未知 CPU（无品牌串扩展叶）";
    }
    int brand[16] = {};
    __cpuid(brand, 0x80000002);
    __cpuid(brand + 4, 0x80000003);
    __cpuid(brand + 8, 0x80000004);
    const char* p = reinterpret_cast<const char*>(brand);
    const size_t raw_len = 48;
    size_t begin = 0;
    while (begin < raw_len && p[begin] == ' ') {
        ++begin;
    }
    size_t end = raw_len;
    while (end > begin && p[end - 1] == '\0') {
        --end;
    }
    const std::string ascii(p + begin, end - begin);
    return std::wstring(ascii.begin(), ascii.end());
}

bool PawnIoTryGetVersion(unsigned long* version, std::wstring* err) {
    if (version == nullptr || err == nullptr) {
        return false;
    }
    *version = 0;

    const auto nt_open_file = NtdllProc<NtOpenFileFn>("NtOpenFile");
    const auto rtl_init = NtdllProc<RtlInitUnicodeStringFn>("RtlInitUnicodeString");
    const auto nt_dev_ioctl = NtdllProc<NtDeviceIoControlFileFn>("NtDeviceIoControlFile");
    const auto nt_wait = NtdllProc<NtWaitForSingleObjectFn>("NtWaitForSingleObject");
    const auto nt_close = NtdllProc<NtCloseFn>("NtClose");
    if (nt_open_file == nullptr || rtl_init == nullptr || nt_dev_ioctl == nullptr ||
        nt_wait == nullptr || nt_close == nullptr) {
        *err = L"ntdll 接口绑定失败";
        return false;
    }

    // 打开 \Device\PawnIO（无 DOS 符号链接，须用 NT 路径）。
    std::wstring path(kPawnIoDevicePath);
    NtUnicodeString ustr{};
    rtl_init(&ustr, path.data());
    NtObjectAttributes attr{};
    attr.length = sizeof(attr);
    attr.object_name = &ustr;
    attr.attributes = kObjCaseInsensitive;
    NtIoStatusBlock iosb{};
    HANDLE handle = nullptr;
    const LONG open_st = nt_open_file(&handle, GENERIC_READ | GENERIC_WRITE, &attr, &iosb,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (open_st != kNtStatusSuccess) {
        *err = L"PawnIO 设备未就绪（未安装或服务未运行，NTSTATUS " + NtStatusHex(open_st) + L"）";
        return false;
    }

    // IOCTL_PIO_VERSION：METHOD_BUFFERED，出参 = 驱动版本 ULONG。
    unsigned char out[16] = {};
    NtIoStatusBlock v_iosb{};
    LARGE_INTEGER wait_zero{};
    wait_zero.QuadPart = -100000000;  // 最多等 10s，防悬挂
    LONG st = nt_dev_ioctl(handle, nullptr, nullptr, nullptr, &v_iosb, kPawnIoVersion,
                           nullptr, 0, out, sizeof(out));
    if (st == kNtStatusPending) {
        st = nt_wait(handle, FALSE, &wait_zero);
        if (st == kNtStatusSuccess) {
            st = v_iosb.u.status;
        }
    }
    nt_close(handle);
    if (st != kNtStatusSuccess) {
        *err = L"IOCTL_PIO_VERSION 失败（NTSTATUS " + NtStatusHex(st) + L"）";
        return false;
    }
    ULONG v = 0;
    std::memcpy(&v, out, sizeof(v));
    if (v == 0) {
        *err = L"IOCTL_PIO_VERSION 返回 0（协议不符，需复核）";
        return false;
    }
    *version = v;
    return true;
}

// 保留未用的加载/执行 IOCTL 常量引用说明：下一轮按本文件注释的
// 布局（char[32] 函数名 + UINT64 参数数组）实现模块加载与执行。
static_assert(kPawnIoLoadBinary != 0 && kPawnIoExecuteFn != 0 && kPawnIoVersion != 0,
              "PawnIO IOCTL 编码必须非零");

}  // namespace stm

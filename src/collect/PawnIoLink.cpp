// PawnIoLink 实现 —— PawnIO 设备 IOCTL + 官方签名模块的加载/执行 +
// IntelMSR 每核 DTS 温度 + LpcIO SuperIO 风扇/电压（D6，2026-09-20）。
//
// 约束：本单元绝不发起任何网络请求；绝不写任何 MSR/控制寄存器（MSR 只读
// 白名单寄存器 0x19C/0x1A2；SuperIO 只读传感寄存器，绝不写配置寄存器）；
// 全部失败路径诚实（返回 ≤0 / 状态枚举 / 中文 detail），绝不伪造读数；
// 日志模块名 "pawnio"。句柄策略：与 LHM 相同，每个已加载模块保留一个
// 常驻执行器句柄（进程生命周期内）；打开设备本身无内核副作用——PawnIO
// 是只读沙箱 VM，模块按官方签名校验后载入。
//
// 芯片表来源：LibreHardwareMonitor（MPL-2.0）公开的 ITE IT87xx / Nuvoton
// NCT677x 寄存器布局（Hardware/Motherboard/Lpc/IT87XX.cs、Nct677X.cs、
// Chip.cs，2026-09 快照），仅取我们需要的只读寄存器与换算公式。
#include "collect/PawnIoLink.h"

#include "core/Log.h"
#include <windows.h>
#include <winioctl.h>   // CTL_CODE/METHOD_BUFFERED（LEAN_AND_MEAN 下不含）
#include <bcrypt.h>     // SHA-256 白名单（V26 P2）
#pragma comment(lib, "bcrypt")

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace stm {
namespace pawnio {
namespace {

// ---- IOCTL 协议常量（PawnIO v2.2.0 pawnio_um.h / LHM PawnIo.cs 双源核对）----
constexpr ULONG kPawnIODeviceType = 41394;
constexpr ULONG kPawnIoLoadBinary = CTL_CODE(kPawnIODeviceType, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kPawnIoExecuteFn  = CTL_CODE(kPawnIODeviceType, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr ULONG kPawnIoVersion    = CTL_CODE(kPawnIODeviceType, 0x861, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr wchar_t kPawnIoDevicePath[] = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";

// LHM PawnIo.cs：函数名字段固定 32 字节（ASCII，末字节必须为 0）。
constexpr size_t kFnNameLength = 32;
constexpr size_t kMaxBlobBytes = 1u << 20;   // 模块 blob 合法上限（实际 ~5-18 KiB）

// ---- 官方渠道指引常量（仅文档/状态文案用途，代码绝不访问网络）----
// 引用点：GetPawnIoStatus（未安装提示）与 PawnIoLoadModuleFromFile（缺文件提示）。
constexpr wchar_t kUrlSetup[] =
    L"https://github.com/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe";
constexpr wchar_t kUrlModulesRelease[] =
    L"https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11";
constexpr wchar_t kUrlModulesLhm[] =
    L"https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/tree/master/"
    L"LibreHardwareMonitorLib/Resources/PawnIo";

// ---- 官方 blob SHA-256 白名单（V26 P2 纵深防御）---------------------------
// 钉扎对象 = PawnIO.Modules 0.2.11 官方签名模块（驱动侧签名校验之外，
// 在文件加载路径上再钉住具体发布物；未知哈希拒载）。官方发布新版本时
// 同步更新此处。
constexpr struct {
    const char* module;
    const wchar_t* sha256;  // 小写十六进制
} kBlobHashWhitelist[] = {
    {"IntelMSR", L"d6ed85d65ab17a22f813ef98207d6d537155ee2ded5976a21cb48413c9b92e5f"},
    {"LpcIO", L"b3896a1cab0d808fca31fe2ebcae045d59dac690da87b17c858bb8da357eb45e"},
};

// data 的 SHA-256 -> 小写十六进制。失败返回 false（诚实拒载）。
bool Sha256Hex(const uint8_t* data, size_t size, std::wstring* hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        alg == nullptr) {
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32] = {};
    bool ok = false;
    DWORD cbDigest = sizeof(digest);
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 && hash != nullptr) {
        ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(data)),
                            static_cast<ULONG>(size), 0) == 0 &&
             BCryptFinishHash(hash, digest, cbDigest, 0) == 0;
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    static constexpr wchar_t kHexDigits[] = L"0123456789abcdef";
    hex->clear();
    hex->reserve(64);
    for (const uint8_t b : digest) {
        *hex += kHexDigits[b >> 4];
        *hex += kHexDigits[b & 0xF];
    }
    return true;
}

bool HashWhitelisted(const char* name, const std::wstring& hex) {
    for (const auto& entry : kBlobHashWhitelist) {
        if (std::strcmp(entry.module, name) != 0) continue;
        if (hex.size() != 64) return false;
        for (size_t i = 0; i < 64; ++i) {
            const wchar_t a = static_cast<wchar_t>(::towlower(hex[i]));
            const wchar_t b = static_cast<wchar_t>(::towlower(entry.sha256[i]));
            if (a != b) return false;
        }
        return true;
    }
    return false;  // 未知模块名：不在白名单体系内，拒载
}

// ---- 进程内状态与模块槽位表 -----------------------------------------------
std::mutex g_mu;
bool g_probed = false;
bool g_driverOpenable = false;
uint32_t g_driverVersion = 0;
std::wstring g_probeDetail;                      // 设备探测状态（诚实）
std::map<std::string, HANDLE> g_modules;         // 模块名 -> 执行器句柄
std::vector<HANDLE> g_retired;                   // V26 P2：重载退役句柄——延迟到
                                                 // PawnIoShutdown 关闭，防在途
                                                 // ExecuteN 撞上已关闭句柄
std::wstring g_lpcChipName;                      // 最近识别到的 SuperIO 芯片名

HANDLE OpenDevice() {
    // 与 LHM 一致：GLOBALROOT NT 前缀经 Win32 打开，读写共享。
    return CreateFileW(kPawnIoDevicePath, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

// 单次 EXECUTE_FN：入参 = char[32] 函数名 + 参数数组；出参 = UINT64[]。
bool ExecuteN(HANDLE h, const char* fn, const uint64_t* in, size_t inCount,
              uint64_t* out, size_t outMax, size_t* outCount) {
    if (h == nullptr || fn == nullptr || (inCount > 0 && in == nullptr) ||
        (outMax > 0 && out == nullptr)) {
        return false;
    }
    const size_t nameLen = strlen(fn);
    if (nameLen == 0 || nameLen >= kFnNameLength) {
        return false;  // 协议要求名字非空且能容纳 NUL
    }
    std::vector<uint8_t> buf(kFnNameLength + inCount * sizeof(uint64_t), 0);
    std::memcpy(buf.data(), fn, nameLen);
    if (inCount > 0) {
        std::memcpy(buf.data() + kFnNameLength, in, inCount * sizeof(uint64_t));
    }
    std::vector<uint8_t> ret(outMax * sizeof(uint64_t), 0);
    DWORD bytes = 0;
    if (!DeviceIoControl(h, kPawnIoExecuteFn, buf.data(),
                         static_cast<DWORD>(buf.size()),
                         outMax > 0 ? ret.data() : nullptr,
                         static_cast<DWORD>(ret.size()), &bytes, nullptr)) {
        return false;  // 模块函数返回非成功 NTSTATUS 时也走这里
    }
    if (outCount != nullptr) {
        *outCount = bytes / sizeof(uint64_t);
    }
    if (outMax > 0 && bytes > 0) {
        const size_t copy = std::min<size_t>(bytes, outMax * sizeof(uint64_t));
        std::memcpy(out, ret.data(), copy);
    }
    return true;
}

// 取模块句柄（已加载才返回非空；加载须显式走 PawnIoLoadModule*）。
HANDLE ModuleHandle(const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_modules.find(name);
    return it == g_modules.end() ? nullptr : it->second;
}

// ---- LpcIO 模块函数（_mod_lpcio.p / LHM PawnIo.LpcIo.cs 双源核对）----------
constexpr uint8_t kRegChipId = 0x20;
constexpr uint8_t kRegChipRev = 0x21;
constexpr uint8_t kRegDeviceSelect = 0x07;
constexpr uint8_t kRegBaseAddress = 0x60;

bool LpcSelectSlot(HANDLE h, int slot) {
    const uint64_t in = static_cast<uint64_t>(slot);
    return ExecuteN(h, "ioctl_select_slot", &in, 1, nullptr, 0, nullptr);
}
bool LpcFindBars(HANDLE h) {
    return ExecuteN(h, "ioctl_find_bars", nullptr, 0, nullptr, 0, nullptr);
}
bool LpcSuperioInb(HANDLE h, uint8_t reg, uint8_t* value) {
    const uint64_t in = reg;
    uint64_t out = 0;
    size_t n = 0;
    if (!ExecuteN(h, "ioctl_superio_inb", &in, 1, &out, 1, &n) || n != 1) return false;
    *value = static_cast<uint8_t>(out & 0xFF);
    return true;
}
bool LpcSuperioInw(HANDLE h, uint8_t reg, uint16_t* value) {
    const uint64_t in = reg;
    uint64_t out = 0;
    size_t n = 0;
    if (!ExecuteN(h, "ioctl_superio_inw", &in, 1, &out, 1, &n) || n != 1) return false;
    *value = static_cast<uint16_t>(out & 0xFFFF);
    return true;
}
bool LpcSuperioOutb(HANDLE h, uint8_t reg, uint8_t value) {
    const uint64_t in[2] = {reg, value};
    return ExecuteN(h, "ioctl_superio_outb", in, 2, nullptr, 0, nullptr);
}
bool LpcPioOutb(HANDLE h, uint16_t port, uint8_t value) {
    const uint64_t in[2] = {port, value};
    return ExecuteN(h, "ioctl_pio_outb", in, 2, nullptr, 0, nullptr);
}
bool LpcPioInb(HANDLE h, uint16_t port, uint8_t* value) {
    const uint64_t in = port;
    uint64_t out = 0;
    size_t n = 0;
    if (!ExecuteN(h, "ioctl_pio_inb", &in, 1, &out, 1, &n) || n != 1) return false;
    *value = static_cast<uint8_t>(out & 0xFF);
    return true;
}

// SuperIO 配置空间进入/退出序列（LHM LpcPort.cs；写寄存器口本身——
// 模块白名单恒放行 0x2E/0x2F 与 0x4E/0x4F）。
constexpr uint16_t kRegisterPorts[2] = {0x2E, 0x4E};
void NuvotonEnter(HANDLE h, uint16_t regPort) {
    LpcPioOutb(h, regPort, 0x87);
    LpcPioOutb(h, regPort, 0x87);
}
void NuvotonExit(HANDLE h, uint16_t regPort) {
    LpcPioOutb(h, regPort, 0xAA);
}
void It87Enter(HANDLE h, uint16_t regPort) {
    LpcPioOutb(h, regPort, 0x87);
    LpcPioOutb(h, regPort, 0x01);
    LpcPioOutb(h, regPort, 0x55);
    LpcPioOutb(h, regPort, regPort == 0x4E ? 0xAA : 0x55);
}
// IT87Exit：写配置控制寄存器 0x02 <- 0x02；副口（0x4E）按 LHM 不退出。
void It87Exit(HANDLE h, uint16_t regPort) {
    if (regPort == 0x4E) return;
    LpcSuperioOutb(h, 0x02, 0x02);
}

// 环境控制器（hwmon BAR）寄存器访问：全部走 BAR+5（索引）/BAR+6（数据），
// 这正是模块 BAR 白名单按 8 字节窗口放行的访问方式（LHM IT87XX/Nct677X 同法）。
// ITE：写索引 -> 读数据，并回读索引校验（IT8688E 除外，LHM 注）。
bool ItEnvRead(HANDLE h, uint16_t base, uint8_t reg, uint8_t* value, bool* valid) {
    uint8_t v = 0;
    if (!LpcPioOutb(h, static_cast<uint16_t>(base + 5), reg) ||
        !LpcPioInb(h, static_cast<uint16_t>(base + 6), &v)) {
        return false;
    }
    uint8_t rb = 0;
    if (!LpcPioInb(h, static_cast<uint16_t>(base + 5), &rb)) {
        return false;
    }
    *valid = rb == reg;
    *value = v;
    return true;
}
// Nuvoton NCT67xx：bank select（索引口写 0x4E，数据口写 bank 高字节）再寻址。
bool NctEnvRead(HANDLE h, uint16_t base, uint16_t reg, uint8_t* value) {
    if (!LpcPioOutb(h, static_cast<uint16_t>(base + 5), 0x4E) ||
        !LpcPioOutb(h, static_cast<uint16_t>(base + 6), static_cast<uint8_t>(reg >> 8))) {
        return false;
    }
    if (!LpcPioOutb(h, static_cast<uint16_t>(base + 5), static_cast<uint8_t>(reg & 0xFF))) {
        return false;
    }
    return LpcPioInb(h, static_cast<uint16_t>(base + 6), value);
}

// ---- 芯片表（LHM Chip.cs / IT87XX.cs / Nct677X.cs 只读子集）----------------

struct It87Chip {
    uint16_t id;        // inw(0x20)
    const wchar_t* name;
    double voltageGain; // vin 计数 -> V（LHM _voltageGain 按型号）
};

constexpr It87Chip kIt87Chips[] = {
    {0x8613, L"ITE IT8613E", 0.012}, {0x8620, L"ITE IT8620E", 0.012},
    {0x8625, L"ITE IT8625E", 0.011}, {0x8628, L"ITE IT8628E", 0.012},
    {0x8631, L"ITE IT8631E", 0.012}, {0x8638, L"ITE IT8638E", 0.012},
    {0x8655, L"ITE IT8655E", 0.0109}, {0x8665, L"ITE IT8665E", 0.0109},
    {0x8686, L"ITE IT8686E", 0.012}, {0x8688, L"ITE IT8688E", 0.012},
    {0x8689, L"ITE IT8689E", 0.012}, {0x8696, L"ITE IT8696E", 0.012},
    {0x8705, L"ITE IT8705F", 0.016}, {0x8712, L"ITE IT8712F", 0.016},
    {0x8716, L"ITE IT8716F", 0.016}, {0x8718, L"ITE IT8718F", 0.016},
    {0x8720, L"ITE IT8720F", 0.016}, {0x8721, L"ITE IT8721F", 0.012},
    {0x8726, L"ITE IT8726F", 0.016}, {0x8728, L"ITE IT8728F", 0.012},
    {0x8771, L"ITE IT8771E", 0.012}, {0x8772, L"ITE IT8772E", 0.012},
    {0x8790, L"ITE IT8790E", 0.016}, {0x8733, L"ITE IT8792E", 0.011},
};

struct NctChip {
    uint16_t code;      // (inb(0x20) << 8) | inb(0x21)
    const wchar_t* name;
    bool rpm16Bit;      // true = 0x656+i*2 直读 RPM；false = 13 位计数 0x4B0+i*2
};

constexpr NctChip kNctChips[] = {
    {0xB470, L"Nuvoton NCT6771F", true}, {0xC330, L"Nuvoton NCT6776F", true},
    {0xC560, L"Nuvoton NCT6779D", false}, {0xC803, L"Nuvoton NCT6791D", false},
    {0xC911, L"Nuvoton NCT6792D", false}, {0xC913, L"Nuvoton NCT6792DA", false},
    {0xD121, L"Nuvoton NCT6793D", false}, {0xD352, L"Nuvoton NCT6795D", false},
    {0xD423, L"Nuvoton NCT6796D", false}, {0xD42A, L"Nuvoton NCT6796DR", false},
    {0xD451, L"Nuvoton NCT6797D", false}, {0xD42B, L"Nuvoton NCT6798D", false},
    {0xD802, L"Nuvoton NCT6799D", false},
};

// ---- 一次 LpcIO 会话 --------------------------------------------------------
struct LpcResult {
    bool chipKnown = false;
    std::wstring chipName;
    int fans[7] = {0};
    int fanCount = 0;
    double volts[9] = {0.0};
    int voltCount = 0;
};

// LHM Mutexes.WaitIsaBus：全局互斥体 \BaseNamedObjects\Access_ISABUS.HTP.Method。
class IsaBusMutex {
public:
    explicit IsaBusMutex(DWORD timeoutMs)
        : m_(CreateMutexW(nullptr, FALSE, L"Global\\Access_ISABUS.HTP.Method")) {
        if (m_ != nullptr) {
            const DWORD wait = WaitForSingleObject(m_, timeoutMs);
            owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }
    ~IsaBusMutex() {
        if (owned_) ReleaseMutex(m_);
        if (m_ != nullptr) CloseHandle(m_);
    }
    bool Owned() const { return owned_; }
    IsaBusMutex(const IsaBusMutex&) = delete;
    IsaBusMutex& operator=(const IsaBusMutex&) = delete;

private:
    HANDLE m_ = nullptr;
    bool owned_ = false;
};

// 风扇合理域：物理上风扇失速 <~300 rpm，未接/停转的调速计数会残留
// 噪声读数（实测 IT8613E 空闲头给出 10-44 rpm 这类噪声）。低于下限的
// 计数视为"无风扇/未转"，诚实不产出，绝不用 0 顶替。
constexpr int kFanRpmMin = 120;
constexpr int kFanRpmMax = 8000;

// SuperIO 配置态 RAII 守卫（V26 P1-1）：构造后无论哪条路径离开作用域
// （正常填充、find_bars 失败、厂商号不符、未知芯片 continue），都会补发
// 对应家族的退出序列——SuperIO 绝不残留配置态（残留会让下次探测的进入
// 魔数落进配置索引寄存器，等效乱写）。已显式退出后调用 ExitNow 解除。
class SuperioConfigGuard {
public:
    enum Family { kNone, kNuvoton, kIt87 };
    SuperioConfigGuard(HANDLE h, uint16_t regPort, Family family)
        : h_(h), regPort_(regPort), family_(family) {}
    ~SuperioConfigGuard() { ExitNow(); }
    SuperioConfigGuard(const SuperioConfigGuard&) = delete;
    SuperioConfigGuard& operator=(const SuperioConfigGuard&) = delete;
    void ExitNow() {
        if (family_ == kNuvoton) {
            NuvotonExit(h_, regPort_);
        } else if (family_ == kIt87) {
            It87Exit(h_, regPort_);
        }
        family_ = kNone;
    }

private:
    HANDLE h_;
    uint16_t regPort_;
    Family family_;
};

// 枚举两个插槽做芯片识别与只读传感读取（在 ISA 总线互斥体内调用）。
// 进入/退出由 SuperioConfigGuard 保证配对（V26 P1-1）。
LpcResult LpcProbe(HANDLE h) {
    LpcResult res;
    for (int slot = 0; slot < 2 && !res.chipKnown; ++slot) {
        const uint16_t regPort = kRegisterPorts[slot];
        if (!LpcSelectSlot(h, slot)) continue;

        // --- Nuvoton/Winbond/Fintek 家族（0x87 0x87 进入；LHM 探测顺序在前）---
        NuvotonEnter(h, regPort);
        SuperioConfigGuard nctGuard(h, regPort, SuperioConfigGuard::kNuvoton);
        uint8_t id = 0;
        uint8_t rev = 0;
        if (LpcSuperioInb(h, kRegChipId, &id) && LpcSuperioInb(h, kRegChipRev, &rev)) {
            const uint16_t code = static_cast<uint16_t>((id << 8) | rev);
            for (const NctChip& chip : kNctChips) {
                if (chip.code != code) continue;
                res.chipKnown = true;
                res.chipName = chip.name;
                if (!LpcFindBars(h)) break;  // 退出序列由 nctGuard 补发
                // 硬件监控 LDN = 0x0B，基址在 0x60 字（LHM WINBOND_NUVOTON_HARDWARE_MONITOR_LDN）
                LpcSuperioOutb(h, kRegDeviceSelect, 0x0B);
                uint16_t base = 0;
                LpcSuperioInw(h, kRegBaseAddress, &base);
                nctGuard.ExitNow();  // 读基址后退出配置态，再走 BAR 只读
                if (base < 0x100 || (base & 0xF007) != 0) break;  // 基址非法（LHM 同款校验）
                // 厂商号：bank0 0x004F = 0xA3 且 bank0x80 0x804F = 0x5C（0x5CA3）
                uint8_t lo = 0;
                uint8_t hi = 0;
                if (!NctEnvRead(h, base, 0x004F, &lo) || !NctEnvRead(h, base, 0x804F, &hi) ||
                    lo != 0xA3 || hi != 0x5C) {
                    break;  // 厂商号不符：不猜，诚实放弃
                }
                if (chip.rpm16Bit) {
                    // NCT6771F/6776F：0x656+i*2 大端直读 RPM（LHM _fanRpmRegister）
                    for (int i = 0; i < 5 && res.fanCount < 7; ++i) {
                        const uint16_t reg = static_cast<uint16_t>(0x656 + (i << 1));
                        uint8_t hiB = 0;
                        uint8_t loB = 0;
                        if (!NctEnvRead(h, base, reg, &hiB) ||
                            !NctEnvRead(h, base, static_cast<uint16_t>(reg + 1), &loB)) {
                            break;
                        }
                        const int rpm = (hiB << 8) | loB;
                        if (rpm >= kFanRpmMin && rpm <= kFanRpmMax) {
                            res.fans[res.fanCount++] = rpm;
                        }
                    }
                } else {
                    // NCT6779D/679xD：13 位计数 0x4B0+i*2，RPM = 1.35e6/count
                    for (int i = 0; i < 7 && res.fanCount < 7; ++i) {
                        const uint16_t reg = static_cast<uint16_t>(0x4B0 + (i << 1));
                        uint8_t hiB = 0;
                        uint8_t loB = 0;
                        if (!NctEnvRead(h, base, reg, &hiB) ||
                            !NctEnvRead(h, base, static_cast<uint16_t>(reg + 1), &loB)) {
                            break;
                        }
                        const int count = (hiB << 5) | (loB & 0x1F);
                        if (count >= 0x15 && count < 0x1FFF) {  // LHM _minFanCount/_maxFanCount
                            const int rpm = static_cast<int>(1350000.0 / count);
                            if (rpm >= kFanRpmMin && rpm <= kFanRpmMax) {
                                res.fans[res.fanCount++] = rpm;
                            }
                        }
                    }
                }
                // 电压：0.008 V/LSB @ [0x020..0x026, 0x550, 0x551]（LHM _voltageRegisters）
                const uint16_t voltRegs[9] = {0x020, 0x021, 0x022, 0x023,
                                              0x024, 0x025, 0x026, 0x550, 0x551};
                for (const uint16_t reg : voltRegs) {
                    uint8_t v = 0;
                    if (!NctEnvRead(h, base, reg, &v)) break;
                    const double volts = v * 0.008;
                    if (volts > 0.0 && volts < 3.0) {
                        res.volts[res.voltCount++] = volts;
                    }
                }
                break;
            }
            if (res.chipKnown) return res;  // nctGuard 已 ExitNow / 或析构补退出
        }
        // 未识别：nctGuard 析构补发 Nuvoton 退出序列（原显式 NuvotonExit 移除）

        // --- ITE 家族（0x87 0x01 0x55 0x55/0xAA 进入；LHM 探测顺序在后）---
        It87Enter(h, regPort);
        SuperioConfigGuard iteGuard(h, regPort, SuperioConfigGuard::kIt87);
        uint16_t iteId = 0;
        if (LpcSuperioInw(h, kRegChipId, &iteId)) {
            const It87Chip* chip = nullptr;
            for (const It87Chip& c : kIt87Chips) {
                if (c.id == iteId) {
                    chip = &c;
                    break;
                }
            }
            if (chip == nullptr) {
                continue;  // 未识别：iteGuard 析构补发退出序列，下一插槽
            }
            res.chipKnown = true;
            res.chipName = chip->name;
            if (LpcFindBars(h)) {
                // 环境控制器 LDN = 0x04，基址在 0x60 字（LHM IT87_ENVIRONMENT_CONTROLLER_LDN）
                LpcSuperioOutb(h, kRegDeviceSelect, 0x04);
                uint16_t base = 0;
                LpcSuperioInw(h, kRegBaseAddress, &base);
                iteGuard.ExitNow();  // 读基址后退出配置态
                if (base < 0x100 || (base & 0xF007) != 0) {
                    return res;  // 基址非法：诚实放弃（已退出）
                }
                uint8_t vendor = 0;
                bool valid = false;
                uint8_t cfg = 0;
                bool cfgValid = false;
                // 厂商寄存器 0x58 ∈ {0x90, 0x7F}；配置 0x00 的 bit4 恒 1（LHM 校验）
                if (!ItEnvRead(h, base, 0x58, &vendor, &valid) ||
                    !(vendor == 0x90 || vendor == 0x7F)) {
                    return res;
                }
                if (!ItEnvRead(h, base, 0x00, &cfg, &cfgValid) ||
                    (cfgValid && (cfg & 0x10) == 0)) {
                    return res;
                }
                // 风扇：16 位计数 @ {0x0d,0x0e,0x0f}（LHM FAN_TACHOMETER_REG 前三路）
                constexpr uint8_t kFanLo[3] = {0x0d, 0x0e, 0x0f};
                for (int i = 0; i < 3; ++i) {
                    uint8_t lo = 0;
                    uint8_t hi = 0;
                    bool okLo = false;
                    bool okHi = false;
                    if (!ItEnvRead(h, base, kFanLo[i], &lo, &okLo) ||
                        !ItEnvRead(h, base, static_cast<uint8_t>(kFanLo[i] + 1), &hi, &okHi)) {
                        break;
                    }
                    const int cnt = lo | (hi << 8);
                    if (cnt > 0x3F && cnt < 0xFFFF) {  // LHM：计数 >0x3F 才有效
                        const int rpm = static_cast<int>(1350000.0 / (cnt * 2));
                        if (rpm >= kFanRpmMin && rpm <= kFanRpmMax) {
                            res.fans[res.fanCount++] = rpm;
                        }
                    }
                }
                // 电压：vin @ 0x20..0x28 × 芯片增益
                for (uint8_t reg = 0x20; reg <= 0x28; ++reg) {
                    uint8_t v = 0;
                    bool ok = false;
                    if (!ItEnvRead(h, base, reg, &v, &ok)) break;
                    const double volts = v * chip->voltageGain;
                    if (volts > 0.0 && volts < 5.0) {
                        res.volts[res.voltCount++] = volts;
                    }
                }
            }
            return res;
        }
        // id 读取失败：iteGuard 析构补发退出序列
    }
    return res;
}

// 带短 TTL 缓存的会话执行：风扇+电压同 tick 共享一次探测，避免重复总线占用。
struct LpcOutcome {
    int status = 0;  // 0=完成（可能无读数）；-1 不可用；-2 模块缺失；-3 总线/设备失败
    LpcResult res;
};

// 10s TTL：find_bars 一次 ≈1500 次 IOCTL（实测 ~160ms），低于采集节奏的
// 话会把半数 tick 拖过延迟预算（V26 复验中 collect_tick_latency 因此失败）。
// 风扇/电压为尽力而为读数，传感器页刷新间隔本就 ≥10s——10s TTL 与之相称。
constexpr ULONGLONG kLpcCacheTtlMs = 10000;

LpcOutcome RunLpcProbeOnce() {
    static std::mutex cm;
    static LpcOutcome cached;
    static ULONGLONG cachedAt = 0;
    static bool cachedValid = false;

    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(cm);
        if (cachedValid && now - cachedAt < kLpcCacheTtlMs) {
            return cached;
        }
    }

    LpcOutcome oc;
    do {
        if (!PawnIoAvailable()) {
            oc.status = -1;
            break;
        }
        std::wstring detail;
        if (!PawnIoLoadModuleFromFile(kLpcModuleName, &detail)) {
            STM_LOG_WARN("pawnio", L"LpcIO 模块不可用：{}",
                         detail.empty() ? std::wstring(L"加载失败") : detail);
            oc.status = -2;
            break;
        }
        const HANDLE h = ModuleHandle(kLpcModuleName);
        if (h == nullptr) {
            oc.status = -2;
            break;
        }
        IsaBusMutex bus(5000);
        if (!bus.Owned()) {
            STM_LOG_WARN("pawnio", L"未取得 ISA 总线互斥体（Access_ISABUS.HTP.Method 被占）");
            oc.status = -3;
            break;
        }
        oc.res = LpcProbe(h);
        std::lock_guard<std::mutex> lk(g_mu);
        g_lpcChipName = oc.res.chipName;
    } while (false);

    std::lock_guard<std::mutex> lk(cm);
    cached = oc;
    cachedAt = now;
    cachedValid = true;
    return oc;
}

}  // namespace

// ---- 公共 API ---------------------------------------------------------------

PawnIoStatus GetPawnIoStatus() {
    // 首次探测：打开设备 + VERSION。进程内缓存，安装驱动后需重启进程。
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_probed) {
        g_probed = true;
        HANDLE h = OpenDevice();
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD gle = GetLastError();
            g_probeDetail =
                gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND
                    ? L"PawnIO 驱动未安装或服务未运行（\\Device\\PawnIO 不存在）。"
                      L"官方安装包（由用户自行安装，本应用不代装）：" + std::wstring(kUrlSetup)
                    : gle == ERROR_ACCESS_DENIED
                          ? L"PawnIO 设备拒绝访问（权限不足）"
                          : L"PawnIO 设备打开失败";
            wchar_t extra[32] = {};
            swprintf_s(extra, L"（Win32 错误 %lu）", gle);
            g_probeDetail += extra;
            STM_LOG_INFO("pawnio", L"{}", g_probeDetail);
        } else {
            // 驱动要求 VERSION 出参恰好为 sizeof(ULONG)（driver.cpp：
            // OutputBufferLength != sizeof(ULONG) -> STATUS_INVALID_PARAMETER）。
            ULONG out = 0;
            DWORD bytes = 0;
            if (DeviceIoControl(h, kPawnIoVersion, nullptr, 0, &out, sizeof(out), &bytes,
                                nullptr) &&
                bytes == sizeof(out)) {
                g_driverVersion = out;
                g_driverOpenable = out >= 0x020000;
                wchar_t ver[48] = {};
                swprintf_s(ver, L"PawnIO 驱动就绪（版本 %u.%u.%u）", (out >> 16) & 0xFFFF,
                           (out >> 8) & 0xFF, out & 0xFF);
                g_probeDetail = ver;
                STM_LOG_INFO("pawnio", L"{}", g_probeDetail);
            } else {
                g_probeDetail = L"PawnIO 设备存在但 VERSION 查询失败";
                STM_LOG_WARN("pawnio", L"{}（GLE={}）", g_probeDetail, GetLastError());
            }
            CloseHandle(h);
        }
    }
    PawnIoStatus s;
    s.probed = g_probed;
    s.driverOpenable = g_driverOpenable;
    s.driverVersion = g_driverVersion;
    s.msrLoaded = g_modules.count(kMsrModuleName) != 0;
    s.lpcLoaded = g_modules.count(kLpcModuleName) != 0;
    s.detail = g_probeDetail;
    return s;
}

bool PawnIoAvailable() {
    const PawnIoStatus s = GetPawnIoStatus();
    return s.driverOpenable && s.driverVersion >= 0x020000;
}

bool PawnIoLoadModule(const uint8_t* blob, size_t size, const char* name) {
    if (blob == nullptr || name == nullptr || *name == '\0') return false;
    if (size < 16 || size > kMaxBlobBytes) {
        STM_LOG_WARN("pawnio", L"模块 {} blob 尺寸非法（{} 字节）", Utf8ToWide(name),
                     static_cast<unsigned long long>(size));
        return false;
    }
    if (!PawnIoAvailable()) return false;

    HANDLE h = OpenDevice();
    if (h == INVALID_HANDLE_VALUE) {
        STM_LOG_WARN("pawnio", L"打开设备失败（GLE={}），无法加载模块 {}", GetLastError(),
                     Utf8ToWide(name));
        return false;
    }
    DWORD bytes = 0;
    // 未签名/签名不符的 blob 会被驱动拒绝（错误码来自驱动校验）。
    const BOOL ok = DeviceIoControl(h, kPawnIoLoadBinary, const_cast<uint8_t*>(blob),
                                    static_cast<DWORD>(size), nullptr, 0, &bytes, nullptr);
    if (!ok) {
        const DWORD gle = GetLastError();
        CloseHandle(h);
        STM_LOG_WARN("pawnio", L"IOCTL_PIO_LOAD_BINARY 失败（GLE={}），模块 {}", gle,
                     Utf8ToWide(name));
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_modules.find(name);
    if (it != g_modules.end()) {
        // V26 P2：同名重载不立即 Close——在途调用可能刚从 ModuleHandle 取走旧
        // 句柄；先退役（保持打开），延迟到 PawnIoShutdown 统一回收，
        // 杜绝 use-after-close / 句柄值复用。
        if (it->second != nullptr) g_retired.push_back(it->second);
        it->second = h;
    } else {
        g_modules.emplace(name, h);
    }
    STM_LOG_INFO("pawnio", L"模块 {} 加载成功（{} 字节）", Utf8ToWide(name),
                 static_cast<unsigned long long>(size));
    return true;
}

bool PawnIoLoadModuleFromFile(const char* name, std::wstring* detail) {
    if (name == nullptr || *name == '\0') return false;
    if (ModuleHandle(name) != nullptr) return true;  // 已加载：幂等

    // 搜索顺序（文件头"获取策略"）：LOCALAPPDATA 模块目录 → exe 目录\modules。
    const std::wstring wideName = Utf8ToWide(name);
    std::wstring paths[2];
    wchar_t local[MAX_PATH] = {};
    const DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (ln > 0 && ln < MAX_PATH) {
        paths[0] = std::wstring(local) + L"\\SuperTaskMgr\\modules\\" + wideName + L".bin";
    }
    wchar_t exe[MAX_PATH] = {};
    const DWORD en = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (en > 0 && en < MAX_PATH) {
        std::wstring dir(exe);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash);
        paths[1] = dir + L"\\modules\\" + wideName + L".bin";
    }

    std::wstring searched;
    for (const std::wstring& path : paths) {
        if (path.empty()) continue;
        if (!searched.empty()) searched += L"；";
        searched += path;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER sz{};
        std::vector<uint8_t> blob;
        bool readOk = false;
        if (GetFileSizeEx(f, &sz) && sz.QuadPart > 16 &&
            sz.QuadPart <= static_cast<LONGLONG>(kMaxBlobBytes)) {
            blob.resize(static_cast<size_t>(sz.QuadPart));
            DWORD got = 0;
            readOk = ReadFile(f, blob.data(), static_cast<DWORD>(blob.size()), &got, nullptr) &&
                     got == blob.size();
        }
        CloseHandle(f);
        if (!readOk) continue;
        // V26 P2：SHA-256 白名单——文件在但哈希不在官方白名单 → 拒载（诚实报告）。
        std::wstring hex;
        if (!Sha256Hex(blob.data(), blob.size(), &hex)) {
            if (detail != nullptr) {
                *detail = L"模块文件 SHA-256 计算失败，已拒绝加载：" + path;
            }
            return false;
        }
        if (!HashWhitelisted(name, hex)) {
            STM_LOG_WARN("pawnio", L"模块 {} 哈希不在官方白名单（sha256={}），拒绝加载：{}",
                         Utf8ToWide(name), hex, path);
            if (detail != nullptr) {
                *detail = L"模块文件 SHA-256 与官方白名单不符，已拒绝加载"
                          L"（请从官方渠道重新获取 0.2.11 版模块）：" + path;
            }
            return false;
        }
        if (PawnIoLoadModule(blob.data(), blob.size(), name)) {
            return true;
        }
        // 文件在、哈希对但驱动拒载：诚实报告，不试下一路径。
        if (detail != nullptr) {
            *detail = L"模块文件存在但驱动拒绝加载（签名/版本不符？）：" + path;
        }
        return false;
    }
    if (detail != nullptr) {
        *detail = L"未找到官方签名模块文件（官方渠道：" + std::wstring(kUrlModulesRelease) +
                  L" 或 " + kUrlModulesLhm + L"）：" + searched;
    }
    return false;
}

bool PawnIoExecute(const char* fn, uint64_t arg, uint64_t* out) {
    const HANDLE h = ModuleHandle(kMsrModuleName);
    if (h == nullptr) return false;
    return ExecuteN(h, fn, &arg, 1, out, out != nullptr ? 1 : 0, nullptr);
}

bool PawnIoExecuteN(const char* module, const char* fn, const uint64_t* in,
                    size_t inCount, uint64_t* out, size_t outMax, size_t* outCount) {
    const HANDLE h = ModuleHandle(module);
    if (h == nullptr) return false;
    return ExecuteN(h, fn, in, inCount, out, outMax, outCount);
}

int ReadCpuDtsTemps(int* outTemps, int* outLpIndex, int maxCores) {
    if (outTemps == nullptr || maxCores <= 0) return -1;
    if (!PawnIoAvailable()) return -1;
    std::wstring detail;
    if (!PawnIoLoadModuleFromFile(kMsrModuleName, &detail)) {
        STM_LOG_WARN("pawnio", L"IntelMSR 模块不可用：{}",
                     detail.empty() ? std::wstring(L"加载失败") : detail);
        return -2;
    }
    const HANDLE h = ModuleHandle(kMsrModuleName);
    if (h == nullptr) return -2;

    // 逻辑核枚举：GetLogicalProcessorInformationEx——与 CPUID 0xB 叶子等价的
    // 线程/核映射，由内核保证与处理器组/混合架构（P/E 核）一致。
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return -3;
    std::vector<uint8_t> buf(size);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                                              buf.data()),
                                          &size)) {
        return -3;
    }

    GROUP_AFFINITY oldAffinity{};
    GetThreadGroupAffinity(GetCurrentThread(), &oldAffinity);
    int count = 0;
    DWORD offset = 0;
    bool anyRead = false;
    while (offset < size && count < maxCores) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            buf.data() + offset);
        if (entry->Size == 0) break;
        offset += entry->Size;
        if (entry->Relationship != RelationProcessorCore) continue;
        const WORD groupCount = entry->Processor.GroupCount > 0 ? entry->Processor.GroupCount : 1;
        for (WORD g = 0; g < groupCount && count < maxCores; ++g) {
            const GROUP_AFFINITY& ga = entry->Processor.GroupMask[g];
            ULONG_PTR mask = ga.Mask;
            // 系统级逻辑处理器号 = 组号*64 + 组内位号（Windows 全局编号规则；
            // 组内位号即该位代表的处理器索引——掩码可为稀疏，如 0xc = LP2,3）。
            const int groupBase = static_cast<int>(ga.Group) * 64;
            int bit = 0;
            while (mask != 0 && count < maxCores) {
                if (mask & 1) {
                    GROUP_AFFINITY target{};
                    target.Group = ga.Group;
                    target.Mask = static_cast<ULONG_PTR>(1) << bit;
                    if (SetThreadGroupAffinity(GetCurrentThread(), &target, nullptr)) {
                        // 绑核后：0x19C [22:16] = DTS 数字读出；0x1A2 [23:16] = TjMax
                        uint64_t therm = 0;
                        uint64_t targetReg = 0;
                        const uint64_t msr19c = 0x19C;
                        const uint64_t msr1a2 = 0x1A2;
                        if (ExecuteN(h, "ioctl_read_msr", &msr19c, 1, &therm, 1, nullptr) &&
                            ExecuteN(h, "ioctl_read_msr", &msr1a2, 1, &targetReg, 1, nullptr)) {
                            const int dts = static_cast<int>((therm >> 16) & 0x7F);
                            const int tjMax = static_cast<int>((targetReg >> 16) & 0xFF);
                            const int temp = tjMax - dts;
                            anyRead = true;
                            if (tjMax > 0 && temp >= 0 && temp <= 120) {
                                outTemps[count] = temp;
                                if (outLpIndex != nullptr) {
                                    outLpIndex[count] = groupBase + bit;  // 真实 LP 号
                                }
                                ++count;  // 滤掉的核不占输出槽位
                            }
                        }
                        SetThreadGroupAffinity(GetCurrentThread(), &oldAffinity, nullptr);
                    }
                }
                mask >>= 1;
                ++bit;
            }
        }
    }
    SetThreadGroupAffinity(GetCurrentThread(), &oldAffinity, nullptr);
    if (count == 0) {
        return anyRead ? 0 : -3;  // 有应答但全部不合理 -> 0；完全失败 -> -3
    }
    return count;
}

int ReadLpcIoFans(int* rpms, int max) {
    if (rpms == nullptr || max <= 0) return -1;
    const LpcOutcome oc = RunLpcProbeOnce();
    if (oc.status != 0) return oc.status;
    const int n = std::min<int>(oc.res.fanCount, max);
    for (int i = 0; i < n; ++i) rpms[i] = oc.res.fans[i];
    return n;
}

int ReadLpcIoVoltages(double* volts, int max) {
    if (volts == nullptr || max <= 0) return -1;
    const LpcOutcome oc = RunLpcProbeOnce();
    if (oc.status != 0) return oc.status;
    const int n = std::min<int>(oc.res.voltCount, max);
    for (int i = 0; i < n; ++i) volts[i] = oc.res.volts[i];
    return n;
}

std::wstring LpcIoChipName() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_lpcChipName;
}

void PawnIoShutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    int closed = 0;
    for (auto& kv : g_modules) {
        if (kv.second != nullptr) {
            CloseHandle(kv.second);
            kv.second = nullptr;
            ++closed;
        }
    }
    // V26 P2：退役句柄（重载后遗留）在此统一回收——在途调用此刻必然已结束
    //（调用方契约：进程退出/采集线程停止后调用）。
    for (HANDLE h : g_retired) {
        if (h != nullptr) {
            CloseHandle(h);
            ++closed;
        }
    }
    g_retired.clear();
    STM_LOG_INFO("pawnio", L"PawnIoShutdown：已释放 {} 个执行器句柄", closed);
}

}  // namespace pawnio
}  // namespace stm

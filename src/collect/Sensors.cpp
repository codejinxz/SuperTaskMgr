// Sensors.cpp — phase 3 (contract Sensors.h). Every reading follows the honest
// trichotomy of R6: Ok = real value from a documented user-mode source;
// NeedAdmin = source exists but is elevation-gated; NeedDriver = unreachable in
// user mode (fan speeds — no kernel driver ships with this app); NoHardware =
// adapter/disk present but this sensor absent. NEVER a 0 in place of data.
//
//   CPU frequency  CallNtPowerInformation(ProcessorInformation=11)  documented, no admin
//   CPU package T  WMI root\WMI\MSAcpi_ThermalZoneTemperature     admin on this box
//   GPU temp/util  nvml.dll from System32 (driver-supplied) when present; otherwise the
//                  gpu vector stays EMPTY with an honesty note (IGCL skipped: complex
//                  interface, see R6 §4). No fake readings for Intel iGPU boxes.
//   Disks          MSFT_PhysicalDisk (coarse health) + NVMe health log page 0x02 /
//                  ATA SMART via SMART_RCV_DRIVE_DATA for temperature + power-on hours.
//   Fans           NeedDriver, always.
//
// Blocking call: run on the ops job queue. Never throws; partial results allowed.
#include "collect/Sensors.h"
#include "core/HandleGuard.h"
#include "core/Log.h"
#include "core/Str.h"
#include <winioctl.h>   // SMART_* / STORAGE_* ioctls (also defines DEVICE_TYPE)
#include <ntddstor.h>   // StorageDeviceProtocolSpecificProperty + NVMe log page
#include <objbase.h>
#include <powrprof.h>
#include <wbemidl.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>

namespace stm {

namespace {

// ===========================================================================
// dynamic helpers (libs not in the link line: oleaut32, powrprof, ws2_32-free)
// ===========================================================================
using SysAllocStringFn = BSTR(STDAPICALLTYPE*)(const OLECHAR*);
using SysFreeStringFn = void(STDAPICALLTYPE*)(BSTR);
using CallNtPowerInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PVOID, ULONG);

SysAllocStringFn SysAlloc() {
    static SysAllocStringFn fn = []() -> SysAllocStringFn {
        const HMODULE h = ::LoadLibraryW(L"oleaut32.dll");
        return h ? reinterpret_cast<SysAllocStringFn>(::GetProcAddress(h, "SysAllocString"))
                 : nullptr;
    }();
    return fn;
}

SysFreeStringFn SysFree() {
    static SysFreeStringFn fn = []() -> SysFreeStringFn {
        const HMODULE h = ::GetModuleHandleW(L"oleaut32.dll");
        return h ? reinterpret_cast<SysFreeStringFn>(::GetProcAddress(h, "SysFreeString"))
                 : nullptr;
    }();
    return fn;
}

CallNtPowerInformationFn CallNtPower() {
    static CallNtPowerInformationFn fn = []() -> CallNtPowerInformationFn {
        const HMODULE h = ::LoadLibraryW(L"powrprof.dll");
        return h ? reinterpret_cast<CallNtPowerInformationFn>(
                       ::GetProcAddress(h, "CallNtPowerInformation"))
                 : nullptr;
    }();
    return fn;
}

// Scoped BSTR (needs oleaut32, bound above).
struct Bs {
    BSTR b = nullptr;
    explicit Bs(const wchar_t* s) {
        const SysAllocStringFn f = SysAlloc();
        if (s && f) b = f(s);
    }
    ~Bs() {
        const SysFreeStringFn f = SysFree();
        if (b && f) f(b);
    }
    Bs(const Bs&) = delete;
    Bs& operator=(const Bs&) = delete;
    BSTR get() const { return b; }
};

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    void Reset() {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
    T** pp() { return &p; }
    T* operator->() const { return p; }
    T* get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// Local GUIDs (wbemuuid.lib is not linked).
constexpr GUID kCLSID_WbemLocator = {
    0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
constexpr GUID kIID_IWbemLocator = {
    0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};

// Local WBEM constants (values per wbemcli.h; named k* to avoid #define drift).
constexpr HRESULT kWbemAccessDenied = 0x80041003;     // WBEM_E_ACCESS_DENIED
constexpr HRESULT kWbemNotFound = 0x80041002;         // WBEM_E_NOT_FOUND
constexpr HRESULT kWbemInvalidClass = 0x80041010;     // WBEM_E_INVALID_CLASS
constexpr HRESULT kWbemInvalidNamespace = 0x8004100E; // WBEM_E_INVALID_NAMESPACE
constexpr HRESULT kEAccessDenied = 0x80070005;        // E_ACCESSDENIED (DCM level)
constexpr long kFlagForwardOnly = 0x10;               // WBEM_FLAG_FORWARD_ONLY
constexpr long kFlagReturnImmediately = 0x20;         // WBEM_FLAG_RETURN_IMMEDIATELY

void EnsureComSecurity() {
    // Must run at most once per process; RPC_E_TOO_LATE (already set by
    // somebody else) is fine. We only assert the WMI-friendly defaults.
    static const bool done = []() {
        (void)::CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                     RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
        return true;
    }();
    (void)done;
}

// ===========================================================================
// minimal WMI query helper
// ===========================================================================
struct WmiVal {
    bool present = false;
    bool isNum = false;
    std::wstring str;
    uint32_t num = 0;
};
using WmiRow = std::map<std::wstring, WmiVal>;

struct WmiResult {
    bool ok = false;            // executed; rows (possibly empty) are valid
    bool denied = false;        // elevation-gated -> NeedAdmin
    bool notSupported = false;  // namespace/class absent -> NoHardware
    std::vector<WmiRow> rows;
};

void ClassifyWmiHr(HRESULT hr, WmiResult* r) {
    if (hr == kWbemAccessDenied || hr == kEAccessDenied) {
        r->denied = true;
    } else if (hr == kWbemInvalidNamespace || hr == kWbemInvalidClass || hr == kWbemNotFound) {
        r->notSupported = true;
    } else {
        STM_LOG_WARN("sensors", Fmt(L"WMI 查询失败 hr=0x{:08X}", static_cast<unsigned>(hr)));
    }
}

WmiVal ReadProp(IWbemClassObject* obj, const wchar_t* name) {
    WmiVal v;
    VARIANT var{};  // zero-init == VariantInit (oleaut32 is not linked)
    var.vt = VT_EMPTY;
    if (FAILED(obj->Get(name, 0, &var, nullptr, nullptr))) return v;
    if (var.vt == VT_BSTR && var.bstrVal) {
        v.present = true;
        v.str = var.bstrVal;
    } else if (var.vt == VT_I4) {
        v.present = v.isNum = true;
        v.num = static_cast<uint32_t>(var.lVal);
    } else if (var.vt == VT_UI4) {
        v.present = v.isNum = true;
        v.num = var.ulVal;
    } else if (var.vt == VT_I2) {
        v.present = v.isNum = true;
        v.num = static_cast<uint32_t>(static_cast<uint16_t>(var.iVal));
    } else if (var.vt == VT_UI2) {
        v.present = v.isNum = true;
        v.num = var.uiVal;
    } else if (var.vt == VT_UI1) {
        v.present = v.isNum = true;
        v.num = var.bVal;
    }
    if (var.vt == VT_BSTR && var.bstrVal) {
        const SysFreeStringFn f = SysFree();
        if (f) f(var.bstrVal);
    }
    return v;
}

// One query, `props` fetched per row. Row cap keeps a wedged provider bounded.
WmiResult WmiQuery(const wchar_t* ns, const wchar_t* wql,
                   const std::vector<const wchar_t*>& props) {
    WmiResult r;
    const HRESULT ci = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE: COM is up in STA on this thread — usable, but we do
    // NOT own the init and must not pair a CoUninitialize for it.
    const bool ownInit = SUCCEEDED(ci);
    if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) {
        STM_LOG_WARN("sensors", Fmt(L"CoInitializeEx 失败 hr=0x{:08X}", static_cast<unsigned>(ci)));
        return r;
    }
    struct CoGuard {
        bool own;
        ~CoGuard() {
            if (own) ::CoUninitialize();
        }
    } guard{ownInit};

    EnsureComSecurity();

    ComPtr<IWbemLocator> loc;
    HRESULT hr = ::CoCreateInstance(kCLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    kIID_IWbemLocator, reinterpret_cast<void**>(loc.pp()));
    if (FAILED(hr) || !loc) {
        ClassifyWmiHr(hr, &r);
        return r;
    }
    Bs bns(ns);
    ComPtr<IWbemServices> svc;
    hr = loc->ConnectServer(bns.get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, svc.pp());
    if (FAILED(hr) || !svc) {
        ClassifyWmiHr(hr, &r);
        return r;
    }
    hr = ::CoSetProxyBlanket(svc.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                             RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                             EOAC_NONE);
    if (FAILED(hr)) {
        ClassifyWmiHr(hr, &r);
        return r;
    }
    Bs blang(L"WQL");
    Bs bwql(wql);
    ComPtr<IEnumWbemClassObject> en;
    hr = svc->ExecQuery(blang.get(), bwql.get(), kFlagForwardOnly | kFlagReturnImmediately,
                        nullptr, en.pp());
    if (FAILED(hr) || !en) {
        ClassifyWmiHr(hr, &r);
        return r;
    }
    for (int i = 0; i < 64; ++i) {
        ComPtr<IWbemClassObject> obj;
        ULONG got = 0;
        if (en->Next(5000, 1, obj.pp(), &got) != S_OK || got == 0) break;
        WmiRow row;
        for (const wchar_t* name : props) row[name] = ReadProp(obj.get(), name);
        r.rows.push_back(std::move(row));
    }
    r.ok = true;
    return r;
}

// ===========================================================================
// CPU: frequency (documented, no admin) + package temperature (ACPI thermal zone)
// ===========================================================================
#pragma pack(push, 4)
struct ProcessorPowerInfo {  // PROCESSOR_POWER_INFORMATION (winnt.h, documented)
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};
#pragma pack(pop)
static_assert(sizeof(ProcessorPowerInfo) == 24, "PROCESSOR_POWER_INFORMATION layout");

void ReadCpuFreq(std::vector<SensorReading>* cpu, std::wstring* notes) {
    const CallNtPowerInformationFn fn = CallNtPower();
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    if (!fn || si.dwNumberOfProcessors == 0) {
        *notes += L"；CPU 频率源（CallNtPowerInformation）不可用";
        return;
    }
    std::vector<ProcessorPowerInfo> info(si.dwNumberOfProcessors);
    if (fn(11 /*ProcessorInformation*/, nullptr, 0, info.data(),
           static_cast<ULONG>(info.size() * sizeof(ProcessorPowerInfo))) != 0) {
        *notes += L"；CPU 频率读取失败（CallNtPowerInformation）";
        return;
    }
    int added = 0;
    for (const ProcessorPowerInfo& p : info) {
        if (p.CurrentMhz == 0) continue;  // honest: no reading instead of a fake 0
        SensorReading r;
        r.label = Fmt(L"CPU 核 {} 频率", p.Number);
        r.value = static_cast<double>(p.CurrentMhz);
        r.unit = L"MHz";
        r.state = SensorReading::State::Ok;
        cpu->push_back(std::move(r));
        ++added;
    }
    if (added == 0) {
        SensorReading r;
        r.label = L"CPU 频率";
        r.unit = L"MHz";
        r.state = SensorReading::State::NoHardware;
        cpu->push_back(std::move(r));
    }
}

void ReadCpuTemp(std::vector<SensorReading>* cpu, std::wstring* notes) {
    // MSAcpi_ThermalZoneTemperature: implemented by the ACPI driver; unit is
    // 0.1 K. Non-admin is usually rejected (R6 §3: this box reports 拒绝访问).
    const WmiResult w = WmiQuery(L"ROOT\\WMI", L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature",
                                 {L"CurrentTemperature"});
    if (w.denied) {
        SensorReading r;
        r.label = L"CPU 整包温度（ACPI 热区）";
        r.unit = L"°C";
        r.state = SensorReading::State::NeedAdmin;
        cpu->push_back(r);
        *notes += L"；ACPI 热区温度需管理员权限";
        return;
    }
    if (w.notSupported || w.rows.empty()) {
        SensorReading r;
        r.label = L"CPU 整包温度（ACPI 热区）";
        r.unit = L"°C";
        r.state = SensorReading::State::NoHardware;
        cpu->push_back(r);
        *notes += L"；本机无 ACPI 热区传感器";
        return;
    }
    int added = 0;
    for (const WmiRow& row : w.rows) {
        const auto it = row.find(L"CurrentTemperature");
        if (it == row.end() || !it->second.present || !it->second.isNum) continue;
        const double c = static_cast<double>(it->second.num) / 10.0 - 273.15;
        if (c < -60.0 || c > 250.0) continue;  // implausible -> no fake reading
        SensorReading r;
        r.label = w.rows.size() > 1
                      ? Fmt(L"CPU 整包温度（ACPI 热区 {}）", static_cast<int>(added))
                      : L"CPU 整包温度（ACPI 热区）";
        r.value = c;
        r.unit = L"°C";
        r.state = SensorReading::State::Ok;
        cpu->push_back(std::move(r));
        ++added;
    }
    if (added == 0) {
        SensorReading r;
        r.label = L"CPU 整包温度（ACPI 热区）";
        r.unit = L"°C";
        r.state = SensorReading::State::NoHardware;
        cpu->push_back(r);
        *notes += L"；ACPI 热区返回无效温度值（不显示假数据）";
    }
}

// ===========================================================================
// GPU: NVML only (driver-supplied nvml.dll in System32). Absent -> empty gpu
// vector + honesty note; we never fake values for unsupported vendors.
// ===========================================================================
namespace nvml {

using Device = void*;
constexpr int kRetSuccess = 0;  // nvmlReturn_t NVML_SUCCESS
constexpr int kTempGpu = 0;     // nvmlTemperatureSensors_t NVML_TEMPERATURE_GPU

struct Utilization {  // nvmlUtilization_t
    unsigned gpu;
    unsigned memory;
};

using InitFn = int (*)();
using ShutdownFn = int (*)();
using GetCountFn = int (*)(unsigned*);
using GetHandleFn = int (*)(unsigned, Device*);
using GetTempFn = int (*)(Device, int, unsigned*);
using GetUtilFn = int (*)(Device, Utilization*);
using GetPowerFn = int (*)(Device, unsigned*);  // milliwatts

struct Fns {
    InitFn init = nullptr;
    ShutdownFn shutdown = nullptr;
    GetCountFn count = nullptr;
    GetHandleFn handle = nullptr;
    GetTempFn temp = nullptr;
    GetUtilFn util = nullptr;
    GetPowerFn power = nullptr;
};

// Loads C:\Windows\System32\nvml.dll (driver-owned; absolute path — never a
// search-order load). Returns false with notes filled when unavailable.
bool Load(Fns* f, HMODULE* modOut, std::wstring* notes) {
    *modOut = ::LoadLibraryW(L"C:\\Windows\\System32\\nvml.dll");
    if (!*modOut) {
        *notes += L"；本机无 NVIDIA/驱动组件，GPU 温度需厂商运行时（NVML；IGCL 接口复杂，明确跳过）";
        return false;
    }
    auto proc = [&](const char* name) { return reinterpret_cast<void*>(::GetProcAddress(*modOut, name)); };
    f->init = reinterpret_cast<InitFn>(proc("nvmlInit_v2"));
    if (!f->init) f->init = reinterpret_cast<InitFn>(proc("nvmlInit"));
    f->shutdown = reinterpret_cast<ShutdownFn>(proc("nvmlShutdown"));
    f->count = reinterpret_cast<GetCountFn>(proc("nvmlDeviceGetCount"));
    f->handle = reinterpret_cast<GetHandleFn>(proc("nvmlDeviceGetHandleByIndex_v2"));
    if (!f->handle) f->handle = reinterpret_cast<GetHandleFn>(proc("nvmlDeviceGetHandleByIndex"));
    f->temp = reinterpret_cast<GetTempFn>(proc("nvmlDeviceGetTemperature"));
    f->util = reinterpret_cast<GetUtilFn>(proc("nvmlDeviceGetUtilizationRates"));
    f->power = reinterpret_cast<GetPowerFn>(proc("nvmlDeviceGetPowerUsage"));
    if (!f->init || !f->shutdown || !f->count || !f->handle || !f->temp) {
        *notes += L"；nvml.dll 缺少所需导出，GPU 传感器不可用";
        ::FreeLibrary(*modOut);
        *modOut = nullptr;
        return false;
    }
    if (f->init() != kRetSuccess) {
        *notes += L"；NVML 初始化失败（驱动不兼容？），GPU 传感器不可用";
        ::FreeLibrary(*modOut);
        *modOut = nullptr;
        return false;
    }
    return true;
}

void Read(std::vector<SensorReading>* gpu, std::wstring* notes) {
    Fns f;
    HMODULE mod = nullptr;
    if (!Load(&f, &mod, notes)) return;
    struct NvmlGuard {
        ShutdownFn shutdown;
        HMODULE mod;
        ~NvmlGuard() {
            if (shutdown) shutdown();
            if (mod) FreeLibrary(mod);
        }
    } nvmlGuard{f.shutdown, mod};

    unsigned count = 0;
    if (f.count(&count) != kRetSuccess) {
        *notes += L"；NVML 设备枚举失败";
        return;
    }
    count = std::min<unsigned>(count, 8);
    for (unsigned i = 0; i < count; ++i) {
        Device dev = nullptr;
        if (f.handle(i, &dev) != kRetSuccess) continue;
        unsigned tempC = 0;
        if (f.temp(dev, kTempGpu, &tempC) == kRetSuccess && tempC > 0) {
            SensorReading r;
            r.label = Fmt(L"GPU {} 温度", i);
            r.value = static_cast<double>(tempC);
            r.unit = L"°C";
            r.state = SensorReading::State::Ok;
            gpu->push_back(std::move(r));
        }
        if (f.util) {
            Utilization u{};
            if (f.util(dev, &u) == kRetSuccess) {
                SensorReading r;
                r.label = Fmt(L"GPU {} 利用率", i);
                r.value = static_cast<double>(std::min(u.gpu, 100u));
                r.unit = L"%";
                r.state = SensorReading::State::Ok;
                gpu->push_back(std::move(r));
            }
        }
        if (f.power) {
            unsigned mw = 0;
            if (f.power(dev, &mw) == kRetSuccess && mw > 0) {
                SensorReading r;
                r.label = Fmt(L"GPU {} 功耗", i);
                r.value = static_cast<double>(mw) / 1000.0;
                r.unit = L"W";
                r.state = SensorReading::State::Ok;
                gpu->push_back(std::move(r));
            }
        }
    }
    if (gpu->empty()) *notes += L"；NVML 在线但未报告 GPU 传感器";
    else *notes += L"；GPU 读数来自 NVML；GPU 占用率另见性能页（GpuCollector）";
}

}  // namespace nvml

// ===========================================================================
// Disks: per-PhysicalDrive health, honest at every permission level
// ===========================================================================
enum class BusProto { Nvme, Ata, Usb, Other };

BusProto ClassifyBus(uint32_t busType) {
    // STORAGE_BUS_TYPE: 3=ATA 2=ATAPI 11=SATA 17=NVMe 7=USB ...
    switch (busType) {
        case 17: return BusProto::Nvme;
        case 2:
        case 3:
        case 11: return BusProto::Ata;
        case 7: return BusProto::Usb;
        default: return BusProto::Other;
    }
}

const wchar_t* BusName(uint32_t busType) {
    switch (busType) {
        case 1: return L"SCSI";
        case 2: return L"ATAPI";
        case 3: return L"ATA";
        case 4: return L"IEEE1394";
        case 6: return L"FC";
        case 7: return L"USB";
        case 8: return L"RAID";
        case 9: return L"iSCSI";
        case 10: return L"SAS";
        case 11: return L"SATA";
        case 12: return L"SD";
        case 13: return L"MMC";
        case 14: return L"虚拟";
        case 16: return L"存储空间";
        case 17: return L"NVMe";
        default: return L"未知";
    }
}

std::wstring CStrFromDesc(const BYTE* base, ULONG off) {
    if (off == 0 || off >= 2048) return {};
    const char* s = reinterpret_cast<const char*>(base + off);
    std::string out;
    for (size_t i = 0; i < 1024 && s[i]; ++i) out += s[i];
    // Trim trailing spaces (ATA identify padding).
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return {};
    return Utf8ToWide(out);
}

// IOCTL_STORAGE_QUERY_PROPERTY(StorageDeviceProtocolSpecificProperty) ->
// NVMe Get Log Page 0x02 SMART/Health (documented "Working with NVMe drives").
bool NvmeHealth(HANDLE h, double* tempC, uint64_t* poh, uint32_t* pctUsed, uint8_t* critWarn) {
    constexpr size_t kLog = 512;
    std::vector<BYTE> buf(sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) +
                          kLog);
    auto* q = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buf.data());
    q->PropertyId = StorageDeviceProtocolSpecificProperty;
    q->QueryType = PropertyStandardQuery;
    auto* spd = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(q->AdditionalParameters);
    spd->ProtocolType = ProtocolTypeNvme;
    spd->DataType = NVMeDataTypeLogPage;
    spd->ProtocolDataRequestValue = 0x02;  // SMART / health information log page
    spd->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    spd->ProtocolDataLength = kLog;
    DWORD ret = 0;
    if (!::DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf.data(),
                           static_cast<DWORD>(sizeof(STORAGE_PROPERTY_QUERY) +
                                              sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)),
                           buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr)) {
        return false;
    }
    if (ret < sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + kLog) {
        return false;
    }
    const BYTE* log =
        buf.data() + sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    // NVMe SMART/Health log (512 B, NVMe 1.3/1.4 Get Log Page 02h layout):
    //   [0] Critical Warning  [1-2] Composite Temperature (LE, Kelvin)
    //   [3] Available Spare   [4] Available Spare Threshold
    //   [5] Percentage Used   [32-47] Data Units Read  [128-143] Power On Hours
    // (V9 P0-2: the previous offsets 2-3 / 32-39 read spare%|temp-hi and
    // Data Units Read — wrong fields. Values are clamped to plausible domains;
    // anything outside becomes "unknown" instead of a fake reading.)
    *critWarn = log[0];
    *pctUsed = log[5];
    const uint16_t tempK = static_cast<uint16_t>(log[1] | (log[2] << 8));
    constexpr double kMinC = -20.0, kMaxC = 120.0;
    const double c = static_cast<double>(tempK) - 273.15;
    *tempC = (c >= kMinC && c <= kMaxC) ? c : 0.0;  // 0 => caller reports NoHardware
    uint64_t hours = 0;
    for (int i = 7; i >= 0; --i) hours = (hours << 8) | log[128 + i];  // power on hours @128
    constexpr uint64_t kMaxHours = 200000;  // ~23 years; beyond = garbage
    *poh = (hours <= kMaxHours) ? hours : UINT64_MAX;
    return true;
}

// Classic SMART READ DATA via SMART_RCV_DRIVE_DATA (winioctl.h, ATA drives).
bool AtaSmart(HANDLE h, uint8_t driveIndex, double* tempC, uint64_t* poh) {
    GETVERSIONINPARAMS ver{};
    DWORD ret = 0;
    if (!::DeviceIoControl(h, SMART_GET_VERSION, nullptr, 0, &ver, sizeof(ver), &ret, nullptr)) {
        return false;
    }
    if ((ver.fCapabilities & CAP_SMART_CMD) == 0) return false;

    SENDCMDINPARAMS inp{};
    inp.cBufferSize = 512;
    inp.irDriveRegs.bFeaturesReg = 0xD0;  // SMART READ ATTRIBUTE VALUES
    inp.irDriveRegs.bSectorCountReg = 1;
    inp.irDriveRegs.bSectorNumberReg = 1;
    inp.irDriveRegs.bCylLowReg = 0x4F;
    inp.irDriveRegs.bCylHighReg = 0xC2;
    inp.irDriveRegs.bDriveHeadReg = 0xA0;
    inp.irDriveRegs.bCommandReg = 0xB0;  // SMART command
    inp.bDriveNumber = driveIndex;
    std::vector<BYTE> outb(sizeof(SENDCMDOUTPARAMS) + 512 - 1);
    if (!::DeviceIoControl(h, SMART_RCV_DRIVE_DATA, &inp, sizeof(inp), outb.data(),
                           static_cast<DWORD>(outb.size()), &ret, nullptr)) {
        return false;
    }
    const auto* outp = reinterpret_cast<const SENDCMDOUTPARAMS*>(outb.data());
    if (outp->DriverStatus.bDriverError != 0) return false;
    const BYTE* s = outp->bBuffer;  // 512-byte attribute block
    // Attribute layout (ATA/ATAPI-6 SMART READ DATA; identical in smartmontools
    // ata_smart_attribute and CrystalDiskInfo — the reference our R6 cites):
    //   id(1) flags(2) value(1) worst(1) raw(6, LE) reserved(1); 30 entries after
    //   a 2-byte version. NOTE (V9 P0-3 review): the review proposed raw@p[4]/8B,
    //   but under this spec layout p[4] is the *worst* normalized byte (0-100);
    //   reading it as Celsius would fabricate plausible fake temperatures. We
    //   therefore keep raw@p[5..10] and enforce strict plausibility domains so
    //   any layout drift degrades to "unknown" instead of a fake value.
    bool haveTemp = false, haveHours = false;
    double t = 0;
    uint64_t hours = 0;
    for (int a = 0; a < 30; ++a) {
        const BYTE* p = s + 2 + a * 12;  // version(2) then 12-byte attributes
        const uint8_t id = p[0];
        if (id == 0) break;
        if ((id == 194 || id == 190) && !haveTemp) {  // Temperature / Airflow
            const double cand = static_cast<double>(p[5]);  // raw[0]
            if (cand >= -20.0 && cand <= 120.0) {           // plausible Celsius only
                t = cand;
                haveTemp = true;
            }
        } else if (id == 9 && !haveHours) {  // Power_On_Hours (raw, LE, 48 bit)
            const uint64_t raw = static_cast<uint64_t>(p[5]) | (static_cast<uint64_t>(p[6]) << 8) |
                                 (static_cast<uint64_t>(p[7]) << 16) |
                                 (static_cast<uint64_t>(p[8]) << 24) |
                                 (static_cast<uint64_t>(p[9]) << 32) |
                                 (static_cast<uint64_t>(p[10]) << 40);
            if (raw > 0 && raw <= 200000ull) {  // ~23 years; beyond = garbage
                hours = raw;
                haveHours = true;
            }
        }
    }
    if (!haveTemp && !haveHours) return false;
    *tempC = haveTemp ? t : 0.0;
    *poh = haveHours ? hours : UINT64_MAX;
    return true;
}

void ReadDisks(std::vector<DiskHealth>* disks, std::wstring* notes) {
    // Coarse OS-level health first (works for standard users on stock Windows;
    // StorageReliabilityCounter would need admin — R6 §5).
    struct WmiDisk {
        uint32_t busNum = UINT32_MAX;
        uint16_t healthStatus = 0xFFFF;  // 0xFFFF = unknown
        std::wstring model, serial;
    };
    std::map<std::wstring, WmiDisk> wmi;  // DeviceId ("0", "1", ...) -> info
    const WmiResult w = WmiQuery(
        L"ROOT\\Microsoft\\Windows\\Storage",
        L"SELECT DeviceId, FriendlyName, SerialNumber, BusType, HealthStatus FROM MSFT_PhysicalDisk",
        {L"DeviceId", L"FriendlyName", L"SerialNumber", L"BusType", L"HealthStatus"});
    if (w.denied) *notes += L"；磁盘 WMI（MSFT_PhysicalDisk）需管理员权限";
    else if (!w.ok && !w.notSupported) *notes += L"；磁盘 WMI 查询失败，健康结论可能为未知";
    for (const WmiRow& row : w.rows) {
        const auto id = row.find(L"DeviceId");
        if (id == row.end() || !id->second.present) continue;
        WmiDisk wd;
        const auto bt = row.find(L"BusType");
        if (bt != row.end() && bt->second.present && bt->second.isNum) wd.busNum = bt->second.num;
        const auto hs = row.find(L"HealthStatus");
        if (hs != row.end() && hs->second.present && hs->second.isNum) {
            wd.healthStatus = static_cast<uint16_t>(hs->second.num);
        }
        const auto fn = row.find(L"FriendlyName");
        if (fn != row.end() && fn->second.present) wd.model = fn->second.str;
        const auto sn = row.find(L"SerialNumber");
        if (sn != row.end() && sn->second.present) wd.serial = sn->second.str;
        wmi[id->second.str] = std::move(wd);
    }

    int needAdmin = 0;
    for (uint32_t n = 0; n < 32; ++n) {
        const std::wstring path = Fmt(L"\\\\.\\PhysicalDrive{}", n);
        const HANDLE h0 = ::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
        // CreateFileW fails with INVALID_HANDLE_VALUE (-1), not NULL: comparing
        // against NULL fabricated one fake disk row per absent drive number
        // (V9 P0-1). core/HandleGuard is frozen, so normalize HERE: skip the
        // invalid value and never hand it to UniqueHandle/CloseHandle.
        if (h0 == INVALID_HANDLE_VALUE) continue;  // absent drive; gaps are legal
        const stm::UniqueHandle h(h0);

        DiskHealth d;
        uint32_t busNum = UINT32_MAX;
        const auto wit = wmi.find(std::to_wstring(n));
        if (wit != wmi.end()) {
            busNum = wit->second.busNum;
            d.model = wit->second.model;
            d.serial = wit->second.serial;
            switch (wit->second.healthStatus) {  // MSFT_PhysicalDisk.HealthStatus
                case 0: d.health = L"良好"; break;
                case 1: d.health = L"警告"; break;
                case 2: d.health = L"异常"; break;
                default: d.health = L"未知"; break;
            }
        }
        if (d.health.empty()) d.health = L"未知";
        d.busType = L"未知";
        d.tempC = 0.0;
        d.powerOnHours = UINT64_MAX;
        d.tempState = SensorReading::State::NoHardware;

        // Descriptor: model/serial/bus even where WMI was denied (0-access
        // handle is enough for the plain property query).
        BYTE qbuf[2048]{};
        auto* q = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(qbuf);
        q->PropertyId = StorageDeviceProperty;
        q->QueryType = PropertyStandardQuery;
        DWORD ret = 0;
        if (::DeviceIoControl(h.get(), IOCTL_STORAGE_QUERY_PROPERTY, qbuf,
                              sizeof(STORAGE_PROPERTY_QUERY), qbuf, sizeof(qbuf), &ret,
                              nullptr) &&
            ret >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(qbuf);
            if (d.model.empty()) {
                const std::wstring vendor = CStrFromDesc(qbuf, desc->VendorIdOffset);
                const std::wstring product = CStrFromDesc(qbuf, desc->ProductIdOffset);
                d.model = vendor + L" " + product;
                while (!d.model.empty() && d.model.front() == L' ') d.model.erase(d.model.begin());
            }
            if (d.serial.empty()) d.serial = CStrFromDesc(qbuf, desc->SerialNumberOffset);
            if (busNum == UINT32_MAX) {
                busNum = desc->BusType;
            }
        }
        d.busType = BusName(busNum == UINT32_MAX ? 0 : busNum);
        // Never a blank line: WMI and descriptor both unavailable -> the drive
        // number is the honest fallback name.
        if (d.model.empty()) d.model = Fmt(L"PhysicalDrive{}", n);

        // Detailed health: only for buses that plausibly report SMART.
        const BusProto proto = busNum != UINT32_MAX ? ClassifyBus(busNum) : BusProto::Other;
        if (proto == BusProto::Nvme || proto == BusProto::Ata) {
            bool ok = false;
            double t = 0;
            uint64_t poh = UINT64_MAX;
            uint32_t pct = 0;
            uint8_t crit = 0;
            if (proto == BusProto::Nvme) {
                ok = NvmeHealth(h.get(), &t, &poh, &pct, &crit);
            } else {
                ok = AtaSmart(h.get(), static_cast<uint8_t>(n), &t, &poh);
            }
            if (!ok) {
                // Retry behind an elevated handle; classify by the outcome.
                const HANDLE h1 = ::CreateFileW(path.c_str(), GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                OPEN_EXISTING, 0, nullptr);
                if (h1 != INVALID_HANDLE_VALUE) {  // V9 P0-1: not NULL on failure
                    const stm::UniqueHandle h1g(h1);
                    ok = proto == BusProto::Nvme
                             ? NvmeHealth(h1g.get(), &t, &poh, &pct, &crit)
                             : AtaSmart(h1g.get(), static_cast<uint8_t>(n), &t, &poh);
                } else if (::GetLastError() == ERROR_ACCESS_DENIED) {
                    ++needAdmin;
                    d.tempState = SensorReading::State::NeedAdmin;
                }
            }
            if (ok) {
                d.powerOnHours = poh;
                if (t > 0.0) {
                    d.tempC = t;
                    d.tempState = SensorReading::State::Ok;
                } else {
                    d.tempState = SensorReading::State::NoHardware;  // log ok, no temp sensor
                }
                if (proto == BusProto::Nvme) {
                    if (crit != 0) d.health = L"警告";       // real SMART critical warning
                    else if (pct > 100) d.health = L"警告";  // worn beyond rated endurance
                    else if (d.health == L"未知") d.health = L"良好";
                }
            } else if (d.tempState != SensorReading::State::NeedAdmin) {
                d.tempState = SensorReading::State::NoHardware;
                if (proto == BusProto::Ata) *notes += L"；SATA SMART 读取失败（该盘不支持？）";
                if (proto == BusProto::Nvme) *notes += L"；NVMe 健康日志读取失败";
            }
        } else if (proto == BusProto::Usb) {
            *notes += L"；USB 桥通常不透传 SMART（温度/通电时长显示为不可用）";
        }
        disks->push_back(std::move(d));
    }
    if (needAdmin > 0) {
        *notes += Fmt(L"；{} 块磁盘的 SMART/温度需管理员权限", needAdmin);
    }
}

}  // namespace

SensorSnapshot ReadSensors(std::wstring* err) {
    SensorSnapshot snap;
    std::wstring notes;
    try {
        ReadCpuTemp(&snap.cpu, &notes);
        ReadCpuFreq(&snap.cpu, &notes);
        nvml::Read(&snap.gpu, &notes);
        ReadDisks(&snap.disks, &notes);

        SensorReading fan;
        fan.label = L"风扇转速";
        fan.value = 0.0;
        fan.unit = L"rpm";
        fan.state = SensorReading::State::NeedDriver;
        snap.fans.push_back(fan);
        notes += L"；风扇转速需要内核驱动（本应用不随包分发驱动，见调研 R6）；CPU 占用率见性能页（采集线程每核数据）";
    } catch (const std::exception& e) {
        if (err) *err = Fmt(L"传感器读取异常：{}", Utf8ToWide(e.what()));
        STM_LOG_ERROR("sensors", Fmt(L"ReadSensors 异常：{}", Utf8ToWide(e.what())));
    } catch (...) {
        if (err) *err = L"传感器读取未知异常";
        STM_LOG_ERROR("sensors", L"ReadSensors 未知异常");
    }
    if (notes.size() > 2) notes.erase(0, 1);  // drop the leading '；'
    snap.notes = notes;
    return snap;
}

}  // namespace stm

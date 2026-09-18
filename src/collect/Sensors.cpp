
// Sensors.cpp — phase 3 + F3 extension (contract Sensors.h). Every reading
// follows the honest trichotomy of R6: Ok = real value from a documented
// user-mode source; NeedAdmin = source exists but is elevation-gated;
// NeedDriver = unreachable in user mode (fan speeds — no kernel driver ships
// with this app); NoHardware = adapter/disk present but this sensor absent.
// NEVER a 0 in place of data.
//
//   CPU frequency  CallNtPowerInformation(ProcessorInformation=11)  documented, no admin
//   CPU per-core % PDH \Processor Information(*)\% Processor Time (documented, R6 §6)
//   ACPI thermal   WMI root\WMI\MSAcpi_ThermalZoneTemperature — ONE READING PER
//                  INSTANCE (G-B multi-value: every zone of the instance set,
//                  label "ACPI 热区 N" + InstanceName); admin on this box
//   GPU temp/util  nvml.dll from System32 (driver-supplied) when present; per card
//                  the NVML path expands to per-sensor readings (G-B): temp,
//                  slowdown temp threshold, power W, GPU util, VRAM util (the two
//                  nvmlDeviceGetUtilizationRates components), fan; multi-GPU
//                  enumerated card by card. Absent nvml.dll -> the gpus group
//                  keeps its PDH content with an honesty note (IGCL skipped:
//                  complex interface, see R6 §4). No fake readings anywhere.
//   GPU engine %   PDH \GPU Engine(*)\Utilization Percentage per engtype_* (R6 §4);
//                  VRAM dedicated/shared via \GPU Adapter Memory(*)
//   Network        GetIfTable2 per-adapter octet deltas over a shared 350 ms window
//                  (F3) + link speed; loopback excluded, non-Up adapters skipped
//   Battery        CallNtPowerInformation(SystemBatteryState=5) — NoHardware when absent
//   Memory         GlobalMemoryStatusEx + GetPerformanceInfo (K32 bound dynamically)
//   Disks          MSFT_PhysicalDisk (coarse health) + NVMe health log page 0x02 /
//                  ATA SMART via SMART_RCV_DRIVE_DATA for temperature + power-on hours
//                  + spare/wear/critical-warning detail (F3). One DiskHealth PER
//                  physical drive, never aggregated (G-B: confirmed per-disk).
//   Extra (G-B)    best-effort WMI temperature classes outside the ACPI zone;
//                  empty group = nothing found = not shown. P2 (2026-09-18,
//                  "CPU 温度信息增强") exhausts the documented USER-MODE thermal
//                  sources here, one reading PER INSTANCE, every label prefixed
//                  with its provenance ("WMI 温度 N/（实例）" for Win32_Temperature,
//                  "WMI 热区计数器 N/（实例）" for the PerfProc thermal-zone counter,
//                  "DPTF 温度（参与者）" for the Intel DPTF TEMPERATURE set when its
//                  namespace exists). Per-core DTS temperatures (MSR 0x19C/0x1A2/
//                  0x1B1) stay unreachable by design: they require a kernel
//                  driver and this app ships none (red line) — the sensor page
//                  says so and offers the optional LibreHardwareMonitor bridge.
//   Fans           NeedDriver, always.
//
// Blocking call: run on the ops job queue. Never throws; partial results allowed.
// The F3 delta window adds one ~350 ms sleep per read (page refreshes are >=10 s).
#include <winsock2.h>  // must precede iphlpapi/netioapi (LEAN_AND_MEAN hides winsock)
#include <ws2tcpip.h>  // pulls ws2ipdef.h -> defines _WS2IPDEF_ for netioapi MIB_* decls
#include "collect/Sensors.h"
#include "collect/CollectDetail.h"  // cd:: PDH wildcard helpers (same library)
#include "core/HandleGuard.h"
#include "core/Log.h"
#include "core/Str.h"
#include <windows.h>
#include <iphlpapi.h>   // GetIfTable2 / FreeMibTable (iphlpapi is on the link line)
#include <netioapi.h>   // MIB_IF_TABLE2 / MIB_IF_ROW2
#include <psapi.h>      // PERFORMANCE_INFORMATION (bound dynamically below)
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
using VariantClearFn = HRESULT(STDAPICALLTYPE*)(VARIANT*);
using CallNtPowerInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PVOID, ULONG);
using SafeArrayAccessDataFn = HRESULT(STDAPICALLTYPE*)(SAFEARRAY*, void**);
using SafeArrayUnaccessDataFn = HRESULT(STDAPICALLTYPE*)(SAFEARRAY*);
using SafeArrayGetBoundFn = HRESULT(STDAPICALLTYPE*)(SAFEARRAY*, UINT, LONG*);
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
// V15/P1: uniform VARIANT teardown. VariantClear releases whichever resource
// the property read produced (BSTR, SAFEARRAY incl. its data block, ...) exactly
// once. The VT_ARRAY|VT_UI1 VendorSpecific path previously paired only
// Access/UnaccessData and leaked one SAFEARRAY per row on every refresh.
VariantClearFn VariantClr() {
    static VariantClearFn fn = []() -> VariantClearFn {
        const HMODULE h = ::GetModuleHandleW(L"oleaut32.dll");
        return h ? reinterpret_cast<VariantClearFn>(::GetProcAddress(h, "VariantClear"))
                 : nullptr;
    }();
    return fn;
}
// SAFEARRAY byte access (oleaut32; only the G-B "extra" WMI SMART path needs it).
SafeArrayAccessDataFn SafeArrAccess() {
    static SafeArrayAccessDataFn fn = []() -> SafeArrayAccessDataFn {
        const HMODULE h = ::GetModuleHandleW(L"oleaut32.dll");
        return h ? reinterpret_cast<SafeArrayAccessDataFn>(
                       ::GetProcAddress(h, "SafeArrayAccessData"))
                 : nullptr;
    }();
    return fn;
}
SafeArrayUnaccessDataFn SafeArrUnaccess() {
    static SafeArrayUnaccessDataFn fn = []() -> SafeArrayUnaccessDataFn {
        const HMODULE h = ::GetModuleHandleW(L"oleaut32.dll");
        return h ? reinterpret_cast<SafeArrayUnaccessDataFn>(
                       ::GetProcAddress(h, "SafeArrayUnaccessData"))
                 : nullptr;
    }();
    return fn;
}
SafeArrayGetBoundFn SafeArrLBound() {
    static SafeArrayGetBoundFn fn = []() -> SafeArrayGetBoundFn {
        const HMODULE h = ::GetModuleHandleW(L"oleaut32.dll");
        return h ? reinterpret_cast<SafeArrayGetBoundFn>(
                       ::GetProcAddress(h, "SafeArrayGetLBound"))
                 : nullptr;
    }();
    return fn;
}
SafeArrayGetBoundFn SafeArrUBound() {
    static SafeArrayGetBoundFn fn = []() -> SafeArrayGetBoundFn {
        const HMODULE h = ::GetModuleHandleW(L"oleaut32.dll");
        return h ? reinterpret_cast<SafeArrayGetBoundFn>(
                       ::GetProcAddress(h, "SafeArrayGetUBound"))
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
    std::vector<uint8_t> bytes;  // VT_ARRAY|VT_UI1 (G-B: SMART VendorSpecific)
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
    } else if (var.vt == (VT_ARRAY | VT_UI1) && var.parray) {
        // Byte vector (G-B: MSStorageDriver_FailurePredictData.VendorSpecific).
        const SafeArrayAccessDataFn acc = SafeArrAccess();
        const SafeArrayUnaccessDataFn unacc = SafeArrUnaccess();
        const SafeArrayGetBoundFn lb = SafeArrLBound();
        const SafeArrayGetBoundFn ub = SafeArrUBound();
        LONG lo = 0, hi = -1;
        void* data = nullptr;
        if (acc && unacc && lb && ub && lb(var.parray, 1, &lo) == S_OK &&
            ub(var.parray, 1, &hi) == S_OK && hi >= lo && acc(var.parray, &data) == S_OK) {
            const LONG n = hi - lo + 1;
            const auto* bytes = static_cast<const uint8_t*>(data);
            v.bytes.assign(bytes, bytes + (n > 4096 ? 4096 : n));  // bound the copy
            v.present = true;
            unacc(var.parray);
        }
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
    // V15/P1: the VARIANT returned by IWbemClassObject::Get owns its payload.
    // Copy first (above), then hand ownership to VariantClear exactly once —
    // this replaces the old manual SysFreeString (double-free hazard) and adds
    // the missing SAFEARRAY destroy. Degenerate fallback only if oleaut32 went
    // missing between binding points; the leak regresses to BSTR-free-only.
    const VariantClearFn clear = VariantClr();
    if (clear) {
        clear(&var);
    } else if (var.vt == VT_BSTR && var.bstrVal) {
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
        const HRESULT step = en->Next(5000, 1, obj.pp(), &got);
        if (step != S_OK || got == 0) {
            // G-B honesty fix: the access denial of an elevation-gated class can
            // surface at ENUMERATION time, not at ExecQuery (measured on
            // MSAcpi_ThermalZoneTemperature non-admin: ExecQuery S_OK, first
            // Next() -> 0x80041003 WBEM_E_ACCESS_DENIED). Without this the
            // snapshot claimed NoHardware ("本机无此传感器") where the truth is
            // NeedAdmin. (The old code hit exactly that on this box.)
            if (step == kWbemAccessDenied || step == kEAccessDenied) r.denied = true;
            break;
        }
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
    // MSAcpi_ThermalZoneTemperature: an INSTANCE SET (one per ACPI thermal zone,
    // not a single package value) — implemented by the ACPI driver, unit 0.1 K.
    // G-B: enumerate every instance (WMI collection traversal), one reading per
    // zone, labeled "ACPI 热区 N" (+ InstanceName when the row carries one).
    // Non-admin is usually rejected (R6 §3: this box reports 拒绝访问).
    const WmiResult w = WmiQuery(L"ROOT\\WMI",
                                 L"SELECT InstanceName, CurrentTemperature FROM "
                                 L"MSAcpi_ThermalZoneTemperature",
                                 {L"InstanceName", L"CurrentTemperature"});
    if (w.denied && w.rows.empty()) {
        // The normal non-admin case: enumeration denied before any instance.
        SensorReading r;
        r.label = L"ACPI 热区温度";
        r.unit = L"°C";
        r.state = SensorReading::State::NeedAdmin;
        cpu->push_back(r);
        *notes += L"；ACPI 热区温度需管理员权限";
        return;
    }
    // V15/P2 honesty: an unclassified failure at locator/connect/proxy/query
    // stage is WMI INFRASTRUCTURE being unavailable — the thermal-zone source
    // itself is unproven. That must not render as NoHardware ("本机无此传感器").
    // Within the frozen four-state contract this surfaces as NeedAdmin + an
    // explicit note that elevation will NOT fix it (label carries the truth too).
    if (!w.ok && !w.notSupported) {
        STM_LOG_WARN("sensors", L"WMI 基础设施不可用（连接/查询失败），热区状态未知");
        SensorReading r;
        r.label = L"ACPI 热区温度（WMI 不可用）";
        r.unit = L"°C";
        r.state = SensorReading::State::NeedAdmin;
        cpu->push_back(r);
        *notes += L"；WMI 服务不可用（连接/查询失败），热区温度状态未知（非权限问题，提权无济于事）";
        return;
    }
    if (w.notSupported || w.rows.empty()) {
        SensorReading r;
        r.label = L"ACPI 热区温度";
        r.unit = L"°C";
        r.state = SensorReading::State::NoHardware;
        cpu->push_back(r);
        *notes += L"；本机无 ACPI 热区传感器";
        return;
    }
    int added = 0, invalid = 0, rowIdx = -1;
    for (const WmiRow& row : w.rows) {
        ++rowIdx;
        const auto it = row.find(L"CurrentTemperature");
        if (it == row.end() || !it->second.present || !it->second.isNum) {
            ++invalid;
            continue;  // honest: no reading instead of a fake 0
        }
        const double c = static_cast<double>(it->second.num) / 10.0 - 273.15;
        if (c < -60.0 || c > 250.0) {
            ++invalid;
            continue;  // implausible -> no fake reading
        }
        SensorReading r;
        std::wstring zone = Fmt(L"ACPI 热区 {}", rowIdx);
        const auto in = row.find(L"InstanceName");  // present but maybe empty
        if (in != row.end() && in->second.present && !in->second.str.empty()) {
            zone += Fmt(L"（{}）", in->second.str);
        }
        r.label = std::move(zone);
        r.value = c;
        r.unit = L"°C";
        r.state = SensorReading::State::Ok;
        cpu->push_back(std::move(r));
        ++added;
    }
    if (added == 0) {
        SensorReading r;
        r.label = L"ACPI 热区温度";
        r.unit = L"°C";
        r.state = SensorReading::State::NoHardware;
        cpu->push_back(r);
        *notes += L"；ACPI 热区返回无效温度值（不显示假数据）";
    } else if (invalid > 0) {
        *notes += Fmt(L"；{} 个 ACPI 热区返回无效值（已省略，不显示假数据）", invalid);
    } else if (w.denied) {
        // Rare: partial enumeration cut short by a denial after some instances.
        *notes += L"；ACPI 热区枚举被拒绝（结果可能不完整，需管理员权限）";
    }
}
// ===========================================================================
// GPU: NVML only (driver-supplied nvml.dll in System32). Absent -> empty gpu
// vector + honesty note; we never fake values for unsupported vendors.
// G-B: per card the NVML path expands into one reading PER SENSOR (temperature,
// slowdown temperature threshold, power, GPU util, VRAM util, fan) instead of a
// single aggregate — the user asked for "同类传感器多个值都展示出来". The
// legacy `gpu` vector keeps its exact F3 content (temp/gpu-util/power) for
// contract stability; the full expansion lands in the `gpus` group.
// ===========================================================================
namespace nvml {
using Device = void*;
constexpr int kRetSuccess = 0;  // nvmlReturn_t NVML_SUCCESS
constexpr int kTempGpu = 0;     // nvmlTemperatureSensors_t NVML_TEMPERATURE_GPU
constexpr int kThreshSlowdown = 1;  // nvmlTemperatureThresholds_t ..._SLOWDOWN
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
using GetThreshFn = int (*)(Device, int, unsigned*);  // temperature threshold, °C
using GetFanFn = int (*)(Device, unsigned*);  // percent of max
struct Fns {
    InitFn init = nullptr;
    ShutdownFn shutdown = nullptr;
    GetCountFn count = nullptr;
    GetHandleFn handle = nullptr;
    GetTempFn temp = nullptr;
    GetUtilFn util = nullptr;
    GetPowerFn power = nullptr;
    GetThreshFn thresh = nullptr;  // optional export
    GetFanFn fan = nullptr;        // optional export
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
    f->thresh = reinterpret_cast<GetThreshFn>(proc("nvmlDeviceGetTemperatureThreshold_v2"));
    if (!f->thresh) {
        f->thresh = reinterpret_cast<GetThreshFn>(proc("nvmlDeviceGetTemperatureThreshold"));
    }
    f->fan = reinterpret_cast<GetFanFn>(proc("nvmlDeviceGetFanSpeed"));
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
SensorReading Make(const wchar_t* label, double value, const wchar_t* unit) {
    SensorReading r;
    r.label = label;
    r.value = value;
    r.unit = unit;
    r.state = SensorReading::State::Ok;
    r.source = L"NVML";
    return r;
}
void Read(std::vector<SensorReading>* legacy, std::vector<SensorReading>* expanded,
          std::wstring* notes) {
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
        // --- temperature (legacy vector keeps its F3 entry too) ---
        unsigned tempC = 0;
        if (f.temp(dev, kTempGpu, &tempC) == kRetSuccess && tempC > 0) {
            const SensorReading r = Make(Fmt(L"GPU {} 温度", i).c_str(),
                                         static_cast<double>(tempC), L"°C");
            legacy->push_back(r);      // existing contract vector
            expanded->push_back(r);    // G-B per-sensor group
        }
        // --- slowdown temperature threshold (per card, when the driver knows it) ---
        if (f.thresh) {
            unsigned slowC = 0;
            if (f.thresh(dev, kThreshSlowdown, &slowC) == kRetSuccess && slowC > 0) {
                expanded->push_back(
                    Make(Fmt(L"GPU {} 慢速温度阈值", i).c_str(), static_cast<double>(slowC), L"°C"));
            }
        }
        // --- power draw (W) ---
        if (f.power) {
            unsigned mw = 0;
            if (f.power(dev, &mw) == kRetSuccess && mw > 0) {
                const SensorReading r = Make(Fmt(L"GPU {} 功耗", i).c_str(),
                                             static_cast<double>(mw) / 1000.0, L"W");
                legacy->push_back(r);
                expanded->push_back(r);
            }
        }
        // --- utilization: gpu AND memory components as separate readings ---
        if (f.util) {
            Utilization u{};
            if (f.util(dev, &u) == kRetSuccess) {
                const SensorReading ru = Make(Fmt(L"GPU {} 利用率", i).c_str(),
                                              static_cast<double>(std::min(u.gpu, 100u)), L"%");
                legacy->push_back(ru);
                expanded->push_back(ru);
                expanded->push_back(Make(Fmt(L"GPU {} 显存利用率", i).c_str(),
                                         static_cast<double>(std::min(u.memory, 100u)), L"%"));
            }
        }
        // --- fan (NVML reports % of max; 0% is a REAL reading — zero-RPM idle
        // mode — so unlike temps it stays Ok at 0) ---
        if (f.fan) {
            unsigned pct = 0;
            if (f.fan(dev, &pct) == kRetSuccess) {
                expanded->push_back(Make(Fmt(L"GPU {} 风扇", i).c_str(),
                                         static_cast<double>(std::min(pct, 100u)), L"%"));
            }
        }
    }
    if (expanded->empty()) *notes += L"；NVML 在线但未报告 GPU 传感器";
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
// F3: also reports available spare % / spare threshold % alongside pctUsed.
bool NvmeHealth(HANDLE h, double* tempC, uint64_t* poh, uint32_t* pctUsed, uint8_t* critWarn,
                uint32_t* sparePct, uint32_t* spareThreshPct) {
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
    // F3: spare fields; 0xFF is unpopulated padding, reported as unavailable.
    *sparePct = log[3] == 0xFF ? UINT32_MAX : log[3];
    *spareThreshPct = log[4] == 0xFF ? UINT32_MAX : log[4];
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
            uint32_t spare = UINT32_MAX, spareTh = UINT32_MAX;  // F3 NVMe detail
            if (proto == BusProto::Nvme) {
                ok = NvmeHealth(h.get(), &t, &poh, &pct, &crit, &spare, &spareTh);
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
                             ? NvmeHealth(h1g.get(), &t, &poh, &pct, &crit, &spare, &spareTh)
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
                    // F3 detail (honest: only from a real log page read; 0xFF padding
                    // byte is already mapped to UINT32_MAX inside NvmeHealth).
                    d.critWarnValid = true;
                    d.critWarnBits = crit;
                    d.wearPct = pct == 0xFF ? UINT32_MAX : pct;
                    d.sparePct = spare;
                    d.spareThreshPct = spareTh;
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
// ===========================================================================
// F3: shared 350 ms delta window. One PDH query carries the rate counters
// (per-core CPU % via Processor Information; per-engtype GPU Engine; GPU
// Adapter Memory dedicated/shared) and two GetIfTable2 samples give
// per-adapter octet rates. All Ok readings come from documented user-mode
// sources (R6 §4/§6); a missing source yields ONE NoHardware placeholder
// entry — never fabricated zeros.
// ===========================================================================
constexpr DWORD kDeltaWindowMs = 350;
// PDH engine type token -> short display name; nullptr = keep the raw token.
const wchar_t* EngineTypeLabel(const std::wstring& raw) {
    if (raw == L"3D") return L"3D";
    if (raw == L"Copy") return L"Copy";
    if (raw == L"VideoDecode") return L"视频解码";
    if (raw == L"VideoEncode") return L"视频编码";
    if (raw == L"VideoProcessing") return L"视频处理";
    return nullptr;
}
double ClampPct(double v) {
    return v < 0.0 ? 0.0 : (v > 100.0 ? 100.0 : v);
}
void ReadDeltaWindow(std::vector<SensorReading>* coreUtil, std::vector<SensorReading>* gpuOut,
                     std::vector<SensorReading>* netOut, std::wstring* notes) {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER procUtil = nullptr, gpuEng = nullptr, gpuDed = nullptr, gpuShr = nullptr;
    bool haveProc = false, haveEng = false, haveDed = false, haveShr = false;
    MIB_IF_TABLE2* ifT0 = nullptr;
    MIB_IF_TABLE2* ifT1 = nullptr;
    struct Cleanup {
        PDH_HQUERY q = nullptr;
        MIB_IF_TABLE2* t0 = nullptr;
        MIB_IF_TABLE2* t1 = nullptr;
        ~Cleanup() {
            if (t0) ::FreeMibTable(t0);
            if (t1) ::FreeMibTable(t1);
            if (q) ::PdhCloseQuery(q);
        }
    } cleanup;
    if (::PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS && q != nullptr) {
        cleanup.q = q;
        std::wstring tpl;
        if (stm::cd::PdhLocalizeEnglishPath(L"\\Processor Information(*)\\% Processor Time", &tpl)) {
            haveProc = stm::cd::PdhAddWildcardCounter(q, tpl, &procUtil);
        }
        if (stm::cd::PdhLocalizeEnglishPath(L"\\GPU Engine(*)\\Utilization Percentage", &tpl)) {
            haveEng = stm::cd::PdhAddWildcardCounter(q, tpl, &gpuEng);
        }
        if (stm::cd::PdhLocalizeEnglishPath(L"\\GPU Adapter Memory(*)\\Dedicated Usage", &tpl)) {
            haveDed = stm::cd::PdhAddWildcardCounter(q, tpl, &gpuDed);
        }
        if (stm::cd::PdhLocalizeEnglishPath(L"\\GPU Adapter Memory(*)\\Shared Usage", &tpl)) {
            haveShr = stm::cd::PdhAddWildcardCounter(q, tpl, &gpuShr);
        }
        if (haveProc || haveEng) stm::cd::PdhCollect(q);  // rate counters need a warm-up sample
    }
    const bool haveNet0 = ::GetIfTable2(&ifT0) == NO_ERROR && ifT0 != nullptr;
    cleanup.t0 = ifT0;
    const ULONGLONG t0ms = ::GetTickCount64();
    if (haveProc || haveEng || haveNet0) ::Sleep(kDeltaWindowMs);
    if (haveProc || haveEng) stm::cd::PdhCollect(q);
    const ULONGLONG t1ms = ::GetTickCount64();
    const bool haveNet1 = haveNet0 && ::GetIfTable2(&ifT1) == NO_ERROR && ifT1 != nullptr;
    cleanup.t1 = ifT1;
    const double dtSec = static_cast<double>(t1ms - t0ms) / 1000.0;
    // --- per-core CPU utilization (Processor Information, documented; R6 §6) ---
    if (haveProc) {
        std::vector<stm::cd::PdhArrayItem> items;
        int added = 0;
        if (stm::cd::PdhFmtArrayDouble(procUtil, &items)) {
            for (const stm::cd::PdhArrayItem& it : items) {
                if (!it.valid) continue;
                if (it.name.empty() || it.name.find(L"_Total") != std::wstring::npos) continue;
                // Instances are "0,3" (processor group, logical core): the number
                // after the last comma labels the core.
                std::wstring idx = it.name;
                const size_t comma = idx.rfind(L',');
                if (comma != std::wstring::npos) idx = idx.substr(comma + 1);
                SensorReading r;
                r.label = Fmt(L"CPU 核 {} 占用率", idx);
                r.value = ClampPct(it.value);
                r.unit = L"%";
                r.state = SensorReading::State::Ok;
                coreUtil->push_back(std::move(r));
                ++added;
            }
        }
        if (added == 0) {
            SensorReading r;
            r.label = L"CPU 每核占用率";
            r.unit = L"%";
            r.state = SensorReading::State::NoHardware;
            coreUtil->push_back(r);
            *notes += L"；Processor Information 无有效样本，每核占用率不可用";
        }
    } else {
        SensorReading r;
        r.label = L"CPU 每核占用率";
        r.unit = L"%";
        r.state = SensorReading::State::NoHardware;
        coreUtil->push_back(r);
        *notes += L"；Processor Information 计数器不可用，每核占用率不可用";
    }
    // --- GPU engine utilization, aggregated per engtype_* (R6 §4) ---
    bool anyEngine = false;
    if (haveEng) {
        std::vector<stm::cd::PdhArrayItem> items;
        if (stm::cd::PdhFmtArrayDouble(gpuEng, &items)) {
            std::map<std::wstring, std::pair<double, int>> agg;  // engtype -> (sum, instances)
            for (const stm::cd::PdhArrayItem& it : items) {
                if (!it.valid) continue;
                const size_t e = it.name.find(L"engtype_");
                if (e == std::wstring::npos) continue;
                const std::wstring type = it.name.substr(e + 8);  // len(L"engtype_") == 8
                if (type.empty()) continue;
                auto& slot = agg[type];
                slot.first += ClampPct(it.value);
                slot.second += 1;
            }
            for (const auto& kv : agg) {
                SensorReading r;
                const wchar_t* cn = EngineTypeLabel(kv.first);
                r.label = cn ? Fmt(L"GPU {} 引擎占用率", cn) : Fmt(L"GPU 引擎占用率（{}）", kv.first);
                r.value = ClampPct(kv.second.first);  // aggregate, clamped like per-engine
                r.unit = L"%";
                r.state = SensorReading::State::Ok;
                gpuOut->push_back(std::move(r));
                anyEngine = true;
            }
        }
        if (!anyEngine) {
            SensorReading r;
            r.label = L"GPU 引擎占用率";
            r.unit = L"%";
            r.state = SensorReading::State::NoHardware;
            gpuOut->push_back(r);
            *notes += L"；GPU Engine 计数器在但无实例（独显休眠或无 GPU 活动）";
        }
    } else {
        SensorReading r;
        r.label = L"GPU 引擎占用率";
        r.unit = L"%";
        r.state = SensorReading::State::NoHardware;
        gpuOut->push_back(r);
        *notes += L"；GPU Engine 计数器不可用（需 Win10 1709+ 图形栈）";
    }
    // --- GPU adapter memory, summed over adapters (raw usage counters) ---
    auto adapterMemoryBytes = [](PDH_HCOUNTER h) {
        if (!h) return -1.0;
        std::vector<stm::cd::PdhArrayItem> items;
        if (!stm::cd::PdhFmtArrayDouble(h, &items)) return -1.0;
        double sum = 0.0;
        bool any = false;
        for (const stm::cd::PdhArrayItem& it : items) {
            if (!it.valid || it.value < 0.0) continue;
            sum += it.value;
            any = true;
        }
        return any ? sum : -1.0;
    };
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    const double ded = adapterMemoryBytes(gpuDed);
    if (ded >= 0.0) {
        SensorReading r;
        r.label = L"GPU 显存（专用）";
        r.value = ded / kGiB;
        r.unit = L"GiB";
        r.state = SensorReading::State::Ok;
        gpuOut->push_back(std::move(r));
    }
    const double shr = adapterMemoryBytes(gpuShr);
    if (shr >= 0.0) {
        SensorReading r;
        r.label = L"GPU 显存（共享）";
        r.value = shr / kGiB;
        r.unit = L"GiB";
        r.state = SensorReading::State::Ok;
        gpuOut->push_back(std::move(r));
    }
    if (ded < 0.0 && shr < 0.0) {
        SensorReading r;
        r.label = L"GPU 显存";
        r.unit = L"GiB";
        r.state = SensorReading::State::NoHardware;
        gpuOut->push_back(r);
        if (!haveEng) *notes += L"；GPU Adapter Memory 计数器不可用";
    }
    // --- per-adapter network rates (GetIfTable2 octet deltas) ---
    if (haveNet1) {
        std::map<uint32_t, const MIB_IF_ROW2*> t0rows;
        for (ULONG i = 0; i < ifT0->NumEntries; ++i) {
            t0rows.emplace(ifT0->Table[i].InterfaceIndex, &ifT0->Table[i]);
        }
        int adapters = 0;
        for (ULONG i = 0; i < ifT1->NumEntries; ++i) {
            const MIB_IF_ROW2& r1 = ifT1->Table[i];
            if (r1.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (r1.OperStatus != IfOperStatusUp) continue;
            const auto it0 = t0rows.find(r1.InterfaceIndex);
            if (it0 == t0rows.end()) continue;  // appeared mid-window: no honest delta yet
            const MIB_IF_ROW2& r0 = *it0->second;
            std::wstring name(r1.Alias);
            if (name.empty()) name = r1.Description;
            if (name.empty()) name = Fmt(L"接口 {}", static_cast<unsigned>(r1.InterfaceIndex));
            auto rate = [&](uint64_t now, uint64_t prev) {
                if (dtSec <= 0.0 || now < prev) return -1.0;  // counter reset is not a spike
                return static_cast<double>(now - prev) / dtSec;
            };
            const double recv = rate(r1.InOctets, r0.InOctets);
            const double send = rate(r1.OutOctets, r0.OutOctets);
            SensorReading rr;
            rr.label = name + L" 接收";
            rr.unit = L"B/s";
            if (recv >= 0.0) {
                rr.value = recv;
                rr.state = SensorReading::State::Ok;
            } else {  // honest placeholder instead of a wrapped-around spike
                rr.label += L"（计数器重置）";
                rr.state = SensorReading::State::NoHardware;
            }
            netOut->push_back(std::move(rr));
            SensorReading rs;
            rs.label = name + L" 发送";
            rs.unit = L"B/s";
            if (send >= 0.0) {
                rs.value = send;
                rs.state = SensorReading::State::Ok;
            } else {
                rs.label += L"（计数器重置）";
                rs.state = SensorReading::State::NoHardware;
            }
            netOut->push_back(std::move(rs));
            if (r1.TransmitLinkSpeed > 0 && r1.TransmitLinkSpeed != UINT64_MAX) {
                SensorReading rl;
                rl.label = name + L" 链路速度";
                rl.value = static_cast<double>(r1.TransmitLinkSpeed) / 1.0e6;
                rl.unit = L"Mbps";
                rl.state = SensorReading::State::Ok;
                netOut->push_back(std::move(rl));
            }
            ++adapters;
        }
        if (adapters == 0) {
            SensorReading r;
            r.label = L"网络适配器吞吐";
            r.unit = L"B/s";
            r.state = SensorReading::State::NoHardware;
            netOut->push_back(r);
            *notes += L"；无非回环且在线的网卡";
        }
    } else {
        *notes += haveNet0 ? L"；GetIfTable2 二次采样失败，本拍网卡速率不可用"
                           : L"；GetIfTable2 不可用，网卡速率不可见";
        SensorReading r;
        r.label = L"网络适配器吞吐";
        r.unit = L"B/s";
        r.state = SensorReading::State::NoHardware;
        netOut->push_back(r);
    }
}
// ===========================================================================
// F3: battery (CallNtPowerInformation SystemBatteryState=5, winnt.h layout).
// No battery -> one NoHardware entry (honest; desktops are the normal case).
// ===========================================================================
#pragma pack(push, 4)
struct BatteryStateRow {  // SYSTEM_BATTERY_STATE
    BYTE AcOnLine;
    BYTE BatteryPresent;
    BYTE Charging;
    BYTE Discharging;
    BYTE Spare4[3];
    DWORD MaxCapacity;
    DWORD RemainingCapacity;
    DWORD Rate;           // mW, current charge/discharge rate
    DWORD EstimatedTime;  // seconds; 0xFFFFFFFF = unknown
    DWORD DefaultAlert1;
    DWORD DefaultAlert2;
};
#pragma pack(pop)
static_assert(sizeof(BatteryStateRow) == 32, "SYSTEM_BATTERY_STATE layout");
void ReadBattery(std::vector<SensorReading>* out, std::wstring* notes) {
    const CallNtPowerInformationFn fn = CallNtPower();
    BatteryStateRow bs{};
    if (!fn || fn(5 /*SystemBatteryState*/, nullptr, 0, &bs, sizeof(bs)) != 0) {
        SensorReading r;
        r.label = L"电池状态";
        r.unit = L"";
        r.state = SensorReading::State::NoHardware;
        out->push_back(r);
        *notes += L"；电池状态读取失败（CallNtPowerInformation SystemBatteryState）";
        return;
    }
    if (bs.BatteryPresent == 0) {
        SensorReading r;
        r.label = L"电池";
        r.unit = L"";
        r.state = SensorReading::State::NoHardware;
        out->push_back(r);
        *notes += L"；本机无电池（台式机/外接供电属正常）";
        return;
    }
    SensorReading ac;
    ac.label = bs.AcOnLine ? L"电池（交流供电）" : L"电池（电池供电）";
    ac.value = bs.AcOnLine ? 1.0 : 0.0;
    ac.unit = L"";
    ac.state = SensorReading::State::Ok;
    out->push_back(ac);
    if (bs.MaxCapacity > 0) {
        SensorReading r;
        r.label = L"电池 剩余电量";
        r.value = ClampPct(100.0 * static_cast<double>(bs.RemainingCapacity) /
                           static_cast<double>(bs.MaxCapacity));
        r.unit = L"%";
        r.state = SensorReading::State::Ok;
        out->push_back(std::move(r));
    }
    if (bs.EstimatedTime != 0 && bs.EstimatedTime != 0xFFFFFFFF) {  // 0/-1 = unknown
        SensorReading r;
        r.label = L"电池 剩余时间";
        r.value = static_cast<double>(bs.EstimatedTime);
        r.unit = L"s";
        r.state = SensorReading::State::Ok;
        out->push_back(std::move(r));
    }
    if ((bs.Charging || bs.Discharging) && bs.Rate > 0) {
        SensorReading r;
        r.label = bs.Charging ? L"电池 充电功率" : L"电池 放电功率";
        r.value = static_cast<double>(bs.Rate) / 1000.0;
        r.unit = L"W";
        r.state = SensorReading::State::Ok;
        out->push_back(std::move(r));
    }
}
// ===========================================================================
// F3: memory (GlobalMemoryStatusEx + GetPerformanceInfo). GetPerformanceInfo
// is bound dynamically (K32GetPerformanceInfo in kernel32, psapi.dll
// fallback) so the stm_collect link line stays unchanged.
// ===========================================================================
using GetPerfInfoFn = BOOL(WINAPI*)(PPERFORMANCE_INFORMATION, DWORD);
GetPerfInfoFn GetPerfInfo() {
    static GetPerfInfoFn fn = []() -> GetPerfInfoFn {
        const HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
        if (k32) {
            const auto p = reinterpret_cast<GetPerfInfoFn>(::GetProcAddress(k32, "K32GetPerformanceInfo"));
            if (p) return p;
        }
        // Deliberately never freed: process-lifetime binding, same as powrprof above.
        const HMODULE psapi = ::LoadLibraryW(L"psapi.dll");
        return psapi ? reinterpret_cast<GetPerfInfoFn>(::GetProcAddress(psapi, "GetPerformanceInfo"))
                     : nullptr;
    }();
    return fn;
}
void ReadMemory(std::vector<SensorReading>* out, std::wstring* notes) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    const bool haveMs = ::GlobalMemoryStatusEx(&ms) != FALSE;
    const GetPerfInfoFn pf = GetPerfInfo();
    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    const bool havePi = pf != nullptr && pf(&pi, sizeof(pi)) != FALSE;
    if (!haveMs && !havePi) {
        SensorReading r;
        r.label = L"内存";
        r.unit = L"%";
        r.state = SensorReading::State::NoHardware;
        out->push_back(r);
        *notes += L"；内存计数器读取失败（GlobalMemoryStatusEx/GetPerformanceInfo）";
        return;
    }
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    if (haveMs && ms.ullTotalPhys > 0) {
        const double used = static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys);
        const double total = static_cast<double>(ms.ullTotalPhys);
        SensorReading pct;
        pct.label = L"物理内存占用";
        pct.value = ClampPct(100.0 * used / total);
        pct.unit = L"%";
        pct.state = SensorReading::State::Ok;
        out->push_back(std::move(pct));
        SensorReading usedR;
        usedR.label = L"物理内存已用";
        usedR.value = used / kGiB;
        usedR.unit = L"GiB";
        usedR.state = SensorReading::State::Ok;
        out->push_back(std::move(usedR));
        SensorReading totalR;
        totalR.label = L"物理内存总量";
        totalR.value = total / kGiB;
        totalR.unit = L"GiB";
        totalR.state = SensorReading::State::Ok;
        out->push_back(std::move(totalR));
    }
    if (havePi) {
        if (pi.CommitLimit > 0) {
            SensorReading r;
            r.label = L"提交内存占用";
            r.value = ClampPct(100.0 * static_cast<double>(pi.CommitTotal) /
                               static_cast<double>(pi.CommitLimit));
            r.unit = L"%";
            r.state = SensorReading::State::Ok;
            out->push_back(std::move(r));
        }
        const double pageSize = static_cast<double>(pi.PageSize);
        SensorReading paged;
        paged.label = L"分页池";
        paged.value = static_cast<double>(pi.KernelPaged) * pageSize / kMiB;
        paged.unit = L"MiB";
        paged.state = SensorReading::State::Ok;
        out->push_back(std::move(paged));
        SensorReading nonPaged;
        nonPaged.label = L"非分页池";
        nonPaged.value = static_cast<double>(pi.KernelNonpaged) * pageSize / kMiB;
        nonPaged.unit = L"MiB";
        nonPaged.state = SensorReading::State::Ok;
        out->push_back(std::move(nonPaged));
    }
}
// ===========================================================================
// G-B: "extra" — best-effort readings from documented WMI temperature classes
// OUTSIDE MSAcpi_ThermalZoneTemperature. The snapshot group is displayed only
// when non-empty, so every failure path stays silent: absence promises nothing.
// P2 (2026-09-18): each source is enumerated instance by instance and every
// label carries its provenance prefix, so same-name readings from different
// providers can never collide (contract: "同类传感器多值逐项展示").
// ===========================================================================
void ReadExtraSensors(std::vector<SensorReading>* out) {
    // 1) Win32_Temperature (ROOT\CIMV2, DMTF temperature sensor): CurrentReading
    //    per instance. Almost never implemented by a real provider — an absent
    //    class is the normal case and simply yields nothing. Unit caveat: the
    //    value arrives in tenths, but providers differ between tenths of Kelvin
    //    (ACPI-style) and tenths of °C (DMTF sensor model); the two readings
    //    can never BOTH fall inside the plausible window, so pick the one that
    //    fits and drop the row entirely when neither does (never a fake temp).
    {
        const WmiResult w = WmiQuery(L"ROOT\\CIMV2",
                                     L"SELECT InstanceName, CurrentReading FROM Win32_Temperature",
                                     {L"InstanceName", L"CurrentReading"});
        int added = 0;
        for (const WmiRow& row : w.rows) {
            const auto it = row.find(L"CurrentReading");
            if (it == row.end() || !it->second.present || !it->second.isNum) continue;
            const double raw = static_cast<double>(it->second.num);
            if (raw == 0.0) continue;  // empty-register sentinel; a real 0.0 °C reading is implausible (V15-P2-3)
            double c = raw / 10.0 - 273.15;  // tenths of Kelvin first
            if (c < -60.0 || c > 250.0) c = raw / 10.0;  // else tenths of °C
            if (c < -60.0 || c > 250.0) continue;        // implausible -> skip, no fake
            SensorReading r;
            r.label = Fmt(L"WMI 温度 {}", added);
            const auto in = row.find(L"InstanceName");
            if (in != row.end() && in->second.present && !in->second.str.empty()) {
                r.label = Fmt(L"WMI 温度（{}）", in->second.str);
            }
            r.value = c;
            r.unit = L"°C";
            r.state = SensorReading::State::Ok;
            out->push_back(std::move(r));
            ++added;
        }
    }
    // 2) MSStorageDriver_FailurePredictData (ROOT\WMI): per-drive SMART attribute
    //    block (same 12-byte layout as AtaSmart); attribute 194/190 raw[0] is the
    //    temperature. Usually admin-gated (this box: 拒绝访问) — a denial leaves
    //    the group untouched; the per-disk IOCTL path above remains the source.
    {
        const WmiResult w = WmiQuery(L"ROOT\\WMI",
                                     L"SELECT InstanceName, VendorSpecific FROM "
                                     L"MSStorageDriver_FailurePredictData",
                                     {L"InstanceName", L"VendorSpecific"});
        int added = 0;
        for (const WmiRow& row : w.rows) {
            const auto vs = row.find(L"VendorSpecific");
            if (vs == row.end() || !vs->second.present) continue;
            const std::vector<uint8_t>& s = vs->second.bytes;
            double t = 0;
            bool have = false;
            // version(2) then 12-byte attributes: id(1) flags(2) value(1)
            // worst(1) raw(6, LE) reserved(1) — raw[0] at p[5], see AtaSmart.
            for (size_t off = 2; off + 6 <= s.size(); off += 12) {
                const uint8_t id = s[off];
                if (id == 0) break;
                if ((id == 194 || id == 190) && !have) {
                    const double cand = static_cast<double>(s[off + 5]);
                    if (cand >= 10.0 && cand <= 120.0) {  // plausible Celsius only
                        t = cand;
                        have = true;
                    }
                }
            }
            if (!have) continue;  // honest: unreadable/absent -> no entry at all
            SensorReading r;
            r.label = Fmt(L"磁盘 {} 温度（WMI SMART）", added);
            const auto in = row.find(L"InstanceName");
            if (in != row.end() && in->second.present && !in->second.str.empty()) {
                r.label += Fmt(L"（{}）", in->second.str);
            }
            r.value = t;
            r.unit = L"°C";
            r.state = SensorReading::State::Ok;
            out->push_back(std::move(r));
            ++added;
        }
    }
    // 3) Win32_PerfFormattedData_Counters_ThermalZoneInformation (ROOT\CIMV2,
    //    PerfProc): one instance per ACPI thermal zone. Documented unit is
    //    tenths of Kelvin, but firmware providers differ — this box (measured,
    //    \_TZ.TZ00) reports raw=301, i.e. 30.1 °C under a tenths-of-°C reading
    //    and a bogus -243 °C under the documented one. Same unit ladder as
    //    source 1: tenths of K first (the documented unit wins whenever it is
    //    plausible), tenths of °C as the only alternative, anything else is
    //    dropped. Reads without elevation; absent/broken on many desktops —
    //    any failure or implausible value stays silent.
    {
        const WmiResult w = WmiQuery(
            L"ROOT\\CIMV2",
            L"SELECT Name, Temperature FROM "
            L"Win32_PerfFormattedData_Counters_ThermalZoneInformation",
            {L"Name", L"Temperature"});
        int added = 0;
        for (const WmiRow& row : w.rows) {
            const auto it = row.find(L"Temperature");
            if (it == row.end() || !it->second.present || !it->second.isNum) continue;
            const double raw = static_cast<double>(it->second.num);
            if (raw == 0.0) continue;  // empty-register sentinel; never report a fake 0.0 °C (V15-P2-3)
            double c = raw / 10.0 - 273.15;  // documented tenths of Kelvin first
            if (c < -60.0 || c > 250.0) c = raw / 10.0;  // measured tenths-of-°C units
            if (c < -60.0 || c > 250.0) continue;        // implausible -> skip, no fake
            SensorReading r;
            r.label = Fmt(L"WMI 热区计数器 {}", added);
            const auto in = row.find(L"Name");
            if (in != row.end() && in->second.present && !in->second.str.empty()) {
                r.label = Fmt(L"WMI 热区计数器（{}）", in->second.str);
            }
            r.value = c;
            r.unit = L"°C";
            r.state = SensorReading::State::Ok;
            out->push_back(std::move(r));
            ++added;
        }
    }
    // 4) Intel DPTF (Dynamic Platform and Thermal Framework) participants: the
    //    driver-owned TEMPERATURE instance set under root\Intel_DPTF (older
    //    drivers: root\Intel(DPTF)), CurrentTemperature in tenths of Kelvin
    //    per the DPTF spec. Undocumented by Intel publicly and absent without
    //    the driver — a missing namespace classifies as notSupported and is
    //    skipped SILENTLY (the normal case); only plausible Ok rows surface.
    for (const wchar_t* ns : {L"ROOT\\Intel_DPTF", L"ROOT\\Intel(DPTF)"}) {
        const WmiResult w = WmiQuery(ns,
                                     L"SELECT InstanceName, CurrentTemperature FROM TEMPERATURE",
                                     {L"InstanceName", L"CurrentTemperature"});
        if (w.notSupported) continue;  // namespace/class absent: quiet skip
        int added = 0;
        for (const WmiRow& row : w.rows) {
            const auto it = row.find(L"CurrentTemperature");
            if (it == row.end() || !it->second.present || !it->second.isNum) continue;
            const double c = static_cast<double>(it->second.num) / 10.0 - 273.15;
            if (c < -60.0 || c > 250.0) continue;  // implausible -> skip, no fake
            SensorReading r;
            r.label = L"DPTF 温度";
            const auto in = row.find(L"InstanceName");
            if (in != row.end() && in->second.present && !in->second.str.empty()) {
                r.label = Fmt(L"DPTF 温度（{}）", in->second.str);
            }
            r.value = c;
            r.unit = L"°C";
            r.state = SensorReading::State::Ok;
            r.source = L"DPTF";
            out->push_back(std::move(r));
            ++added;
        }
        if (added > 0) break;  // this namespace delivered; don't double-report
    }
}
}  // namespace
SensorSnapshot ReadSensors(std::wstring* err) {
    SensorSnapshot snap;
    std::wstring notes;
    try {
        ReadCpuTemp(&snap.cpu, &notes);
        std::vector<SensorReading> coreFreq;
        ReadCpuFreq(&coreFreq, &notes);
        for (const SensorReading& r : coreFreq) snap.cpu.push_back(r);  // existing contract
        // F3: shared ~350 ms delta window (per-core %, GPU engine/VRAM, per-NIC rates).
        std::vector<SensorReading> coreUtil, gpuEngine, net;
        ReadDeltaWindow(&coreUtil, &gpuEngine, &net, &notes);
        snap.cpuCores = std::move(coreFreq);
        snap.cpuCores.insert(snap.cpuCores.end(), coreUtil.begin(), coreUtil.end());
        // G-B: gpus = PDH engine/VRAM group + the NVML per-card, per-sensor
        // expansion (legacy `gpu` vector keeps its exact F3 content).
        snap.gpus = std::move(gpuEngine);
        nvml::Read(&snap.gpu, &snap.gpus, &notes);
        snap.network = std::move(net);
        ReadDisks(&snap.disks, &notes);
        ReadBattery(&snap.battery, &notes);
        ReadMemory(&snap.memory, &notes);
        ReadExtraSensors(&snap.extra);  // G-B: best effort; empty -> not shown
        snap.uptimeSec = static_cast<double>(::GetTickCount64()) / 1000.0;
        SensorReading fan;
        fan.label = L"风扇转速";
        fan.value = 0.0;
        fan.unit = L"rpm";
        fan.state = SensorReading::State::NeedDriver;
        snap.fans.push_back(fan);
        notes += L"；风扇转速需要内核驱动（本应用不随包分发驱动，见调研 R6）；CPU 占用率另见性能页；"
                 L"每核占用率/GPU 引擎与显存来自 PDH、网卡速率来自 GetIfTable2（350ms 增量窗口）；"
                 L"热区/热区计数器/DPTF 温度均为区域级读数而非每核 DTS；"
                 L"每核温度等更多传感器可外接 LibreHardwareMonitor 数据源（默认关闭）";
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


// Sensors.cpp — 第 3 阶段 + F3 扩展（契约 Sensors.h）。每条读数都遵循
// R6 的诚实三分类：Ok = 来自有文档的用户态来源的真实值；
// NeedAdmin = 来源存在但需提权；
// NeedDriver = 用户态不可达（风扇转速——本应用不附带内核驱动）；
// NoHardware = 适配器/磁盘存在但该传感器缺失。
// 绝不用 0 顶替数据。
//
//   CPU 频率        CallNtPowerInformation(ProcessorInformation=11)  有文档，无需管理员
//   CPU 每核 %      PDH \Processor Information(*)\% Processor Time（有文档，R6 §6）
//   ACPI 热区       WMI root\WMI\MSAcpi_ThermalZoneTemperature —— 每实例一条读数
//                  INSTANCE (G-B multi-value: every zone of the instance set,
//                  label "ACPI 热区 N" + InstanceName); admin on this box
//   GPU 温度/利用率  存在时用 System32 的 nvml.dll（驱动自带）；每卡
//                  经 NVML 展开为每传感器读数（G-B）：温度、
//                  减速温度阈值、功耗 W、GPU 利用率、显存利用率
//                 （nvmlDeviceGetUtilizationRates 两个分量）、风扇；多 GPU
//                  逐卡枚举。无 nvml.dll -> gpus 组
//                  保留 PDH 内容并附诚实备注（跳过 IGCL：
//                  接口复杂，见 R6 §4）。任何地方都不伪造读数。
//   GPU 引擎 %      PDH \GPU Engine(*)\Utilization Percentage 按 engtype_*（R6 §4）；
//                  专用/共享 VRAM 经 \GPU Adapter Memory(*)
//   网络            GetIfTable2 每适配器字节差值，共享 350 ms 窗口
//  （F3）+ 链路速度；排除回环，跳过非 Up 适配器
//   电池            CallNtPowerInformation(SystemBatteryState=5)——无电池时 NoHardware
//   内存            GlobalMemoryStatusEx + GetPerformanceInfo（K32 动态绑定）
//   磁盘            MSFT_PhysicalDisk（粗粒度健康）+ NVMe 健康日志页 0x02 /
//                  经 SMART_RCV_DRIVE_DATA 的 ATA SMART 取温度 + 通电时长
//                  + 备余/磨损/严重警告细节（F3）。每物理盘一条 DiskHealth，
//                  绝不聚合（G-B：确认按盘逐条展示）。
//   Extra（G-B）    尽力而为的 ACPI 热区之外的 WMI 温度类；
//                  空组 = 未找到 = 不展示。P2（2026-09-18，
//                  "CPU 温度信息增强") exhausts the documented USER-MODE thermal
//                  来源逐个列出，每实例一条读数，每条标签都标注
//                  with its provenance ("WMI 温度 N/（实例）" for Win32_Temperature,
//                  "WMI 热区计数器 N/（实例）" for the PerfProc thermal-zone counter,
//                  "DPTF 温度（参与者）" for the Intel DPTF TEMPERATURE set when its
//                  命名空间存在）。每核 DTS 温度（MSR 0x19C/0x1A2/
//                  0x1B1）按设计不可达：需要内核驱动，
//                  而本应用不带任何驱动（红线）——传感器页
//                  已注明并提供可选的 LibreHardwareMonitor 桥接。
//   每核 DTS 温度    （D6，2026-09-20）可选内核通道：本机装有 PawnIO
//                  （官方签名运行时，用户自行安装）且官方签名模块
//                  blob 已放置时，经 PawnIO IntelMSR 白名单只读 MSR
//                  0x19C/0x1A2 取每核 DTS（collect/PawnIoLink.h），
//                  逐条追加进 cpu 组（label "CPU 核心 N（DTS）"，
//                  source "PawnIO"）。未安装/模块缺失 -> 如实不出线，
//                  行为与从前完全一致（NeedDriver 说明见下）。
//   风扇            默认 NeedDriver；PawnIO LpcIO 通道成功识别已知
//                  SuperIO 芯片时填真实 RPM（label 带芯片名），失败保持
//                  NeedDriver。电压经 LpcIO 时进 extra 组。
//                  本应用仍绝不附带/下载/静默安装任何内核驱动或模块——
//                  PawnIO 及其模块均由用户从官方渠道自行放置（红线不变）。
//
// 阻塞调用：在 ops 任务队列上运行。绝不抛异常；允许部分结果。
// F3 差值窗口使每次读取多睡约 350 ms（页面刷新间隔 >=10 s）。
#include <winsock2.h>  // 必须在 iphlpapi/netioapi 之前包含（LEAN_AND_MEAN 会隐藏 winsock）
#include <ws2tcpip.h>  // 引入 ws2ipdef.h -> 为 netioapi 的 MIB_* 声明定义 _WS2IPDEF_
#include "collect/Sensors.h"
#include "collect/CollectDetail.h"  // cd:: PDH 通配辅助（同一库）
#include "collect/PawnIoLink.h"     // D6：PawnIO 可选内核通道（温度/风扇/电压）
#include "core/HandleGuard.h"
#include "core/Log.h"
#include "core/Str.h"
#include <windows.h>
#include <iphlpapi.h>   // GetIfTable2 / FreeMibTable（iphlpapi 在链接行上）
#include <netioapi.h>   // MIB_IF_TABLE2 / MIB_IF_ROW2
#include <psapi.h>      // PERFORMANCE_INFORMATION（下文动态绑定）
#include <winioctl.h>   // SMART_* / STORAGE_* ioctl（也定义 DEVICE_TYPE）
#include <ntddstor.h>   // StorageDeviceProtocolSpecificProperty + NVMe 日志页
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
// 动态辅助（不在链接行上的库：oleaut32、powrprof，无需 ws2_32）
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
// V15/P1：统一的 VARIANT 清理。VariantClear 恰好一次地释放
// 属性读取所产生的资源（BSTR、SAFEARRAY 含其数据块等）。
// 之前 VT_ARRAY|VT_UI1 VendorSpecific 路径只配对了
// Access/UnaccessData，每次刷新每行泄漏一个 SAFEARRAY。
VariantClearFn VariantClr() {
    static VariantClearFn fn = []() -> VariantClearFn {
        const HMODULE h = ::GetModuleHandleW(L"oleaut32.dll");
        return h ? reinterpret_cast<VariantClearFn>(::GetProcAddress(h, "VariantClear"))
                 : nullptr;
    }();
    return fn;
}
// SAFEARRAY 字节访问（oleaut32；仅 G-B "extra" 的 WMI SMART 路径需要）。
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
// 作用域 BSTR（需要 oleaut32，已在上方绑定）。
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
// 本地 GUID（不链接 wbemuuid.lib）。
constexpr GUID kCLSID_WbemLocator = {
    0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
constexpr GUID kIID_IWbemLocator = {
    0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
// 本地 WBEM 常量（值依 wbemcli.h；命名 k* 以避免与 #define 漂移）。
constexpr HRESULT kWbemAccessDenied = 0x80041003;     // WBEM_E_ACCESS_DENIED
constexpr HRESULT kWbemNotFound = 0x80041002;         // WBEM_E_NOT_FOUND
constexpr HRESULT kWbemInvalidClass = 0x80041010;     // WBEM_E_INVALID_CLASS
constexpr HRESULT kWbemInvalidNamespace = 0x8004100E; // WBEM_E_INVALID_NAMESPACE
constexpr HRESULT kEAccessDenied = 0x80070005;        // E_ACCESSDENIED (DCM level)
constexpr long kFlagForwardOnly = 0x10;               // WBEM_FLAG_FORWARD_ONLY
constexpr long kFlagReturnImmediately = 0x20;         // WBEM_FLAG_RETURN_IMMEDIATELY
void EnsureComSecurity() {
    // 每进程至多运行一次；RPC_E_TOO_LATE（已被他人设置）
    // 也没关系。我们只断言对 WMI 友好的默认值。
    static const bool done = []() {
        (void)::CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                     RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
        return true;
    }();
    (void)done;
}
// ===========================================================================
// 极简 WMI 查询辅助
// ===========================================================================
struct WmiVal {
    bool present = false;
    bool isNum = false;
    std::wstring str;
    uint32_t num = 0;
    std::vector<uint8_t> bytes;  // VT_ARRAY|VT_UI1（G-B：SMART VendorSpecific）
};
using WmiRow = std::map<std::wstring, WmiVal>;
struct WmiResult {
    bool ok = false;            // 已执行；rows（可能为空）即有效
    bool denied = false;        // 受提权限制 -> NeedAdmin
    bool notSupported = false;  // 命名空间/类缺失 -> NoHardware
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
    VARIANT var{};  // 零初始化 == VariantInit（不链接 oleaut32）
    var.vt = VT_EMPTY;
    if (FAILED(obj->Get(name, 0, &var, nullptr, nullptr))) return v;
    if (var.vt == VT_BSTR && var.bstrVal) {
        v.present = true;
        v.str = var.bstrVal;
    } else if (var.vt == (VT_ARRAY | VT_UI1) && var.parray) {
        // 字节向量（G-B：MSStorageDriver_FailurePredictData.VendorSpecific）。
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
            v.bytes.assign(bytes, bytes + (n > 4096 ? 4096 : n));  // 限制拷贝大小
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
    // V15/P1：IWbemClassObject::Get 返回的 VARIANT 拥有其载荷。
    // 先拷贝（上文），然后把所有权交给 VariantClear 且仅一次——
    // 这取代了旧的手工 SysFreeString（双重释放隐患），并补上
    // 缺失的 SAFEARRAY 销毁。仅当 oleaut32 在绑定点之间消失
    // 时才走退化兜底；泄漏退化为只剩 BSTR 不释放。
    const VariantClearFn clear = VariantClr();
    if (clear) {
        clear(&var);
    } else if (var.vt == VT_BSTR && var.bstrVal) {
        const SysFreeStringFn f = SysFree();
        if (f) f(var.bstrVal);
    }
    return v;
}
// 一次查询，按行取 `props`。行数上限让卡死的提供程序保持有界。
WmiResult WmiQuery(const wchar_t* ns, const wchar_t* wql,
                   const std::vector<const wchar_t*>& props) {
    WmiResult r;
    const HRESULT ci = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE：本线程 COM 已以 STA 启动——可用，但初始化
    // 不属于我们，绝不能为它配对 CoUninitialize。
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
            // G-B 诚实性修复：受提权限制类的访问拒绝可能
            // 在枚举阶段而非 ExecQuery 阶段暴露（实测
            // MSAcpi_ThermalZoneTemperature 非管理员：ExecQuery S_OK，第一次
            // Next() -> 0x80041003 WBEM_E_ACCESS_DENIED）。没有这一步，
            // snapshot claimed NoHardware ("本机无此传感器") where the truth is
            // NeedAdmin。（旧代码在本机正好踩中这一点。）
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
// CPU：频率（有文档，无需管理员）+ 整包温度（ACPI 热区）
// ===========================================================================
#pragma pack(push, 4)
struct ProcessorPowerInfo {  // PROCESSOR_POWER_INFORMATION（winnt.h，有文档）
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
        if (p.CurrentMhz == 0) continue;  // 诚实：宁无读数也不给假 0
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
    // MSAcpi_ThermalZoneTemperature：一个实例集（每个 ACPI 热区一条，
    // 而非单一整包值）——由 ACPI 驱动实现，单位 0.1 K。
    // G-B：枚举每个实例（遍历 WMI 集合），每实例一条读数。
    // zone, labeled "ACPI 热区 N" (+ InstanceName when the row carries one).
    // Non-admin is usually rejected (R6 §3: this box reports 拒绝访问).
    const WmiResult w = WmiQuery(L"ROOT\\WMI",
                                 L"SELECT InstanceName, CurrentTemperature FROM "
                                 L"MSAcpi_ThermalZoneTemperature",
                                 {L"InstanceName", L"CurrentTemperature"});
    if (w.denied && w.rows.empty()) {
        // 常见的非管理员情形：还没枚举到实例就被拒绝。
        SensorReading r;
        r.label = L"ACPI 热区温度";
        r.unit = L"°C";
        r.state = SensorReading::State::NeedAdmin;
        cpu->push_back(r);
        *notes += L"；ACPI 热区温度需管理员权限";
        return;
    }
    // V15/P2 诚实性：locator/connect/proxy/query 阶段的未分类失败
    // 意味着 WMI 基础设施不可用——热区来源
    // itself is unproven. That must not render as NoHardware ("本机无此传感器").
    // 在冻结的四态契约内表现为 NeedAdmin +
    // 明确备注提权也无济于事（label 同样如实标注）。
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
            continue;  // 诚实：宁无读数也不给假 0
        }
        const double c = static_cast<double>(it->second.num) / 10.0 - 273.15;
        if (c < -60.0 || c > 250.0) {
            ++invalid;
            continue;  // 不合理 -> 不伪造读数
        }
        SensorReading r;
        std::wstring zone = Fmt(L"ACPI 热区 {}", rowIdx);
        const auto in = row.find(L"InstanceName");  // 存在但可能为空
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
        // 罕见：已枚举部分实例后才被拒绝而中断。
        *notes += L"；ACPI 热区枚举被拒绝（结果可能不完整，需管理员权限）";
    }
}
// ===========================================================================
// GPU: NVML only (driver-supplied nvml.dll in System32). Absent -> empty gpu
// 向量 + 诚实备注；对不支持的厂商绝不伪造数值。
// G-B：每卡经 NVML 展开为每传感器一条读数（温度、
// 减速温度阈值、功耗、GPU 利用率、显存利用率、风扇），而不是
// single aggregate — the user asked for "同类传感器多个值都展示出来". The
// 旧 `gpu` 向量保持原 F3 内容（温度/GPU 利用率/功耗）以保证
// 契约稳定；完整展开放入 `gpus` 组。
// ===========================================================================
namespace nvml {
using Device = void*;
constexpr int kRetSuccess = 0;  // nvmlReturn_t NVML_SUCCESS（保留原 API 名）
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
using GetPowerFn = int (*)(Device, unsigned*);  // 单位毫瓦
using GetThreshFn = int (*)(Device, int, unsigned*);  // 温度阈值，°C
using GetFanFn = int (*)(Device, unsigned*);  // 最大值的百分比
struct Fns {
    InitFn init = nullptr;
    ShutdownFn shutdown = nullptr;
    GetCountFn count = nullptr;
    GetHandleFn handle = nullptr;
    GetTempFn temp = nullptr;
    GetUtilFn util = nullptr;
    GetPowerFn power = nullptr;
    GetThreshFn thresh = nullptr;  // 可选导出
    GetFanFn fan = nullptr;        // 可选导出
};
// 加载 C:\Windows\System32\nvml.dll（驱动所有；绝对路径——绝不
// 按搜索顺序加载）。不可用时返回 false 并填写 notes。
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
        // --- 温度（旧向量同时保留其 F3 条目）---
        unsigned tempC = 0;
        if (f.temp(dev, kTempGpu, &tempC) == kRetSuccess && tempC > 0) {
            const SensorReading r = Make(Fmt(L"GPU {} 温度", i).c_str(),
                                         static_cast<double>(tempC), L"°C");
            legacy->push_back(r);      // 既有契约向量
            expanded->push_back(r);    // G-B 每传感器组
        }
        // --- 减速温度阈值（每卡，当驱动知晓时）---
        if (f.thresh) {
            unsigned slowC = 0;
            if (f.thresh(dev, kThreshSlowdown, &slowC) == kRetSuccess && slowC > 0) {
                expanded->push_back(
                    Make(Fmt(L"GPU {} 慢速温度阈值", i).c_str(), static_cast<double>(slowC), L"°C"));
            }
        }
        // --- 功耗（W）---
        if (f.power) {
            unsigned mw = 0;
            if (f.power(dev, &mw) == kRetSuccess && mw > 0) {
                const SensorReading r = Make(Fmt(L"GPU {} 功耗", i).c_str(),
                                             static_cast<double>(mw) / 1000.0, L"W");
                legacy->push_back(r);
                expanded->push_back(r);
            }
        }
        // --- 利用率：gpu 与 memory 分量各出一条读数 ---
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
        // --- 风扇（NVML 报告最大值的百分比；0% 是真实读数——零转速待机
        // 模式——因此与温度不同，为 0 时保持 Ok）---
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
// 磁盘：每 PhysicalDrive 的健康状态，任何权限层级都保持诚实
// ===========================================================================
enum class BusProto { Nvme, Ata, Usb, Other };
BusProto ClassifyBus(uint32_t busType) {
    // STORAGE_BUS_TYPE：3=ATA 2=ATAPI 11=SATA 17=NVMe 7=USB ...
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
    // 去除尾部空格（ATA identify 填充）。
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return {};
    return Utf8ToWide(out);
}
// IOCTL_STORAGE_QUERY_PROPERTY(StorageDeviceProtocolSpecificProperty) ->
// NVMe Get Log Page 0x02 SMART/Health（有文档 "Working with NVMe drives"）。
// F3：除 pctUsed 外还报告可用备余百分比 / 备余阈值百分比。
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
    spd->ProtocolDataRequestValue = 0x02;  // SMART / 健康信息日志页
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
    // NVMe SMART/Health 日志（512 B，NVMe 1.3/1.4 Get Log Page 02h 布局）：
    //   [0] 严重警告          [1-2] 综合温度（小端，开尔文）
    //   [3] 可用备余          [4] 可用备余阈值
    //   [5] 已用百分比        [32-47] 已读数据单元   [128-143] 通电小时数
    //（V9 P0-2：旧的偏移 2-3 / 32-39 读到的是备余%|温度高字节与
    // 已读数据单元——字段错了。取值先钳制到合理域内；
    // 域外一律记为"未知"，绝不给出伪造读数。）
    *critWarn = log[0];
    *pctUsed = log[5];
    // F3：备余字段；0xFF 是未填充的占位，按不可得上报。
    *sparePct = log[3] == 0xFF ? UINT32_MAX : log[3];
    *spareThreshPct = log[4] == 0xFF ? UINT32_MAX : log[4];
    const uint16_t tempK = static_cast<uint16_t>(log[1] | (log[2] << 8));
    constexpr double kMinC = -20.0, kMaxC = 120.0;
    const double c = static_cast<double>(tempK) - 273.15;
    *tempC = (c >= kMinC && c <= kMaxC) ? c : 0.0;  // 0 => 调用方按 NoHardware 上报
    uint64_t hours = 0;
    for (int i = 7; i >= 0; --i) hours = (hours << 8) | log[128 + i];  // 通电小时数 @128
    constexpr uint64_t kMaxHours = 200000;  // 约 23 年；超过即视为垃圾数据
    *poh = (hours <= kMaxHours) ? hours : UINT64_MAX;
    return true;
}
// 经 SMART_RCV_DRIVE_DATA 的经典 SMART READ DATA（winioctl.h，ATA 盘）。
bool AtaSmart(HANDLE h, uint8_t driveIndex, double* tempC, uint64_t* poh) {
    GETVERSIONINPARAMS ver{};
    DWORD ret = 0;
    if (!::DeviceIoControl(h, SMART_GET_VERSION, nullptr, 0, &ver, sizeof(ver), &ret, nullptr)) {
        return false;
    }
    if ((ver.fCapabilities & CAP_SMART_CMD) == 0) return false;
    SENDCMDINPARAMS inp{};
    inp.cBufferSize = 512;
    inp.irDriveRegs.bFeaturesReg = 0xD0;  // SMART READ ATTRIBUTE VALUES（读取属性值）
    inp.irDriveRegs.bSectorCountReg = 1;
    inp.irDriveRegs.bSectorNumberReg = 1;
    inp.irDriveRegs.bCylLowReg = 0x4F;
    inp.irDriveRegs.bCylHighReg = 0xC2;
    inp.irDriveRegs.bDriveHeadReg = 0xA0;
    inp.irDriveRegs.bCommandReg = 0xB0;  // SMART 命令
    inp.bDriveNumber = driveIndex;
    std::vector<BYTE> outb(sizeof(SENDCMDOUTPARAMS) + 512 - 1);
    if (!::DeviceIoControl(h, SMART_RCV_DRIVE_DATA, &inp, sizeof(inp), outb.data(),
                           static_cast<DWORD>(outb.size()), &ret, nullptr)) {
        return false;
    }
    const auto* outp = reinterpret_cast<const SENDCMDOUTPARAMS*>(outb.data());
    if (outp->DriverStatus.bDriverError != 0) return false;
    const BYTE* s = outp->bBuffer;  // 512 字节属性块
    // 属性布局（ATA/ATAPI-6 SMART READ DATA；与 smartmontools 的
    // ata_smart_attribute 及 CrystalDiskInfo 一致——即 R6 引用的参考）：
    //   id(1) flags(2) value(1) worst(1) raw(6, 小端) reserved(1)；2 字节版本
    //   之后共 30 个条目。注意（V9 P0-3 评审）：评审曾提议 raw@p[4]/8B，
    //   但按本规范布局 p[4] 是 *worst* 归一化字节（0-100）；
    //   把它当摄氏度读会编造出貌似合理的假温度。因此我们
    //   保留 raw@p[5..10] 并执行严格的合理域校验，任何布局漂移
    //   都退化为"未知"，而不是假值。
    bool haveTemp = false, haveHours = false;
    double t = 0;
    uint64_t hours = 0;
    for (int a = 0; a < 30; ++a) {
        const BYTE* p = s + 2 + a * 12;  // 版本(2) 之后是 12 字节属性
        const uint8_t id = p[0];
        if (id == 0) break;
        if ((id == 194 || id == 190) && !haveTemp) {  // Temperature / Airflow（温度/气流）
            const double cand = static_cast<double>(p[5]);  // raw[0]（原始首字节）
            if (cand >= -20.0 && cand <= 120.0) {           // 仅接受合理的摄氏值
                t = cand;
                haveTemp = true;
            }
        } else if (id == 9 && !haveHours) {  // Power_On_Hours（原始，小端，48 位）
            const uint64_t raw = static_cast<uint64_t>(p[5]) | (static_cast<uint64_t>(p[6]) << 8) |
                                 (static_cast<uint64_t>(p[7]) << 16) |
                                 (static_cast<uint64_t>(p[8]) << 24) |
                                 (static_cast<uint64_t>(p[9]) << 32) |
                                 (static_cast<uint64_t>(p[10]) << 40);
            if (raw > 0 && raw <= 200000ull) {  // 约 23 年；超过即垃圾数据
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
    // 先取粗粒度的 OS 级健康（标准用户在原版 Windows 上即可用；
    // StorageReliabilityCounter 需要管理员——R6 §5）。
    struct WmiDisk {
        uint32_t busNum = UINT32_MAX;
        uint16_t healthStatus = 0xFFFF;  // 0xFFFF = 未知
        std::wstring model, serial;
    };
    std::map<std::wstring, WmiDisk> wmi;  // DeviceId（"0"、"1"...）-> 信息
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
        // CreateFileW 失败时返回 INVALID_HANDLE_VALUE（-1）而非 NULL：
        // 与 NULL 比较会为每个不存在的盘号编造一条假磁盘行
        //（V9 P0-1）。core/HandleGuard 已冻结，因此在这里归一化：
        // 跳过无效值，绝不把它交给 UniqueHandle/CloseHandle。
        if (h0 == INVALID_HANDLE_VALUE) continue;  // 盘不存在；盘号空洞是合法的
        const stm::UniqueHandle h(h0);
        DiskHealth d;
        uint32_t busNum = UINT32_MAX;
        const auto wit = wmi.find(std::to_wstring(n));
        if (wit != wmi.end()) {
            busNum = wit->second.busNum;
            d.model = wit->second.model;
            d.serial = wit->second.serial;
            switch (wit->second.healthStatus) {  // MSFT_PhysicalDisk.HealthStatus（健康状态）
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
        // 描述符：即使 WMI 被拒绝也给出型号/序列号/总线（只读访问
        // 权限为 0 的句柄足以做普通属性查询）。
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
        // 绝不留空白行：WMI 与描述符都不可得时 -> 以盘号作为
        // 诚实的兜底名称。
        if (d.model.empty()) d.model = Fmt(L"PhysicalDrive{}", n);
        // 详细健康信息：只对合理报告 SMART 的总线类型执行。
        const BusProto proto = busNum != UINT32_MAX ? ClassifyBus(busNum) : BusProto::Other;
        if (proto == BusProto::Nvme || proto == BusProto::Ata) {
            bool ok = false;
            double t = 0;
            uint64_t poh = UINT64_MAX;
            uint32_t pct = 0;
            uint8_t crit = 0;
            uint32_t spare = UINT32_MAX, spareTh = UINT32_MAX;  // F3 NVMe 细节
            if (proto == BusProto::Nvme) {
                ok = NvmeHealth(h.get(), &t, &poh, &pct, &crit, &spare, &spareTh);
            } else {
                ok = AtaSmart(h.get(), static_cast<uint8_t>(n), &t, &poh);
            }
            if (!ok) {
                // 用提权句柄重试；按结果分类。
                const HANDLE h1 = ::CreateFileW(path.c_str(), GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                OPEN_EXISTING, 0, nullptr);
                if (h1 != INVALID_HANDLE_VALUE) {  // V9 P0-1：失败时不是 NULL
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
                    d.tempState = SensorReading::State::NoHardware;  // 日志正常但无温度传感器
                }
                if (proto == BusProto::Nvme) {
                    if (crit != 0) d.health = L"警告";       // real SMART critical warning
                    else if (pct > 100) d.health = L"警告";  // worn beyond rated endurance
                    else if (d.health == L"未知") d.health = L"良好";
                    // F3 细节（诚实：只来自真实的日志页读取；0xFF 填充
                    // 字节已在 NvmeHealth 内部映射为 UINT32_MAX）。
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
// F3：共享的 350 ms 差值窗口。一个 PDH 查询承载速率计数器
//（每核 CPU % 经 Processor Information；按 engtype 的 GPU Engine；
// GPU Adapter Memory 专用/共享），两个 GetIfTable2 样本给出
// 每适配器字节速率。所有 Ok 读数均来自有文档的用户态
// 来源（R6 §4/§6）；缺失的来源只产出一条 NoHardware 占位条目——
// 绝不编造 0。
// ===========================================================================
constexpr DWORD kDeltaWindowMs = 350;
// PDH 引擎类型记号 -> 短显示名；nullptr = 保留原始记号。
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
        if (haveProc || haveEng) stm::cd::PdhCollect(q);  // 速率计数器需要一个预热样本
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
    // --- 每核 CPU 利用率（Processor Information，有文档；R6 §6）---
    if (haveProc) {
        std::vector<stm::cd::PdhArrayItem> items;
        int added = 0;
        if (stm::cd::PdhFmtArrayDouble(procUtil, &items)) {
            for (const stm::cd::PdhArrayItem& it : items) {
                if (!it.valid) continue;
                if (it.name.empty() || it.name.find(L"_Total") != std::wstring::npos) continue;
                // 实例形如 "0,3"（处理器组,逻辑核）：最后一个逗号后的
                // 数字就是核心标签。
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
    // --- GPU 引擎利用率，按 engtype_* 聚合（R6 §4）---
    bool anyEngine = false;
    if (haveEng) {
        std::vector<stm::cd::PdhArrayItem> items;
        if (stm::cd::PdhFmtArrayDouble(gpuEng, &items)) {
            std::map<std::wstring, std::pair<double, int>> agg;  // engtype ->（总和, 实例数）
            for (const stm::cd::PdhArrayItem& it : items) {
                if (!it.valid) continue;
                const size_t e = it.name.find(L"engtype_");
                if (e == std::wstring::npos) continue;
                const std::wstring type = it.name.substr(e + 8);  // len(L"engtype_") == 8（前缀长度）
                if (type.empty()) continue;
                auto& slot = agg[type];
                slot.first += ClampPct(it.value);
                slot.second += 1;
            }
            for (const auto& kv : agg) {
                SensorReading r;
                const wchar_t* cn = EngineTypeLabel(kv.first);
                r.label = cn ? Fmt(L"GPU {} 引擎占用率", cn) : Fmt(L"GPU 引擎占用率（{}）", kv.first);
                r.value = ClampPct(kv.second.first);  // 聚合值，与单引擎一样做钳制
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
    // --- GPU 适配器内存，跨适配器求和（原始用量计数器）---
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
    // --- 每适配器网络速率（GetIfTable2 字节差值）---
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
            if (it0 == t0rows.end()) continue;  // 窗口中途出现：尚无诚实的差值
            const MIB_IF_ROW2& r0 = *it0->second;
            std::wstring name(r1.Alias);
            if (name.empty()) name = r1.Description;
            if (name.empty()) name = Fmt(L"接口 {}", static_cast<unsigned>(r1.InterfaceIndex));
            auto rate = [&](uint64_t now, uint64_t prev) {
                if (dtSec <= 0.0 || now < prev) return -1.0;  // 计数器重置不是尖峰
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
            } else {  // 诚实的占位，而不是回绕产生的尖峰
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
// F3：电池（CallNtPowerInformation SystemBatteryState=5，winnt.h 布局）。
// 无电池 -> 一条 NoHardware 条目（诚实；台式机是常态）。
// ===========================================================================
#pragma pack(push, 4)
struct BatteryStateRow {  // SYSTEM_BATTERY_STATE（系统电池状态）
    BYTE AcOnLine;
    BYTE BatteryPresent;
    BYTE Charging;
    BYTE Discharging;
    BYTE Spare4[3];
    DWORD MaxCapacity;
    DWORD RemainingCapacity;
    DWORD Rate;           // 毫瓦，当前充/放电速率
    DWORD EstimatedTime;  // 秒；0xFFFFFFFF = 未知
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
    if (bs.EstimatedTime != 0 && bs.EstimatedTime != 0xFFFFFFFF) {  // 0/-1 = 未知
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
// F3：内存（GlobalMemoryStatusEx + GetPerformanceInfo）。GetPerformanceInfo
// 动态绑定（kernel32 的 K32GetPerformanceInfo，psapi.dll 兜底），
// 因此 stm_collect 的链接行保持不变。
// ===========================================================================
using GetPerfInfoFn = BOOL(WINAPI*)(PPERFORMANCE_INFORMATION, DWORD);
GetPerfInfoFn GetPerfInfo() {
    static GetPerfInfoFn fn = []() -> GetPerfInfoFn {
        const HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
        if (k32) {
            const auto p = reinterpret_cast<GetPerfInfoFn>(::GetProcAddress(k32, "K32GetPerformanceInfo"));
            if (p) return p;
        }
        // 刻意永不释放：进程生命周期绑定，同上文的 powrprof。
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
// G-B："extra"——尽力从有文档的 WMI 温度类读取，
// 且位于 MSAcpi_ThermalZoneTemperature 之外。快照组只在非空时展示，
// 因此所有失败路径都保持沉默：缺失不作任何承诺。
// P2（2026-09-18）：每个来源逐实例枚举，每条标签带
// 来源前缀，不同来源的同名读数得以区分。
// providers can never collide (contract: "同类传感器多值逐项展示").
// ===========================================================================
void ReadExtraSensors(std::vector<SensorReading>* out) {
    // 1) Win32_Temperature（ROOT\CIMV2，DMTF 温度传感器）：每实例的
    //    CurrentReading。真实提供程序几乎从不实现——类缺失
    //    是常态，直接不产出。单位注意：数值以十分之一为单位，
    //    但提供程序之间有差异：十分之一开尔文（ACPI 风格）与
    //    十分之一摄氏度（DMTF 传感器模型）并存；两种解读
    //    不可能同时落在合理窗口内，因此选合理者，
    //    都不合理时整行丢弃（绝不给假温度）。
    {
        const WmiResult w = WmiQuery(L"ROOT\\CIMV2",
                                     L"SELECT InstanceName, CurrentReading FROM Win32_Temperature",
                                     {L"InstanceName", L"CurrentReading"});
        int added = 0;
        for (const WmiRow& row : w.rows) {
            const auto it = row.find(L"CurrentReading");
            if (it == row.end() || !it->second.present || !it->second.isNum) continue;
            const double raw = static_cast<double>(it->second.num);
            if (raw == 0.0) continue;  // 空寄存器哨兵值；真实 0.0 °C 读数不合理（V15-P2-3）
            double c = raw / 10.0 - 273.15;  // 先按十分之一开尔文
            if (c < -60.0 || c > 250.0) c = raw / 10.0;  // 否则按十分之一摄氏度
            if (c < -60.0 || c > 250.0) continue;        // 不合理 -> 跳过，绝不伪造
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
    // 2) MSStorageDriver_FailurePredictData（ROOT\WMI）：每盘的 SMART 属性
    //    块（与 AtaSmart 相同的 12 字节布局）；属性 194/190 的 raw[0] 是
    //    temperature. Usually admin-gated (this box: 拒绝访问) — a denial leaves
    //    该组保持不动；上面的每盘 IOCTL 路径仍是权威来源。
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
            // 版本(2) 之后是 12 字节属性：id(1) flags(2) value(1)
            // worst(1) raw(6, 小端) reserved(1)——raw[0] 在 p[5]，见 AtaSmart。
            for (size_t off = 2; off + 6 <= s.size(); off += 12) {
                const uint8_t id = s[off];
                if (id == 0) break;
                if ((id == 194 || id == 190) && !have) {
                    const double cand = static_cast<double>(s[off + 5]);
                    if (cand >= 10.0 && cand <= 120.0) {  // 仅接受合理的摄氏值
                        t = cand;
                        have = true;
                    }
                }
            }
            if (!have) continue;  // 诚实：不可读/缺失 -> 完全不出条目
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
    // 3) Win32_PerfFormattedData_Counters_ThermalZoneInformation（ROOT\CIMV2，
    //    PerfProc）：每个 ACPI 热区一个实例。文档单位是
    //    十分之一开尔文，但固件提供程序并不一致——本机（实测
    //    \_TZ.TZ00）raw=301，按十分之一摄氏度解读为 30.1 °C，
    //    按文档单位则是荒谬的 -243 °C。与来源 1 相同的单位阶梯：
    //    先按十分之一开尔文（只要合理就让文档单位胜出），
    //    十分之一摄氏度作为唯一备选，其余一律丢弃。
    //    无需提权即可读；许多台式机上缺失/损坏——
    //    任何失败或不合理值都保持沉默。
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
            if (raw == 0.0) continue;  // 空寄存器哨兵值；绝不报告假 0.0 °C（V15-P2-3）
            double c = raw / 10.0 - 273.15;  // 先按文档单位十分之一开尔文
            if (c < -60.0 || c > 250.0) c = raw / 10.0;  // 实测的十分之一摄氏度单位
            if (c < -60.0 || c > 250.0) continue;        // 不合理 -> 跳过，绝不伪造
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
    // 4) Intel DPTF（Dynamic Platform and Thermal Framework）参与者：
    //    root\Intel_DPTF 下驱动所有的 TEMPERATURE 实例集（较旧
    //    驱动为 root\Intel(DPTF)），CurrentTemperature 按十分之一
    //    开尔文计（DPTF 规范）。Intel 未公开文档化，且无驱动时缺失——
    //    命名空间缺失归类为 notSupported 并静默跳过
    //（这是常态）；只有合理的 Ok 行才会展示。
    for (const wchar_t* ns : {L"ROOT\\Intel_DPTF", L"ROOT\\Intel(DPTF)"}) {
        const WmiResult w = WmiQuery(ns,
                                     L"SELECT InstanceName, CurrentTemperature FROM TEMPERATURE",
                                     {L"InstanceName", L"CurrentTemperature"});
        if (w.notSupported) continue;  // 命名空间/类缺失：静默跳过
        int added = 0;
        for (const WmiRow& row : w.rows) {
            const auto it = row.find(L"CurrentTemperature");
            if (it == row.end() || !it->second.present || !it->second.isNum) continue;
            const double c = static_cast<double>(it->second.num) / 10.0 - 273.15;
            if (c < -60.0 || c > 250.0) continue;  // 不合理 -> 跳过，绝不伪造
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
        if (added > 0) break;  // 该命名空间已产出；不重复报告
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
        for (const SensorReading& r : coreFreq) snap.cpu.push_back(r);  // 既有契约向量
        // F3：共享的约 350 ms 差值窗口（每核 %、GPU 引擎/显存、每网卡速率）。
        std::vector<SensorReading> coreUtil, gpuEngine, net;
        ReadDeltaWindow(&coreUtil, &gpuEngine, &net, &notes);
        snap.cpuCores = std::move(coreFreq);
        snap.cpuCores.insert(snap.cpuCores.end(), coreUtil.begin(), coreUtil.end());
        // G-B：gpus = PDH 引擎/显存组 + NVML 每卡、每传感器的
        // 展开（旧 `gpu` 向量保持原 F3 内容）。
        snap.gpus = std::move(gpuEngine);
        nvml::Read(&snap.gpu, &snap.gpus, &notes);
        snap.network = std::move(net);
        ReadDisks(&snap.disks, &notes);
        ReadBattery(&snap.battery, &notes);
        ReadMemory(&snap.memory, &notes);
        ReadExtraSensors(&snap.extra);  // G-B：尽力而为；为空 -> 不展示
        snap.uptimeSec = static_cast<double>(::GetTickCount64()) / 1000.0;

        // ---- D6（2026-09-20）：PawnIO 可选内核通道（全部尽力而为、全诚实）----
        // 每核 DTS 温度：仅在 PawnIO 驱动就绪 + 官方签名模块已放置时出线。
        // 标签 N 取真实逻辑处理器序号+1（V26 P1-3：被合理域滤掉的核不占
        // 槽位，但后续核的标签仍与系统核编号对齐，不漂移）。
        {
            int dts[64] = {0};
            int dtsLp[64] = {0};
            const int n = pawnio::ReadCpuDtsTemps(dts, dtsLp, 64);
            if (n > 0) {
                for (int i = 0; i < n; ++i) {
                    SensorReading r;
                    r.label = Fmt(L"CPU 核心 {}（DTS）", dtsLp[i] + 1);
                    r.value = dts[i];
                    r.unit = L"°C";
                    r.state = SensorReading::State::Ok;
                    r.source = L"PawnIO";
                    snap.cpu.push_back(std::move(r));
                }
                notes += L"；每核 DTS 温度来自 PawnIO（官方签名模块，只读寄存器）";
            } else if (n == -2) {
                notes += L"；PawnIO 驱动在但官方签名模块未放置（每核 DTS 不可用）";
            }
        }
        // 风扇/电压：LpcIO 通道成功识别已知 SuperIO 芯片时填真实读数；
        // 无数据（芯片未知/无风扇）不算失败——保持 NeedDriver 诚实态。
        bool fansReal = false;
        {
            int rpms[7] = {0};
            const int nf = pawnio::ReadLpcIoFans(rpms, 7);
            if (nf > 0) {
                fansReal = true;
                const std::wstring chip = pawnio::LpcIoChipName();
                for (int i = 0; i < nf; ++i) {
                    SensorReading r;
                    r.label = chip.empty() ? Fmt(L"风扇 {}", i + 1)
                                           : Fmt(L"风扇 {}（{}）", i + 1, chip);
                    r.value = rpms[i];
                    r.unit = L"rpm";
                    r.state = SensorReading::State::Ok;
                    r.source = L"PawnIO";
                    snap.fans.push_back(std::move(r));
                }
            } else {
                SensorReading fan;
                fan.label = L"风扇转速";
                fan.value = 0.0;
                fan.unit = L"rpm";
                fan.state = SensorReading::State::NeedDriver;
                snap.fans.push_back(fan);
            }
            double volts[9] = {0.0};
            const int nv = pawnio::ReadLpcIoVoltages(volts, 9);
            if (nv > 0) {
                const std::wstring chip = pawnio::LpcIoChipName();
                for (int i = 0; i < nv; ++i) {
                    SensorReading r;
                    r.label = chip.empty() ? Fmt(L"主板电压 in{}", i)
                                           : Fmt(L"主板电压 in{}（{}）", i, chip);
                    r.value = volts[i];
                    r.unit = L"V";
                    r.state = SensorReading::State::Ok;
                    r.source = L"PawnIO";
                    snap.extra.push_back(std::move(r));
                }
            }
        }

        if (!fansReal) {
            notes += L"；风扇转速需要内核驱动（本应用不随包分发驱动，见调研 R6）";
        }
        notes += L"；CPU 占用率另见性能页；"
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
    if (notes.size() > 2) notes.erase(0, 1);  // 去掉开头的 '；'
    snap.notes = notes;
    return snap;
}
}  // namespace stm

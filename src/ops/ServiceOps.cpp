// System services (SCM) operations: enumeration + start/stop (contract ops/ServiceOps.h).
//
// Read paths (enumerate + config) use SERVICE_QUERY_* access only and work without
// admin (R5 row 12). Start/stop need admin for most system services; failures carry
// the Win32 HRESULT so the UI can show an honest "需要管理员" hint.
// Stop semantics: active dependents are stopped leaf-first (EnumDependentServices),
// each name re-checked against core::ProtectedList — a protected/critical service is
// refused, never force-killed (arch section 6).
#include "ops/ServiceOps.h"
#include "core/Err.h"
#include "core/Log.h"
#include "core/ProtectedList.h"
#include "core/Str.h"
#include <windows.h>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stm::ops {
namespace {

constexpr DWORD kStopTimeoutMs = 30'000;
constexpr DWORD kStartTimeoutMs = 15'000;
constexpr DWORD kPollIntervalMs = 200;
constexpr int kMaxDependencyDepth = 16;  // guards against cyclic dependency reports

// SC_HANDLE needs CloseServiceHandle (not CloseHandle) => dedicated RAII wrapper
// (resource charter, arch section 5).
class ScHandle {
public:
    ScHandle() = default;
    explicit ScHandle(SC_HANDLE h) : h_(h) {}
    ~ScHandle() { Reset(); }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    SC_HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
    void Reset() {
        if (h_) ::CloseServiceHandle(h_);
        h_ = nullptr;
    }

private:
    SC_HANDLE h_ = nullptr;
};

// Services.exe hosts nothing a process list would call by name, but the protection
// gate is name-based: a pseudo-pid (never 0/4) + "<name>.exe" lets core::ProtectedReason
// refuse e.g. a service literally named "services"/"csrss".
std::wstring ServiceProtectedReason(const std::wstring& name) {
    return stm::ProtectedReason(1u, name + L".exe", L"");
}

// Grow-loop wrapper: read one SERVICE_CONFIG_* blob. Returns null on failure (caller
// logs and degrades — the enum row stays usable without the detail).
template <typename T>
std::unique_ptr<T> QueryConfigBlob(SC_HANDLE svc, DWORD infoClass) {
    DWORD needed = 0;
    if (!::QueryServiceConfig2W(svc, infoClass, nullptr, 0, &needed) &&
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return nullptr;
    }
    if (needed == 0) return nullptr;
    auto buf = std::make_unique<BYTE[]>(needed);
    if (!::QueryServiceConfig2W(svc, infoClass, buf.get(), needed, &needed)) return nullptr;
    return std::unique_ptr<T>(reinterpret_cast<T*>(buf.release()));
}

std::unique_ptr<QUERY_SERVICE_CONFIGW> QueryConfig(SC_HANDLE svc) {
    DWORD needed = 0;
    if (!::QueryServiceConfigW(svc, nullptr, 0, &needed) &&
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return nullptr;
    }
    if (needed == 0) return nullptr;
    auto buf = std::make_unique<BYTE[]>(needed);
    if (!::QueryServiceConfigW(svc, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.get()),
                               needed, &needed)) {
        return nullptr;
    }
    return std::unique_ptr<QUERY_SERVICE_CONFIGW>(
        reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.release()));
}

// Poll QueryServiceStatusEx until dwCurrentState == target or timeout. Returns true
// only when the target state was observed.
bool WaitForState(SC_HANDLE svc, DWORD target, DWORD timeoutMs, std::wstring* err) {
    for (DWORD waited = 0;; waited += kPollIntervalMs) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (!::QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                    reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
            if (err) *err = stm::ErrContext(L"查询服务状态失败", stm::LastHr());
            return false;
        }
        if (ssp.dwCurrentState == target) return true;
        if (waited >= timeoutMs) {
            if (err) {
                *err = stm::Fmt(L"等待服务进入状态 {} 超时（{} 秒，当前状态代码 {}）",
                                target, timeoutMs / 1000, ssp.dwCurrentState);
            }
            return false;
        }
        ::Sleep(kPollIntervalMs);
    }
}

// Names of currently ACTIVE dependent services (empty on query failure — callers log).
std::vector<std::wstring> ActiveDependentsOf(SC_HANDLE svc) {
    std::vector<std::wstring> out;
    DWORD needed = 0, returned = 0;
    if (!::EnumDependentServicesW(svc, SERVICE_ACTIVE, nullptr, 0, &needed, &returned) &&
        ::GetLastError() != ERROR_MORE_DATA) {
        return out;
    }
    if (needed == 0) return out;
    std::vector<BYTE> buf(needed);
    if (!::EnumDependentServicesW(svc, SERVICE_ACTIVE,
                                  reinterpret_cast<ENUM_SERVICE_STATUSW*>(buf.data()),
                                  needed, &needed, &returned)) {
        return out;
    }
    const auto* arr = reinterpret_cast<const ENUM_SERVICE_STATUSW*>(buf.data());
    out.reserve(returned);
    for (DWORD i = 0; i < returned; ++i) out.emplace_back(arr[i].lpServiceName);
    return out;
}

// Recursive leaf-first stop. `scm` is owned by the caller; depth guards cycles.
bool StopOne(SC_HANDLE scm, const std::wstring& name, bool stopDependents, int depth,
             std::wstring* err) {
    if (depth > kMaxDependencyDepth) {
        if (err) *err = L"服务依赖链过深或存在循环依赖，已中止：" + name;
        return false;
    }
    // Protection gate BEFORE any handle open (same order as ProcessOps).
    if (const std::wstring reason = ServiceProtectedReason(name); !reason.empty()) {
        if (err) *err = L"已拒绝停止服务：" + reason + L"（保护名单强制拦截）";
        STM_LOG_INFO("services", L"停止请求被保护名单拦截：{}", name);
        return false;
    }
    const DWORD access = SERVICE_STOP | SERVICE_QUERY_STATUS |
                         (stopDependents ? SERVICE_ENUMERATE_DEPENDENTS : 0);
    ScHandle svc(::OpenServiceW(scm, name.c_str(), access));
    if (!svc) {
        if (err) *err = stm::ErrContext(L"打开服务 " + name + L" 失败", stm::LastHr());
        return false;
    }

    if (stopDependents) {
        // Leaf-first: every active dependent (and its own dependents) must be down
        // before the parent accepts SERVICE_CONTROL_STOP.
        for (const std::wstring& dep : ActiveDependentsOf(svc.get())) {
            std::wstring depErr;
            if (!StopOne(scm, dep, true, depth + 1, &depErr)) {
                if (err) *err = L"停止依赖服务失败：" + depErr;
                return false;
            }
        }
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    if (::QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO,
                               reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed) &&
        ssp.dwCurrentState == SERVICE_STOPPED) {
        return true;  // already stopped: idempotent success
    }
    SERVICE_STATUS status{};
    if (!::ControlService(svc.get(), SERVICE_CONTROL_STOP, &status)) {
        const uint32_t hr = stm::LastHr();
        if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE))) {
            return true;  // raced to stopped: still success
        }
        if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_DEPENDENT_SERVICES_RUNNING))) {
            if (err) *err = L"服务 " + name + L" 存在运行中的依赖服务（未允许停止依赖），无法停止";
            return false;
        }
        if (err) *err = stm::ErrContext(L"发送停止控制码失败（" + name + L"）", hr);
        return false;
    }
    if (!WaitForState(svc.get(), SERVICE_STOPPED, kStopTimeoutMs, err)) {
        STM_LOG_WARN("services", L"等待服务停止超时/失败：{}", err ? *err : L"");
        return false;
    }
    STM_LOG_INFO("services", L"服务已停止：{}", name);
    return true;
}

}  // namespace

std::vector<ServiceInfo> EnumServices(std::wstring* err) {
    std::vector<ServiceInfo> out;
    ScHandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
    if (!scm) {
        if (err) *err = stm::ErrContext(L"打开服务控制管理器失败", stm::LastHr());
        return out;
    }

    // Batch 1: name/state/pid for every SERVICE_WIN32 service in one call.
    std::vector<BYTE> buf(64 * 1024);
    DWORD needed = 0, returned = 0, resume = 0;
    for (;;) {
        if (::EnumServicesStatusExW(scm.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                                    SERVICE_STATE_ALL, buf.data(),
                                    static_cast<DWORD>(buf.size()), &needed, &returned,
                                    &resume, nullptr)) {
            break;
        }
        if (::GetLastError() != ERROR_MORE_DATA) {
            if (err) *err = stm::ErrContext(L"枚举服务失败", stm::LastHr());
            return out;
        }
        buf.resize(needed);
    }

    const auto* arr = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    out.reserve(returned);
    for (DWORD i = 0; i < returned; ++i) {
        const SERVICE_STATUS_PROCESS& ssp = arr[i].ServiceStatusProcess;
        ServiceInfo si;
        si.name = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
        si.displayName = arr[i].lpDisplayName ? arr[i].lpDisplayName : si.name;
        si.state = ssp.dwCurrentState;
        // dwProcessId is documented-valid only for the running/paused states (R5 7b);
        // pending/stopped rows report stale or zero values — normalize to 0 so
        // "pid != 0 => process alive" stays a usable invariant for the UI.
        const bool pidValid = ssp.dwCurrentState == SERVICE_RUNNING ||
                              ssp.dwCurrentState == SERVICE_PAUSED ||
                              ssp.dwCurrentState == SERVICE_PAUSE_PENDING ||
                              ssp.dwCurrentState == SERVICE_CONTINUE_PENDING;
        si.pid = pidValid ? ssp.dwProcessId : 0;
        out.push_back(std::move(si));
    }

    // sharedProcess: one host pid serving several services (svchost-style). A service
    // owning its own process (or not running => pid 0) is not shared.
    std::unordered_map<uint32_t, int> pidUsers;
    for (const ServiceInfo& si : out) {
        if (si.pid != 0) ++pidUsers[si.pid];
    }
    for (ServiceInfo& si : out) {
        si.sharedProcess = si.pid != 0 && pidUsers[si.pid] > 1;
    }

    // Batch 2 (per service, skip-on-failure): start type, account, description from
    // QUERY_SERVICE_CONFIG / SERVICE_CONFIG_DESCRIPTION; canStop from the live
    // SERVICE_STATUS_PROCESS.dwControlsAccepted (QUERY_SERVICE_CONFIG has no such field).
    int enriched = 0;
    for (ServiceInfo& si : out) {
        ScHandle svc(::OpenServiceW(scm.get(), si.name.c_str(),
                                    SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
        if (!svc) {
            STM_LOG_WARN("services", L"QueryConfig 打开失败（跳过细节）：{}", si.name);
            continue;
        }
        if (const auto cfg = QueryConfig(svc.get())) {
            si.startType = cfg->dwStartType;
            si.account = cfg->lpServiceStartName ? cfg->lpServiceStartName : L"";
        } else {
            STM_LOG_WARN("services", L"QueryServiceConfig 失败（跳过细节）：{}", si.name);
        }
        SERVICE_STATUS_PROCESS ssp{};
        DWORD sspNeeded = 0;
        if (::QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO,
                                   reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &sspNeeded)) {
            si.canStop = (ssp.dwControlsAccepted & SERVICE_ACCEPT_STOP) != 0;
        }
        if (const auto desc =
                QueryConfigBlob<SERVICE_DESCRIPTIONW>(svc.get(), SERVICE_CONFIG_DESCRIPTION)) {
            si.description = desc->lpDescription ? desc->lpDescription : L"";
        }
        // SERVICE_CONFIG_FAILURE_ACTIONS is deliberately not fetched: the frozen
        // ServiceInfo contract carries no field for it and it doubles the per-service
        // query cost for nothing the UI can show.
        ++enriched;
    }
    STM_LOG_INFO("services", L"服务枚举完成：{} 项，其中 {} 项取到配置细节", out.size(), enriched);
    if (err) err->clear();
    return out;
}

std::vector<std::wstring> GetDependentServices(const std::wstring& name) {
    std::vector<std::wstring> out;
    ScHandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        STM_LOG_WARN("services", L"GetDependentServices：打开 SCM 失败（{}）", stm::LastHr());
        return out;
    }
    ScHandle svc(::OpenServiceW(scm.get(), name.c_str(),
                                SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS));
    if (!svc) {
        STM_LOG_WARN("services", L"GetDependentServices：打开 {} 失败（{}）", name, stm::LastHr());
        return out;
    }
    return ActiveDependentsOf(svc.get());
}

bool StartServiceByName(const std::wstring& name, std::wstring* err) {
    ScHandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        if (err) *err = stm::ErrContext(L"打开服务控制管理器失败", stm::LastHr());
        return false;
    }
    ScHandle svc(::OpenServiceW(scm.get(), name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS));
    if (!svc) {
        if (err) *err = stm::ErrContext(L"打开服务 " + name + L" 失败", stm::LastHr());
        return false;
    }
    if (!::StartServiceW(svc.get(), 0, nullptr)) {
        const uint32_t hr = stm::LastHr();
        if (hr != static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_SERVICE_ALREADY_RUNNING))) {
            if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))) {
                if (err) {
                    *err = L"启动服务 " + name + L" 失败：需要管理员权限（" + stm::HrMessage(hr) + L"）";
                }
                return false;
            }
            if (err) *err = stm::ErrContext(L"启动服务 " + name + L" 失败", hr);
            return false;
        }
        STM_LOG_INFO("services", L"服务已在运行（视为启动成功）：{}", name);
    }
    if (!WaitForState(svc.get(), SERVICE_RUNNING, kStartTimeoutMs, err)) {
        return false;
    }
    STM_LOG_INFO("services", L"服务已启动：{}", name);
    if (err) err->clear();
    return true;
}

bool StopServiceByName(const std::wstring& name, bool stopDependents, std::wstring* err) {
    ScHandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        if (err) *err = stm::ErrContext(L"打开服务控制管理器失败", stm::LastHr());
        return false;
    }
    const bool ok = StopOne(scm.get(), name, stopDependents, 0, err);
    if (ok && err) err->clear();
    return ok;
}

}  // namespace stm::ops

// 系统服务（SCM）操作：枚举 + 启动/停止（契约 ops/ServiceOps.h）。
//
// 读取路径（枚举 + 配置）只用 SERVICE_QUERY_* 访问权，无需管理员即可工作
//（R5 第 12 行）。启动/停止对多数系统服务需要管理员；失败信息附带
// the Win32 HRESULT so the UI can show an honest "需要管理员" hint.
// 停止语义：活动依赖者先叶子后根地停止（EnumDependentServices），
// 每个名称都对照 core::ProtectedList 复查——受保护/关键的服务
// 一律拒绝，绝不强杀（架构第 6 节）。
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
constexpr int kMaxDependencyDepth = 16;  // 防御循环依赖上报

// SC_HANDLE 需要 CloseServiceHandle（而非 CloseHandle）=> 专用 RAII 包装
//（资源章程，架构第 5 节）。
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

// Services.exe 不承载进程列表会按名称调用的东西，但保护闸门
// 是按名称的：伪 pid（绝不 0/4）+ "<名称>.exe" 让 core::ProtectedReason
// 能拒绝例如字面名为 "services"/"csrss" 的服务。
std::wstring ServiceProtectedReason(const std::wstring& name) {
    return stm::ProtectedReason(1u, name + L".exe", L"");
}

// 增长循环包装：读取一段 SERVICE_CONFIG_* 数据。失败返回 null（调用方
// 记日志并降级——枚举行在缺该细节时仍可用）。
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

// 轮询 QueryServiceStatusEx 直到 dwCurrentState == 目标或超时。仅当
// 观察到目标状态时返回 true。
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

// 当前处于活动状态的依赖服务名称（查询失败时为空——调用方记日志）。
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

// 递归的先叶子停止。`scm` 由调用方持有；深度防御循环。
bool StopOne(SC_HANDLE scm, const std::wstring& name, bool stopDependents, int depth,
             std::wstring* err) {
    if (depth > kMaxDependencyDepth) {
        if (err) *err = L"服务依赖链过深或存在循环依赖，已中止：" + name;
        return false;
    }
    // 在任何句柄打开之前过保护闸门（与 ProcessOps 同序）。
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
        // 先叶子：每个活动依赖者（及其依赖者）必须先停止，
        // 父服务才接受 SERVICE_CONTROL_STOP。
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
        return true;  // 已停止：幂等成功
    }
    SERVICE_STATUS status{};
    if (!::ControlService(svc.get(), SERVICE_CONTROL_STOP, &status)) {
        const uint32_t hr = stm::LastHr();
        if (hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE))) {
            return true;  // 竞争中已停止：仍算成功
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

    // 第 1 批：一次调用取全部 SERVICE_WIN32 服务的名称/状态/pid。
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
        // dwProcessId 仅在运行/暂停状态有文档保证（R5 7b）；
        // 挂起/停止行报告陈旧或零值——归一化为 0，使
        // "pid != 0 => 进程存活" 仍是 UI 可用的不变式。
        const bool pidValid = ssp.dwCurrentState == SERVICE_RUNNING ||
                              ssp.dwCurrentState == SERVICE_PAUSED ||
                              ssp.dwCurrentState == SERVICE_PAUSE_PENDING ||
                              ssp.dwCurrentState == SERVICE_CONTINUE_PENDING;
        si.pid = pidValid ? ssp.dwProcessId : 0;
        out.push_back(std::move(si));
    }

    // sharedProcess：一个宿主 pid 服务多个服务（svchost 式）。独占
    // 进程的服务（或未运行 => pid 0）不算共享。
    std::unordered_map<uint32_t, int> pidUsers;
    for (const ServiceInfo& si : out) {
        if (si.pid != 0) ++pidUsers[si.pid];
    }
    for (ServiceInfo& si : out) {
        si.sharedProcess = si.pid != 0 && pidUsers[si.pid] > 1;
    }

    // 第 2 批（每服务，失败即跳过）：启动类型、账户、描述来自
    // QUERY_SERVICE_CONFIG / SERVICE_CONFIG_DESCRIPTION；canStop 来自实时的
    // SERVICE_STATUS_PROCESS.dwControlsAccepted（QUERY_SERVICE_CONFIG 无此字段）。
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
        // 刻意不取 SERVICE_CONFIG_FAILURE_ACTIONS：冻结的 ServiceInfo
        // 契约没有对应字段，且它会使每服务查询开销翻倍，
        // 而 UI 无处可展示。
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

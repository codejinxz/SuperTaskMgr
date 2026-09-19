// 四来源启动项 + 先备份后写入的启用/禁用
//（契约 ops/StartupOps.h；架构第 9 节可逆性；R5 表行 14a-14d）。
//
// 来源：注册表 Run/RunOnce（+ WOW6432Node 即 RegRun32）、启动文件夹（.lnk）、
// 登录/开机触发计划任务（任务计划 COM）、UWP StartupTask State 值。
// 禁用状态存于未公开的 StartupApproved\{Run,Run32,
// StartupFolder} 二进制值（12 字节，低位奇字节 = 禁用，R5 14a2）——
// write-back semantics are community-verified only, hence the 实验性 log marker
//（R5 表 B #1）。每次切换都先把原值备份到
// %LOCALAPPDATA%\SuperTaskMgr\startup_backup\<净化 id>.txt 之后再写入；
// 备份失败则拒绝写入。
//
// 线程：契约函数在 ops 任务线程上运行——COM（shell 链接、任务计划）
// 按调用初始化 COINIT_APARTMENTTHREADED，并在作用域退出时
// 配对 CoUninitialize（架构第 5 节资源章程）。
#include "ops/StartupOps.h"
#include "core/Err.h"
#include "core/FsUtil.h"
#include "core/Log.h"
#include "core/Privilege.h"
#include "core/Str.h"
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <shlobj.h>
#include <taskschd.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace stm::ops {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunOnceKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr wchar_t kRun32Key[] = L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedBase[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved";
constexpr wchar_t kUwpBase[] =
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion"
    L"\\AppModel\\SystemAppData";
constexpr wchar_t kUwpDisplayNameBase[] =
    L"Software\\Classes\\Extensions\\ContractId\\Windows.StartupTask\\PackageId";
constexpr int kMaxTaskFolderDepth = 8;

// ---------------- RAII 辅助（资源章程）----------------

// HKEY 需要 RegCloseKey（而非 CloseHandle）=> 专用类型化 RAII 包装。
class RegKey {
public:
    RegKey() = default;
    explicit RegKey(HKEY h) : h_(h) {}
    ~RegKey() { Reset(); }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    HKEY get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
    void Reset() {
        if (h_) ::RegCloseKey(h_);
        h_ = nullptr;
    }

private:
    HKEY h_ = nullptr;
};

struct FindCloser {
    void operator()(void* h) const noexcept { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
};
using UniqueFind = std::unique_ptr<void, FindCloser>;

struct BstrGuard {
    BSTR b = nullptr;
    ~BstrGuard() { ::SysFreeString(b); }
};

struct CoTaskMemGuard {
    void* p = nullptr;
    ~CoTaskMemGuard() { ::CoTaskMemFree(p); }
};

// 配对护栏：S_FALSE（已初始化）仍需要配对的
// CoUninitialize，因此用 SUCCEEDED() 而非 S_OK。
class ComStaGuard {
public:
    ComStaGuard() : hr_(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComStaGuard() { if (SUCCEEDED(hr_)) ::CoUninitialize(); }
    bool Ok() const { return SUCCEEDED(hr_); }
    ComStaGuard(const ComStaGuard&) = delete;
    ComStaGuard& operator=(const ComStaGuard&) = delete;

private:
    HRESULT hr_;
};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** Put() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }

private:
    T* p_ = nullptr;
};

// ---------------- 小工具 ----------------

std::wstring HivePrefix(HKEY root) { return root == HKEY_LOCAL_MACHINE ? L"HKLM" : L"HKCU"; }

std::wstring BytesToHex(const std::vector<BYTE>& data) {
    static const wchar_t* digits = L"0123456789ABCDEF";
    std::wstring s;
    s.reserve(data.size() * 2);
    for (BYTE b : data) {
        s += digits[b >> 4];
        s += digits[b & 0x0F];
    }
    return s;
}

uint64_t Fnv1a(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

// 文件名安全的备份 id；过长 id 尾部截断并加 FNV-1a 标记。
std::wstring SanitizeId(const std::wstring& id) {
    std::wstring s;
    s.reserve(id.size());
    for (wchar_t c : id) {
        const bool bad = c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
                         c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 0x20;
        s += bad ? L'_' : c;
    }
    if (s.size() > 150) {
        s = s.substr(0, 100) + L"_" + stm::Fmt(L"{:016X}", Fnv1a(id)) + L"_" + s.substr(s.size() - 30);
    }
    return s;
}

struct RegValue {
    bool exists = false;
    std::vector<BYTE> data;
};

RegValue ReadRegBytes(HKEY root, const std::wstring& subkey, const std::wstring& name) {
    RegValue v;
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS) {
        return v;
    }
    RegKey key(raw);
    DWORD type = 0;
    DWORD size = 0;
    if (::RegQueryValueExW(key.get(), name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
        return v;
    }
    v.data.resize(size);
    DWORD got = size;
    if (size > 0 &&
        ::RegQueryValueExW(key.get(), name.c_str(), nullptr, &type, v.data.data(), &got) != ERROR_SUCCESS) {
        v.data.clear();
        return v;
    }
    v.exists = true;
    return v;
}

std::vector<std::wstring> EnumSubKeys(HKEY root, const std::wstring& path) {
    std::vector<std::wstring> out;
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &raw) != ERROR_SUCCESS) return out;
    RegKey key(raw);
    DWORD maxName = 0, count = 0;
    if (::RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &count, &maxName, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        return out;
    }
    std::wstring buf(maxName + 1, L'\0');
    for (DWORD i = 0; i < count; ++i) {
        DWORD len = static_cast<DWORD>(buf.size());
        LONG rc = ::RegEnumKeyExW(key.get(), i, buf.data(), &len, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_SUCCESS) out.emplace_back(buf.data(), len);
    }
    return out;
}

// StartupApproved 状态，三态：1 = 禁用（低位奇字节，R5 14a2），0 = 启用，
// -1 = 无值（默认启用）。两个配置单元都查（任务管理器对 HKLM 项也
// 尊重用户值）——任一禁用即禁用。
int ApprovedStateAny(const wchar_t* leaf, const std::wstring& valueName) {
    const std::wstring sub = std::wstring(kApprovedBase) + L"\\" + leaf;
    const int a = [&] {
        const RegValue v = ReadRegBytes(HKEY_CURRENT_USER, sub, valueName);
        return v.exists && !v.data.empty() ? (v.data[0] & 1) : -1;
    }();
    const int b = [&] {
        const RegValue v = ReadRegBytes(HKEY_LOCAL_MACHINE, sub, valueName);
        return v.exists && !v.data.empty() ? (v.data[0] & 1) : -1;
    }();
    if (a == 1 || b == 1) return 1;
    if (a == 0 || b == 0) return 0;
    return -1;
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, 0, nullptr, &raw))) return {};
    CoTaskMemGuard guard{raw};
    return std::wstring(raw);
}

std::wstring FileTitle(const std::wstring& fileName) {
    const size_t dot = fileName.rfind(L'.');
    return dot == std::wstring::npos ? fileName : fileName.substr(0, dot);
}

// 备份文件名的本地时间标记（评审 V9 P1-2）：每次切换都有自己的
// 文件，重复切换保留完整可逆链（架构第 9 节）。
std::wstring LocalTimestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return stm::Fmt(L"{:04d}{:02d}{:02d}-{:02d}{:02d}{:02d}",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// ---------------- backup (arch section 9: 先备份后写) ----------------

bool WriteTextFileUtf8(const std::wstring& path, const std::string& content) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
    const bool closed = fclose(f) == 0;
    return ok && closed;
}

// 在任何写入之前导出 "注册表路径 + 值名 + 原始字节（十六进制）+ 方向"。
// 带时间戳的文件名保留每次切换的备份（评审 V9 P1-2——固定
// 文件名会让第二次切换覆盖原备份、破坏可逆性）。
// 备份无法持久化时返回 false（=> 调用方拒绝写入）。
bool WriteBackup(const std::wstring& id, const std::wstring& fullPath,
                 const std::wstring& valueName, const RegValue& orig, bool enable,
                 std::wstring* backupPath, std::wstring* err) {
    const std::wstring dir = stm::LocalAppDataRoot() + L"\\startup_backup";
    if (stm::EnsureDir(dir).empty()) {
        if (err) *err = L"无法创建启动项备份目录：" + dir + L"（已拒绝写入）";
        return false;
    }
    const std::wstring path = dir + L"\\" + SanitizeId(id) + L"." + LocalTimestamp() + L".txt";
    std::string content = stm::WideToUtf8(
        Fmt(L"SuperTaskMgr startup backup\noperation={}\npath={}\nvalue={}\nbytes={}\nhex={}\n",
            enable ? L"enable" : L"disable", fullPath, valueName, orig.data.size(),
            orig.exists ? BytesToHex(orig.data) : std::wstring(L"-")));
    if (!orig.exists) {
        content += stm::WideToUtf8(L"note=原值不存在（默认启用态，本次为首次禁用）\n");
    }
    if (!WriteTextFileUtf8(path, content)) {
        if (err) *err = L"无法写入启动项备份文件：" + path + L"（已拒绝写入注册表）";
        return false;
    }
    if (backupPath) *backupPath = path;
    return true;
}

// 评审 V9 P1-1：TASK_UPDATE 重注册兜底是破坏性的（触发注册
// 触发器、可能重置自定义 ACL），因此先把任务的 XML 定义按相同
// 目录/命名规则导出；失败则拒绝写入。
bool WriteTaskXmlBackup(const std::wstring& id, const std::wstring& xml, bool enable,
                        std::wstring* backupPath, std::wstring* err) {
    const std::wstring dir = stm::LocalAppDataRoot() + L"\\startup_backup";
    if (stm::EnsureDir(dir).empty()) {
        if (err) *err = L"无法创建启动项备份目录：" + dir + L"（已拒绝修改计划任务）";
        return false;
    }
    const std::wstring path =
        dir + L"\\" + SanitizeId(id) + L"." + LocalTimestamp() + L".taskxml.txt";
    std::string content = stm::WideToUtf8(
        Fmt(L"SuperTaskMgr scheduled task backup\noperation={}\nid={}\n",
            enable ? L"enable" : L"disable", id));
    content += stm::WideToUtf8(xml);
    content += "\n";
    if (!WriteTextFileUtf8(path, content)) {
        if (err) *err = L"无法写入计划任务 XML 备份：" + path + L"（已拒绝修改计划任务）";
        return false;
    }
    if (backupPath) *backupPath = path;
    return true;
}

// ---------------- 来源 A：注册表 Run / RunOnce（+ WOW6432Node）----------------

void EnumRunKey(HKEY root, const wchar_t* subkey, StartupSource src, const wchar_t* approvedLeaf,
                bool canToggle, std::vector<StartupItem>* out) {
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(root, subkey, 0, KEY_READ, &raw) != ERROR_SUCCESS) {
        STM_LOG_DEBUG("startup", L"Run 键不可读（跳过）：{}", subkey);
        return;
    }
    RegKey key(raw);
    DWORD values = 0, maxName = 0, maxValue = 0;
    if (::RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           &values, &maxName, &maxValue, nullptr, nullptr) != ERROR_SUCCESS) {
        return;
    }
    std::wstring nameBuf(maxName + 1, L'\0');
    std::vector<BYTE> dataBuf(maxValue > 0 ? maxValue : 1);
    for (DWORD i = 0; i < values; ++i) {
        DWORD nameLen = static_cast<DWORD>(nameBuf.size());
        DWORD type = 0;
        DWORD dataLen = static_cast<DWORD>(dataBuf.size());
        const LONG rc = ::RegEnumValueW(key.get(), i, nameBuf.data(), &nameLen, nullptr, &type,
                                        dataBuf.data(), &dataLen);
        if (rc != ERROR_SUCCESS) continue;
        const std::wstring valueName(nameBuf.data(), nameLen);
        if (type != REG_SZ && type != REG_EXPAND_SZ) {
            STM_LOG_WARN("startup", L"Run 值非字符串类型已跳过：{}\\{}", subkey, valueName);
            continue;
        }
        // 命令原样保留（不展开 REG_EXPAND_SZ，不解析引号）。
        std::wstring command(reinterpret_cast<const wchar_t*>(dataBuf.data()),
                             dataLen / sizeof(wchar_t));
        while (!command.empty() && command.back() == L'\0') command.pop_back();

        StartupItem it;
        it.source = src;
        it.id = HivePrefix(root) + L"\\" + subkey + L"\\" + valueName;
        it.name = valueName;
        it.command = command;
        it.location = HivePrefix(root) + L"\\" + subkey;
        it.enabled = ApprovedStateAny(approvedLeaf, valueName) != 1;
        it.canToggle = canToggle;
        out->push_back(std::move(it));
    }
}

// ---------------- 来源 B：启动文件夹（.lnk）----------------

std::wstring ResolveLnkTarget(const std::wstring& lnkPath) {
    ComPtr<IShellLinkW> link;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(link.Put())))) {
        return {};
    }
    ComPtr<IPersistFile> pf;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(pf.Put())))) return {};
    if (FAILED(pf->Load(lnkPath.c_str(), STGM_READ))) return {};
    wchar_t buf[MAX_PATH]{};
    if (FAILED(link->GetPath(buf, MAX_PATH, nullptr, SLGP_RAWPATH))) return {};
    return std::wstring(buf);
}

void EnumStartupFolder(bool common, bool canToggle, std::vector<StartupItem>* out, bool* failed) {
    const std::wstring dir = KnownFolderPath(common ? FOLDERID_CommonStartup : FOLDERID_Startup);
    if (dir.empty()) {
        STM_LOG_WARN("startup", L"获取{}启动文件夹失败", common ? L"公共" : L"用户");
        if (failed) *failed = true;
        return;
    }
    WIN32_FIND_DATAW fd{};
    UniqueFind find(::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                       FindExSearchNameMatch, nullptr, 0));
    if (!find) return;  // 文件夹缺失/为空是常态，不是错误
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring fileName = fd.cFileName;
        if (fileName.size() < 4 || _wcsicmp(fileName.c_str() + fileName.size() - 4, L".lnk") != 0) {
            continue;
        }
        StartupItem it;
        it.source = StartupSource::StartupFolder;
        it.id = dir + L"\\" + fileName;
        it.name = FileTitle(fileName);
        it.command = ResolveLnkTarget(it.id);  // 尽力而为；解析不了为空
        it.location = dir;
        it.enabled = ApprovedStateAny(L"StartupFolder", fileName) != 1;
        it.canToggle = canToggle;
        out->push_back(std::move(it));
    } while (::FindNextFileW(find.get(), &fd));
}

// ---------------- 来源 C：计划任务（登录/开机触发）----------------

// 提取 EXEC 动作的程序路径；缺失或非 EXEC 时为空。
std::wstring TaskExecCommand(ITaskDefinition* def) {
    ComPtr<IActionCollection> actions;
    if (FAILED(def->get_Actions(actions.Put()))) return {};
    LONG count = 0;
    if (FAILED(actions->get_Count(&count))) return {};
    for (LONG i = 1; i <= count; ++i) {  // taskschd 集合从 1 开始
        ComPtr<IAction> action;
        if (FAILED(actions->get_Item(i, action.Put()))) continue;
        TASK_ACTION_TYPE type{};
        if (FAILED(action->get_Type(&type)) || type != TASK_ACTION_EXEC) continue;
        ComPtr<IExecAction> exec;
        if (FAILED(action->QueryInterface(IID_PPV_ARGS(exec.Put())))) continue;
        BstrGuard path;
        if (FAILED(exec->get_Path(&path.b)) || !path.b) return {};
        return path.b;
    }
    return {};
}

void AddTaskIfLogonBoot(IRegisteredTask* task, bool canToggleAll, std::vector<StartupItem>* out) {
    BstrGuard path;
    if (FAILED(task->get_Path(&path.b)) || !path.b) return;
    const std::wstring taskPath = path.b;

    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(task->get_Enabled(&enabled))) return;

    ComPtr<ITaskDefinition> def;
    if (FAILED(task->get_Definition(def.Put()))) return;
    ComPtr<ITriggerCollection> triggers;
    if (FAILED(def->get_Triggers(triggers.Put()))) return;
    LONG count = 0;
    if (FAILED(triggers->get_Count(&count))) return;

    bool logonOrBoot = false;
    for (LONG i = 1; i <= count && !logonOrBoot; ++i) {
        ComPtr<ITrigger> trigger;
        if (FAILED(triggers->get_Item(i, trigger.Put()))) continue;
        TASK_TRIGGER_TYPE2 type{};
        if (FAILED(trigger->get_Type(&type))) continue;
        logonOrBoot = type == TASK_TRIGGER_LOGON || type == TASK_TRIGGER_BOOT;
    }
    if (!logonOrBoot) return;

    const size_t slash = taskPath.rfind(L'\\');
    StartupItem it;
    it.source = StartupSource::ScheduledTask;
    it.id = taskPath;
    it.name = slash == std::wstring::npos ? taskPath : taskPath.substr(slash + 1);
    it.location = slash == std::wstring::npos ? L"\\" : taskPath.substr(0, slash);
    it.command = TaskExecCommand(def.get());
    it.enabled = enabled != VARIANT_FALSE;
    // 系统文件夹（\Microsoft\...）的任务定义通常需要管理员才能改。
    it.canToggle = canToggleAll ||
                   _wcsnicmp(taskPath.c_str(), L"\\Microsoft\\", 12) != 0;
    out->push_back(std::move(it));
}

void WalkTaskFolder(ITaskFolder* folder, int depth, bool canToggleAll,
                    std::vector<StartupItem>* out) {
    if (depth > kMaxTaskFolderDepth) return;
    ComPtr<IRegisteredTaskCollection> tasks;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.Put()))) {
        LONG count = 0;
        if (SUCCEEDED(tasks->get_Count(&count))) {
            for (LONG i = 1; i <= count; ++i) {  // VARIANT 索引从 1 开始
                VARIANT vi;
                ::VariantInit(&vi);
                vi.vt = VT_I4;
                vi.lVal = i;
                ComPtr<IRegisteredTask> task;
                if (SUCCEEDED(tasks->get_Item(vi, task.Put()))) {
                    AddTaskIfLogonBoot(task.get(), canToggleAll, out);
                }
            }
        }
    }
    ComPtr<ITaskFolderCollection> subs;
    if (SUCCEEDED(folder->GetFolders(0, subs.Put()))) {
        LONG count = 0;
        if (SUCCEEDED(subs->get_Count(&count))) {
            for (LONG i = 1; i <= count; ++i) {  // VARIANT 索引从 1 开始
                VARIANT vi;
                ::VariantInit(&vi);
                vi.vt = VT_I4;
                vi.lVal = i;
                ComPtr<ITaskFolder> sub;
                if (SUCCEEDED(subs->get_Item(vi, sub.Put()))) {
                    WalkTaskFolder(sub.get(), depth + 1, canToggleAll, out);
                }
            }
        }
    }
}

bool EnumScheduledTasks(bool elevated, std::vector<StartupItem>* out, std::wstring* sourceErr) {
    ComPtr<ITaskService> svc;
    HRESULT hr = ::CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(svc.Put()));
    if (FAILED(hr)) {
        if (sourceErr) *sourceErr = stm::Fmt(L"计划任务：创建 TaskScheduler COM 失败（0x{:08X}）",
                                             static_cast<uint32_t>(hr));
        return false;
    }
    VARIANT empty;
    ::VariantInit(&empty);
    if (FAILED(svc->Connect(empty, empty, empty, empty))) {
        if (sourceErr) *sourceErr = L"计划任务：连接任务计划程序服务失败";
        return false;
    }
    ComPtr<ITaskFolder> root;
    BstrGuard rootPath;
    rootPath.b = ::SysAllocString(L"\\");
    if (!rootPath.b || FAILED(svc->GetFolder(rootPath.b, root.Put()))) {
        if (sourceErr) *sourceErr = L"计划任务：打开根文件夹失败";
        return false;
    }
    WalkTaskFolder(root.get(), 0, elevated, out);
    return true;
}

// ---------------- 来源 D：UWP StartupTask State 值 ----------------

// 尽力而为的显示名：已注册 StartupTask 契约扩展对部分包带有
// DisplayName；否则回退 PFN 的包名段。
std::wstring UwpDisplayName(const std::wstring& pfn) {
    const std::wstring base = std::wstring(kUwpDisplayNameBase) + L"\\" + pfn;
    // 先找 PFN 键直下的 "DisplayName" 值……
    const RegValue direct = ReadRegBytes(HKEY_CURRENT_USER, base, L"DisplayName");
    if (direct.exists && direct.data.size() >= sizeof(wchar_t) &&
        direct.data.size() % sizeof(wchar_t) == 0) {
        std::wstring s(reinterpret_cast<const wchar_t*>(direct.data.data()),
                       direct.data.size() / sizeof(wchar_t));
        while (!s.empty() && s.back() == L'\0') s.pop_back();
        if (!s.empty()) return s;
    }
    for (const std::wstring& app : EnumSubKeys(HKEY_CURRENT_USER, base)) {
        const RegValue sub = ReadRegBytes(HKEY_CURRENT_USER, base + L"\\" + app, L"DisplayName");
        if (sub.exists && sub.data.size() >= sizeof(wchar_t)) {
            std::wstring s(reinterpret_cast<const wchar_t*>(sub.data.data()),
                           sub.data.size() / sizeof(wchar_t));
            while (!s.empty() && s.back() == L'\0') s.pop_back();
            if (!s.empty()) return s;
        }
    }
    const size_t under = pfn.rfind(L'_');
    return under == std::wstring::npos ? pfn : pfn.substr(0, under);
}

void EnumUwpStartup(std::vector<StartupItem>* out) {
    for (const std::wstring& pfn : EnumSubKeys(HKEY_CURRENT_USER, kUwpBase)) {
        const std::wstring pfnPath = std::wstring(kUwpBase) + L"\\" + pfn;
        for (const std::wstring& taskId : EnumSubKeys(HKEY_CURRENT_USER, pfnPath)) {
            const RegValue state = ReadRegBytes(HKEY_CURRENT_USER, pfnPath + L"\\" + taskId, L"State");
            if (!state.exists || state.data.size() < sizeof(DWORD)) continue;  // 不是启动任务
            const DWORD stateVal = *reinterpret_cast<const DWORD*>(state.data.data());
            StartupItem it;
            it.source = StartupSource::UwpStartupTask;
            it.id = L"HKCU\\" + pfnPath + L"\\" + taskId + L"\\State";
            it.name = UwpDisplayName(pfn);
            it.command = pfn + L"\\" + taskId;
            it.location = pfn;
            // StartupTaskState 映射（R5 14d）：2 = 启用；0/1/3/4 = 禁用语义。
            it.enabled = stateVal == 2;
            it.canToggle = true;  // HKCU 写入，普通用户权限
            out->push_back(std::move(it));
        }
    }
}

// ---------------- SetStartupEnabled：按来源写入器 ----------------

bool ParseRegId(const std::wstring& id, HKEY* root, std::wstring* body) {
    if (id.rfind(L"HKCU\\", 0) == 0) {
        *root = HKEY_CURRENT_USER;
        *body = id.substr(5);
        return true;
    }
    if (id.rfind(L"HKLM\\", 0) == 0) {
        *root = HKEY_LOCAL_MACHINE;
        *body = id.substr(5);
        return true;
    }
    return false;
}

// id 内嵌 "<键路径>\<值名>"；值名合法时可包含 '\'，因此
// 通过对候选切分点从右向左对真实键复查来定位。
bool ResolveValueSplit(HKEY root, const std::wstring& body, std::wstring* keyPath,
                       std::wstring* valueName) {
    size_t cut = body.rfind(L'\\');
    while (cut != std::wstring::npos) {
        const std::wstring candidateKey = body.substr(0, cut);
        const std::wstring candidateValue = body.substr(cut + 1);
        if (ReadRegBytes(root, candidateKey, candidateValue).exists) {
            *keyPath = candidateKey;
            *valueName = candidateValue;
            return true;
        }
        cut = cut == 0 ? std::wstring::npos : body.rfind(L'\\', cut - 1);
    }
    cut = body.rfind(L'\\');  // 回退到普通的最后一段切分
    if (cut == std::wstring::npos) return false;
    *keyPath = body.substr(0, cut);
    *valueName = body.substr(cut + 1);
    return true;
}

// 共享的 approved 值写入器：先备份原始字节，再写 12 字节状态
//（0x03 + 当前 FILETIME = 禁用；0x02 + 零时间戳 = 启用）。
// 实验性标记：Explorer 对写入值的识别仅有社区验证
//（R5 表 B #1）——记日志，绝不隐瞒。
bool WriteApprovedValue(HKEY root, const wchar_t* leaf, const std::wstring& valueName,
                        const std::wstring& idForBackup, bool enable, const wchar_t* kindLabel,
                        std::wstring* err) {
    const std::wstring approvedSub = std::wstring(kApprovedBase) + L"\\" + leaf;
    const RegValue orig = ReadRegBytes(root, approvedSub, valueName);

    HKEY raw = nullptr;
    const LONG opened = ::RegCreateKeyExW(root, approvedSub.c_str(), 0, nullptr,
                                          REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                          &raw, nullptr);
    if (opened != ERROR_SUCCESS) {
        const uint32_t hr = stm::LastHr();
        if (err) {
            *err = root == HKEY_LOCAL_MACHINE
                       ? L"修改该项需要管理员权限（" + stm::ErrContext(L"打开 StartupApproved", hr) + L"）"
                       : stm::ErrContext(L"打开 StartupApproved 失败", hr);
        }
        return false;
    }
    RegKey key(raw);

    std::wstring backupPath;
    if (!WriteBackup(idForBackup, HivePrefix(root) + L"\\" + approvedSub, valueName, orig,
                     enable, &backupPath, err)) {
        return false;
    }

    std::vector<BYTE> next(12, 0);
    const DWORD flag = enable ? 0x02 : 0x03;
    std::memcpy(next.data(), &flag, sizeof(flag));
    if (!enable) {
        FILETIME now{};
        ::GetSystemTimeAsFileTime(&now);
        std::memcpy(next.data() + 4, &now, sizeof(now));
    }
    if (::RegSetValueExW(key.get(), valueName.c_str(), 0, REG_BINARY, next.data(),
                         static_cast<DWORD>(next.size())) != ERROR_SUCCESS) {
        if (err) *err = stm::ErrContext(L"写入 StartupApproved 失败", stm::LastHr());
        return false;
    }
    STM_LOG_INFO("startup", L"{}：{} 已{}（备份 {}）", kindLabel, idForBackup,
                 enable ? L"启用" : L"禁用", backupPath);
    STM_LOG_WARN("startup", L"StartupApproved 写入语义为实验性（R5 表B#1），资源管理器识别性未完全验证");
    if (err) err->clear();
    return true;
}

bool ToggleRegistryApproved(const StartupItem& item, bool enable, const wchar_t* leaf,
                            std::wstring* err) {
    HKEY root = nullptr;
    std::wstring body;
    if (!ParseRegId(item.id, &root, &body)) {
        if (err) *err = L"无法识别的启动项标识：" + item.id;
        return false;
    }
    std::wstring keyPath, valueName;
    if (!ResolveValueSplit(root, body, &keyPath, &valueName)) {
        if (err) *err = L"启动项对应的注册表值已不存在：" + item.id;
        return false;
    }
    return WriteApprovedValue(root, leaf, valueName, item.id, enable, L"注册表启动项", err);
}

bool ToggleStartupFolder(const StartupItem& item, bool enable, std::wstring* err) {
    const size_t slash = item.id.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        if (err) *err = L"无法识别的启动项标识：" + item.id;
        return false;
    }
    const std::wstring dir = item.id.substr(0, slash);
    const std::wstring fileName = item.id.substr(slash + 1);
    HKEY root = nullptr;
    if (_wcsicmp(dir.c_str(), KnownFolderPath(FOLDERID_CommonStartup).c_str()) == 0) {
        root = HKEY_LOCAL_MACHINE;
    } else if (_wcsicmp(dir.c_str(), KnownFolderPath(FOLDERID_Startup).c_str()) == 0) {
        root = HKEY_CURRENT_USER;
    } else {
        if (err) *err = L"启动文件夹路径无法归属（既非用户也非公共启动文件夹）：" + dir;
        return false;
    }
    return WriteApprovedValue(root, L"StartupFolder", fileName, item.id, enable,
                              L"启动文件夹项目", err);
}

bool ToggleScheduledTask(const StartupItem& item, bool enable, std::wstring* err) {
    ComStaGuard com;
    if (!com.Ok()) {
        if (err) *err = L"修改计划任务失败：COM 初始化失败";
        return false;
    }
    ComPtr<ITaskService> svc;
    if (FAILED(::CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(svc.Put())))) {
        if (err) *err = L"修改计划任务失败：创建 TaskScheduler COM 对象失败";
        return false;
    }
    VARIANT empty;
    ::VariantInit(&empty);
    if (FAILED(svc->Connect(empty, empty, empty, empty))) {
        if (err) *err = L"修改计划任务失败：连接任务计划程序服务失败";
        return false;
    }

    const size_t slash = item.id.rfind(L'\\');
    if (slash == std::wstring::npos) {
        if (err) *err = L"无法识别的计划任务路径：" + item.id;
        return false;
    }
    const std::wstring folderPath =
        slash == 0 ? std::wstring(L"\\") : item.id.substr(0, slash);
    const std::wstring taskName = item.id.substr(slash + 1);

    BstrGuard folderB;
    folderB.b = ::SysAllocString(folderPath.c_str());
    ComPtr<ITaskFolder> folder;
    if (!folderB.b || FAILED(svc->GetFolder(folderB.b, folder.Put()))) {
        if (err) *err = stm::ErrContext(L"打开计划任务文件夹 " + folderPath + L" 失败",
                                        stm::LastHr());
        return false;
    }
    BstrGuard nameB;
    nameB.b = ::SysAllocString(taskName.c_str());
    ComPtr<IRegisteredTask> task;
    if (!nameB.b || FAILED(folder->GetTask(nameB.b, task.Put()))) {
        if (err) *err = stm::ErrContext(L"打开计划任务 " + item.id + L" 失败", stm::LastHr());
        return false;
    }

    const VARIANT_BOOL vb = enable ? VARIANT_TRUE : VARIANT_FALSE;
    if (SUCCEEDED(task->put_Enabled(vb))) {
        STM_LOG_INFO("startup", L"计划任务已{}：{}", enable ? L"启用" : L"禁用", item.id);
        if (err) err->clear();
        return true;
    }
    // 兜底（R5 14c / 表 B #5）：put_Enabled 在 C++ 中可能不可用——改写
    // 定义的设置并以 TASK_UPDATE 重注册。
    // 评审 V9 P1-1 加固，按序执行：
    //  1. 不支持的主体验证类型在任何动作发生前即被拒绝——
    //     用空用户/口令变体重注册 PASSWORD/S4U/GROUP 任务
    //     会"成功"但静默产出一个永不运行的任务（有文档）；
    //  2. 原任务 XML 导出至 startup_backup（相同命名规则）；
    //     备份失败则拒绝修改；
    //  3. 原 Principal.LogonType 原样透传（绝不用硬编码值），
    //     使 SYSTEM/服务账户/交互任务保有其身份；
    //     TASK_UPDATE 可能触发注册触发器——因此有第 2 步。
    ComPtr<ITaskDefinition> def;
    if (FAILED(task->get_Definition(def.Put()))) {
        if (err) *err = L"修改计划任务失败：读取任务定义失败：" + item.id;
        return false;
    }
    ComPtr<IPrincipal> principal;  // taskschd.h 中 C++ 接口名为 IPrincipal
    TASK_LOGON_TYPE logon = TASK_LOGON_NONE;
    if (FAILED(def->get_Principal(principal.Put())) ||
        FAILED(principal->get_LogonType(&logon))) {
        if (err) *err = L"修改计划任务失败：读取任务 Principal 失败，为避免破坏任务已拒绝重注册：" + item.id;
        return false;
    }
    bool logonRoundTrips = false;
    switch (logon) {  // 只有经得起空用户/口令重注册的类型
        case TASK_LOGON_NONE:
        case TASK_LOGON_INTERACTIVE_TOKEN:
        case TASK_LOGON_SERVICE_ACCOUNT:
        case TASK_LOGON_INTERACTIVE_TOKEN_OR_PASSWORD:
            logonRoundTrips = true;
            break;
        default:  // PASSWORD(1) / S4U(2) / GROUP(4) / 未来取值
            break;
    }
    if (!logonRoundTrips) {
        if (err) {
            *err = stm::Fmt(L"修改计划任务失败：该任务使用登录类型 {}（密码/S4U/组任务），"
                            L"重注册会使其静默失效，已拒绝修改且未做任何变更；"
                            L"请用任务计划程序或 schtasks /Change 手工处理（任务：{}）",
                            static_cast<int>(logon), item.id);
        }
        STM_LOG_WARN("startup", L"计划任务切换被拒绝（不支持重注册的登录类型 {}）：{}",
                     static_cast<int>(logon), item.id);
        return false;
    }
    BstrGuard xml;
    if (FAILED(def->get_XmlText(&xml.b)) || !xml.b) {
        if (err) *err = L"修改计划任务失败：导出任务 XML 备份失败，已拒绝修改：" + item.id;
        return false;
    }
    std::wstring xmlBackupPath;
    if (!WriteTaskXmlBackup(item.id, xml.b, enable, &xmlBackupPath, err)) {
        return false;
    }
    ComPtr<ITaskSettings> settings;
    if (FAILED(def->get_Settings(settings.Put())) || FAILED(settings->put_Enabled(vb))) {
        if (err) *err = L"修改计划任务失败：无法更新任务设置：" + item.id;
        return false;
    }
    ComPtr<IRegisteredTask> reReg;
    VARIANT sddl;
    ::VariantInit(&sddl);
    const HRESULT hr = folder->RegisterTaskDefinition(nameB.b, def.get(), TASK_UPDATE,
                                                      empty, empty, logon, sddl,
                                                      reReg.Put());
    if (FAILED(hr)) {
        if (hr == static_cast<HRESULT>(0x80070005u)) {  // 以 HRESULT 表示的 E_ACCESSDENIED
            if (err) {
                *err = L"修改计划任务失败：需要管理员权限（0x" +
                       stm::Fmt(L"{:08X}", static_cast<uint32_t>(hr)) + L"）";
            }
            return false;
        }
        if (err) {
            *err = L"修改计划任务失败：重新注册任务失败（0x" +
                   stm::Fmt(L"{:08X}", static_cast<uint32_t>(hr)) + L"）";
        }
        return false;
    }
    STM_LOG_INFO("startup", L"计划任务已{}（重注册路径，XML 备份 {}）：{}", enable ? L"启用" : L"禁用",
                 xmlBackupPath, item.id);
    if (err) err->clear();
    return true;
}

bool ToggleUwpState(const StartupItem& item, bool enable, std::wstring* err) {
    // id = "HKCU\<kUwpBase>\<pfn>\<taskId>\State"
    HKEY root = nullptr;
    std::wstring body;
    if (!ParseRegId(item.id, &root, &body) || root != HKEY_CURRENT_USER ||
        body.rfind(kUwpBase, 0) != 0 || body.size() <= std::size(kUwpBase) ||
        body[std::size(kUwpBase) - 1] != L'\\' || body.rfind(L"\\State") == std::wstring::npos) {
        if (err) *err = L"无法识别的 UWP 启动项标识：" + item.id;
        return false;
    }
    const std::wstring keyPath = body.substr(0, body.size() - 6);  // 去掉 "\State" 后缀
    const std::wstring valueName = L"State";

    const RegValue orig = ReadRegBytes(root, keyPath, valueName);
    HKEY raw = nullptr;
    if (::RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_SET_VALUE, &raw) != ERROR_SUCCESS) {
        if (err) *err = stm::ErrContext(L"打开 UWP 启动项状态键失败", stm::LastHr());
        return false;
    }
    RegKey key(raw);
    std::wstring backupPath;
    if (!WriteBackup(item.id, item.id, valueName, orig, enable, &backupPath, err)) {
        return false;
    }
    const DWORD next = enable ? 2 : 0;  // StartupTaskState：Enabled=2 / Disabled=0
    if (::RegSetValueExW(key.get(), valueName.c_str(), 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&next), sizeof(next)) != ERROR_SUCCESS) {
        if (err) *err = stm::ErrContext(L"写入 UWP 启动项状态失败", stm::LastHr());
        return false;
    }
    STM_LOG_INFO("startup", L"UWP 启动项：{} 已{}（备份 {}，R5 表B#2：生效性尽力而为）",
                 item.id, enable ? L"启用" : L"禁用", backupPath);
    if (err) err->clear();
    return true;
}

}  // namespace

std::vector<StartupItem> EnumStartupItems(std::wstring* err) {
    std::vector<StartupItem> out;
    std::wstring sourceErrs;
    const bool elevated = stm::IsProcessElevated();

    // shell 链接解析与任务计划需要 COM；在调用（任务）线程上
    // 按调用配对初始化/反初始化。
    ComStaGuard com;
    if (!com.Ok()) sourceErrs += L"COM 初始化失败，启动文件夹与计划任务源不可用；";

    EnumRunKey(HKEY_CURRENT_USER, kRunKey, StartupSource::RegRun, L"Run", true, &out);
    EnumRunKey(HKEY_CURRENT_USER, kRunOnceKey, StartupSource::RegRun, L"Run", true, &out);
    EnumRunKey(HKEY_LOCAL_MACHINE, kRunKey, StartupSource::RegRun, L"Run", elevated, &out);
    EnumRunKey(HKEY_LOCAL_MACHINE, kRunOnceKey, StartupSource::RegRun, L"Run", elevated, &out);
    EnumRunKey(HKEY_LOCAL_MACHINE, kRun32Key, StartupSource::RegRun32, L"Run32", elevated, &out);

    if (com.Ok()) {
        bool folderFailed = false;
        EnumStartupFolder(false, true, &out, &folderFailed);
        EnumStartupFolder(true, elevated, &out, &folderFailed);
        if (folderFailed) sourceErrs += L"启动文件夹源读取失败；";
        std::wstring taskErr;
        if (!EnumScheduledTasks(elevated, &out, &taskErr) && !taskErr.empty()) {
            sourceErrs += taskErr + L"；";
        }
    }
    EnumUwpStartup(&out);

    if (err) *err = sourceErrs;
    STM_LOG_INFO("startup", L"启动项枚举完成：{} 项（err={}）", out.size(),
                 sourceErrs.empty() ? L"无" : sourceErrs);
    return out;
}

bool SetStartupEnabled(const StartupItem& item, bool enable, std::wstring* err) {
    STM_LOG_INFO("startup", L"切换请求：{} -> {}（来源 {}）", item.id,
                 enable ? L"启用" : L"禁用", static_cast<uint32_t>(item.source));
    switch (item.source) {
        case StartupSource::RegRun:
            return ToggleRegistryApproved(item, enable, L"Run", err);
        case StartupSource::RegRun32:
            return ToggleRegistryApproved(item, enable, L"Run32", err);
        case StartupSource::StartupFolder:
            return ToggleStartupFolder(item, enable, err);
        case StartupSource::ScheduledTask:
            return ToggleScheduledTask(item, enable, err);
        case StartupSource::UwpStartupTask:
            return ToggleUwpState(item, enable, err);
    }
    if (err) *err = L"未知的启动项来源（id=" + item.id + L"）";
    return false;
}

}  // namespace stm::ops

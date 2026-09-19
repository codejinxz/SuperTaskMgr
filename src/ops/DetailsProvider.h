#pragma once
// 按需的每进程详情，经 ops JobQueue 获取并缓存（架构第 8 节）。
// API 只限 UI 线程调用（Request/Peek）；工作线程填充缓存并投递通知。
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/Jobs.h"
#include "core/Notifications.h"
#include "core/ProcData.h"
#include "ops/Signature.h"

namespace stm {
namespace ops {

enum class DetailKind : uint32_t {
    Signature = 1u << 0,  // WinVerifyTrust 结果
    CmdLine   = 1u << 1,  // 命令行（需要 VM_READ；无权限时为空且 cmdLineAvail=false）
    UserInfo  = 1u << 2,  // 所属用户名
    GuiObjects= 1u << 3,  // GDI/USER 对象计数
    Modules   = 1u << 4,  // 模块列表（提权视图可能不同；失败时如实呈现）
};

struct ProcessDetails {
    SigState sig = SigState::Unknown;
    bool sigResolved = false;
    std::wstring cmdLine;
    bool cmdLineAvail = false;
    std::wstring userName;
    bool userNameResolved = false;
    uint32_t gdiObjects = 0, userObjects = 0;
    bool guiResolved = false;
    std::vector<std::wstring> modules;
    bool modulesResolved = false;
};

class DetailsProvider {
public:
    DetailsProvider(JobQueue& jobs, NotificationQueue& notes) : jobs_(jobs), notes_(notes) {}

    // 未在处理中时为给定类别提交抓取。path 是快照中的映像路径。
    void Request(const ProcKey& key, const std::wstring& path, uint32_t kinds);
    // 仅限 UI 线程。尚未取到任何数据时返回 nullptr。
    const ProcessDetails* Peek(const ProcKey& key) const;
    void Invalidate(const ProcKey& key);
    // 把 GuiObjects 合并进 ProcInfo 供表格展示（UI 线程）。
    bool TryGetGuiObjects(const ProcKey& key, uint32_t* gdi, uint32_t* user) const;

private:
    struct Entry {
        bool pending = false;
        ProcessDetails data;
    };
    Entry* BeginEntry(const ProcKey& key);  // 需持锁
    JobQueue& jobs_;
    NotificationQueue& notes_;
    mutable std::mutex mu_;
    std::unordered_map<ProcKey, Entry> cache_;
};

}  // namespace ops
}  // namespace stm
